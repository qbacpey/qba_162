
#include "userprog/file_desc.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/malloc.h"

/**
 * @brief 用于保存文件描述符的文件指针，带有类型字段
 *
 */
struct file_in_desc {
  union {
    struct file *file; /* 文件指针，务必注意释放问题 */
    struct dir *dir;   /* 目录指针，务必注意释放问题 */
  } file;
  enum file_type type; /* file的类型 */
};

/**
 * @brief 文件描述符表元素
 *
 * 开启即创建，关闭即释放，创建的时候使用files_next_desc作为本文件的文件描述符，随后将这个值进行递增处理
 *
 */
struct file_desc {
  uint32_t file_desc;       /* 文件描述符，从3开始 */
  struct file_in_desc file; /* 文件指针 */
  struct list_elem elem;
};

/* 文件描述符表操作接口 */

/**
 * @brief 将`pcb`工作目录修改为`dir`，将其添加到进程文件打开表的同时关闭旧的文件打开表
 *
 * @param dir 目录路径，既可以是绝对路径，也可以是相对路径
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_chdir(const char *dir, struct process *pcb) { return false; }

/**
 * @brief 创建名为`dir`的目录文件，既可以在`pcb`工作目录中创建，也可以在其他位置创建
 *
 * @param dir 目录路径，既可以是绝对路径，也可以是相对路径
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_mkdir(const char *dir, struct process *pcb) { return false; }

/**
 * @brief 读取`fd`目录中的下一个目录表项，将文件名称保存到`name`中
 *
 * @param fd 文件描述符，必须是目录文件
 * @param name
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_readdir(uint32_t fd, char *name, struct process *pcb) { return false; }

/**
 * @brief 创建名为`name`、初始大小为`initial_size`普通文件
 *
 * @param name 文件路径，既可以是绝对路径，也可以是相对路径
 * @param initial_size
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_create(const char *name, off_t initial_size, struct process *pcb) { return false; }

/**
 * @brief 打开名为`name`的文件，将其加载到进程文件打开表中，返回其文件描述符
 *
 * @param name 文件路径，既可以是绝对路径，也可以是相对路径
 * @param pcb
 * @return int 文件描述符，失败时返回-1
 */
int file_desc_open(const char *name, struct process *pcb) {
  struct list *files_tab = &pcb->files_tab;
  struct lock *files_tab_lock = &pcb->files_lock;
  void *new_file = NULL;

  int result = -1;
  /* TODO 检查文件类型 */
  enum file_type type = FILE;
  /* 根据文件类型，调用不同的文件系统接口打开文件 */
  if (type == FILE) {
    new_file = filesys_open(name);
  } else {
    // TODO new_dir = filesys_open_dir(name);
    PANIC("Error");
  }

  if (new_file != NULL) {
    /* 打开文件成功 */
    ASSERT(pcb->files_next_desc >= 3);

    /* 申请并分配新的文件描述符 */
    struct file_desc *new_file_desc = malloc(sizeof(struct file_desc));

    /* 按照文件类型，配置该结构体并压入文件描述符列表 */
    new_file_desc->file.type = type;
    if (type == FILE) {
      new_file_desc->file.file.file = (struct file *)new_file;
    } else {
      // TODO new_file_desc->file.file.dir = (struct dir *)new_file;
      PANIC("Error");
    }

    lock_acquire(files_tab_lock);
    new_file_desc->file_desc = pcb->files_next_desc;
    list_push_front(files_tab, &new_file_desc->elem);
    pcb->files_next_desc++;
    lock_release(files_tab_lock);

    result = new_file_desc->file_desc;
  }
  return result;
}

/**
 * @brief 关闭`pcb`文件打开表中文件描述符为`fd`的文件
 *
 * @param fd
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_close(uint32_t fd, struct process *pcb) {
  bool result = false;

  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;
  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      if (pos->file.type == FILE) {
        file_close(pos->file.file.file);
      } else {
        // TODO dir_close(pos->file.file.dir);
        PANIC("Error");
      }
      result = true;
      list_remove(&pos->elem);
      free(pos);
      break;
    }
  }
  lock_release(files_tab_lock);

  return result;
}

/**
 * @brief 释放`pcb`文件描述符表中的所有文件
 *
 * @param pcb
 */
void file_desc_destroy(struct process *pcb_to_free) {
  /* 释放文件描述符表，必须使用 NULL 进行初始化 */
  struct file_desc *file_pos = NULL;
  list_clean_each(file_pos, &pcb_to_free->files_tab, elem) {
    if (file_pos->file.type == FILE) {
      file_close(file_pos->file.file.file);
    } else {
      // TODO dir_close(file_pos->file.file.dir);
      PANIC("Error");
    }
    free(file_pos);
  }
  // 如果想要使用这个宏遍历并清除链表元素的话，尾部的if语句是必要的
  if (file_pos != NULL) {
    if (file_pos->file.type == FILE) {
      file_close(file_pos->file.file.file);
    } else {
      // TODO dir_close(file_pos->file.file.dir);
      PANIC("Error");
    }
    free(file_pos);
  }
}

/**
 * @brief 移除名为`name`的文件
 *
 * @param name 文件路径，既可以是绝对路径，也可以是相对路径
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_remove(const char *name, struct process *pcb) { return filesys_remove(name); }

/**
 * @brief Returns true if fd represents a directory, false if it represents an ordinary ﬁle.
 *
 * @param fd
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_isdir(uint32_t fd, struct process *pcb) { return false; }

/**
 * @brief Returns the inode number of the inode associated with fd,
 * which may represent an ordinary ﬁle or a directory.
 *
 * @param fd
 * @param pcb
 * @return int
 */
int file_desc_inumber(uint32_t fd, struct process *pcb) { return -1; }

/**
 * @brief 获取`fd`的大小
 *
 * @param fd
 * @param pcb
 * @return int 文件大小，如果不是普通文件返回-1
 */
int file_desc_size(uint32_t fd, struct process *pcb) {
  int size = -1;
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      if (pos->file.type == FILE) {
        size = (int)file_length(pos->file.file.file);
      } else {
        size = -1;
        PANIC("Error");
      }
      break;
    }
  }
  lock_release(files_tab_lock);

  return size;
}

/**
 * @brief Returns the position of the next byte to be read or written in open ﬁle fd, expressed in bytes from
 * the beginning of the ﬁle.
 *
 * @param fd
 * @param pcb
 * @return int 如果不是普通文件返回-1
 */
int file_desc_tell(uint32_t fd, struct process *pcb) {
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;
  int size = -1;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      if (pos->file.type == FILE) {
        size = (int)file_tell(pos->file.file.file);
      } else {
        size = -1;
        PANIC("Error");
      }
      break;
    }
  }
  lock_release(files_tab_lock);

  return size;
}

/**
 * @brief Changes the next byte to be read or written in open ﬁle fd to position, expressed in bytes from the
 * beginning of the ﬁle. Thus, a position of 0 is the ﬁle’s start.
 *
 * @param fd
 * @param pcb
 * @return int 如果不是普通文件返回-1
 */
bool file_desc_seek(uint32_t fd, unsigned position, struct process *pcb) {
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;
  bool result = false;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      if (pos->file.type == FILE) {
        result = true;
        file_seek(pos->file.file.file, position);
      } else {
        result = false;
        PANIC("Error");
      }
      break;
    }
  }
  lock_release(files_tab_lock);
  return result;
}

/**
 * @brief 读取`pcb`文件打开表中的普通文件`fd`
 *
 * @param fd
 * @param pcb
 * @return int 实际读取到的Byte数目，如果不是普通文件返回-1
 */
int file_desc_read(uint32_t fd, void *buffer, unsigned size, struct process *pcb) {

  uint32_t off = -1;
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      if (pos->file.type == FILE) {
        off = file_read(pos->file.file.file, buffer, size);
      } else {
        off = -1;
        PANIC("Error");
      }
      break;
    }
  }
  lock_release(files_tab_lock);

  return off;
}

/**
 * @brief 写入`pcb`文件打开表中的普通文件`fd`
 *
 * @param fd
 * @param pcb
 * @return int 实际写入到的Byte数目，如果不是普通文件返回-1
 */
int file_desc_write(uint32_t fd, const void *buffer, unsigned size, struct process *pcb) {
  uint32_t off = -1;
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      if (pos->file.type == FILE) {
        off = file_write(pos->file.file.file, buffer, size);
      } else {
        off = -1;
        PANIC("Error");
      }
    }
  }
  lock_release(files_tab_lock);

  return off;
}
