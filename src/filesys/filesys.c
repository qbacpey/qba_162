#include "filesys/filesys.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/file_type.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "userprog/process.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>

/*
 * 浅浅写下Poj1中要用到文件系统的什么功能：
 * 0. 总的要点：文件系统中的任何地方都不能修改，只能调API
 *             注意确保进程间的同步问题
 *
 * 1. 创建文件：这部分基本上只需要用到本文件中的内容
 *    具体来说就是下面的filesys_create函数
 *    file.c文件中的内容则不怎么需要使用和修改
 *
 * 2. 开启文件、删除文件：这部分内容也是仅使用到本文件中的API即可
 *
 * 3. 读取文件、写入文件、关闭文件：这部分内容需要使用file.c中的API
 *
 * 4. 可重入性：这部分的主题就是确保进程被加载到内核中之后
 *    其可执行文件不会被以任何方式进行修改
 *    文档中给出的方法是使用file_deny_write和file_allow_write实现
 *
 * 5. 同步：确保同一时间只能有一个进程在执行系统调用的相关函数
 *    直接使用线程库里的实现算了。考虑到不能修改这里的代码
 *    因此从系统调用实现那边入手
 *
 * 6. 调试：一方面这个文件引入了debug.h, 实际上就是能直接在代码中使用调试API的意思
 *    另一方面，fsutil文件的函数也可以用于辅助调试
 *
 *  额定任务完成，还想到了一些关于文件系统的东西，顺便就写在这了

    为确保此系统的系统调用操作和UNXI系统下的操作一样，需要在文件系统实现
    方面做文章，即文件描述符表和文件描述符结构体的创建和维护：

    1.首先需要在进程PCB中创建一个新的元素，即所谓的文件描述符表。

      该表需要使用链表数据结构进行维护，链表元素除了含有必须的list_elem之外，
      还需要包含两个元素：int 文件描述符和 file* 文件指针。

      初始状态下，链表中就包含三个元素：0——stdin，1——stdout，2——stderr

    2.文件描述符的维护：考虑到0，1，2都各自有自己的含义，因此文件描述符从3
      开始递增。

      同时，为了性能方面的考虑，往后如果要往链表中放元素的话，直接
      跳到最后一个元素，从他的标识符开始递增，而不再复用之前已经用过的标识符

    3.创建文件：首先需要明确的是以下几点：

      1.文件可以多次打开，文件系统中的一个文件能对应多个进程中的文件描述符
        也能对应同一个进程中的多个文件描述符

      2.单个文件的不同的文件描述符不会在同一时间被关闭，同时他们也不会共享
        文件位置标识符

      3.子进程不会继承父进程的文件标识符

      4.创建文件不会打开该文件

    下面是针对各问题的解决方案：
      1.考虑到就算是针对同一个文件，该文件不同的文件描述符都拥有各自的位置信息
        因此在创建文件这块，实际需要做的操作仅仅只不过是跳到链表的尾部，递增标识符
        随后调用文建系统即可，并不需要看链表中是否有文件已经打开（但是文件系统好像有一个
        叫做reopen的东西，一会要去看一下）

      2.同步方面的问题，也就是一个进程描述符写入数据了，另一个文件描述符应该能看到才是
        这里如果文件系统中没有实现相关的功能的话，可能就需要每次写入之后手动刷新了。
        毕竟这个问题一方面关系到同步，另一方面读写也会被影响到

      3.父子进程之间可能存在的问题：考虑到进程间文件描述符不共享，因此这个问题无需费心

 */

/* Partition that contains the file system. */
struct block *fs_device;

static void do_format(void);
static int get_next_part(char[NAME_MAX + 1], const char **);
static bool lookup(const char *, struct inode **);
static bool parse_path(const char *, const char **, struct dir **);
/* Initializes the file system module.
   If FORMAT is true, reformats the file system.
   可以等价地将格式化理解为初始化 */
void filesys_init(bool format) {
  fs_device = block_get_role(BLOCK_FILESYS);
  if (fs_device == NULL)
    PANIC("No file system device found, can't initialize file system.");

  inode_init();
  free_map_init();

  if (format)
    do_format();

  free_map_open();
}

/* Shuts down the file system module, writing any unwritten data
   to disk. */
void filesys_done(void) {
  free_map_close();
  flush_buffer_cache();
}

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char *path, off_t initial_size) {
  bool success = false;
  /* 文件名称 */
  const char *name = NULL;
  /* 新文件的上一级目录 */
  struct dir *dir = NULL;

  if (!parse_path(path, &name, &dir))
    goto done;

  /* 将新文件添加到目录中 */
  block_sector_t inode_sector = 0;

  success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
             inode_create(inode_sector, initial_size, INODE_FILE) && dir_add(dir, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
  dir_close(dir);
done:
  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file *filesys_open_file(const char *path) {
  struct inode *inode = NULL;
  /* 找到`path`所指定的文件 */
  if (!lookup(path, &inode)) {
    return NULL;
  }

  /* 检查`path`是不是普通文件，如果不是，直接返回 */
  if (inode_type(inode) != INODE_FILE) {
    inode_close(inode);
    return NULL;
  }

  return file_open(inode);
}

/**
 * @brief 打开名为`path`的目录文件
 *
 * @param path
 * @return struct dir*
 */
struct dir *filesys_open_dir(const char *path) {
  struct inode *inode = NULL;
  /* 找到`path`所指定的目录 */
  if (!lookup(path, &inode)) {
    return NULL;
  }

  /* 检查`path`是不是目录文件，如果不是，直接返回 */
  if (inode_type(inode) != INODE_DIR) {
    inode_close(inode);
    return NULL;
  }
  return dir_open(inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char *path) {
  struct dir *dir = dir_open_root();
  bool success = dir != NULL && dir_remove(dir, path);
  dir_close(dir);

  return success;
}

/**
 * @brief 查找路径`path`指定的文件是否存在，如果存在设置`inode`为指向该文件的指针，
 * 如果不存在设置其为NULL
 *
 * @note Caller需要关闭inode
 *
 * @param path 文件路径，可以是绝对路径，也可以是相对路径
 * @param struct inode **inode
 * @return bool 文件是否存在
 */
static bool lookup(const char *path, struct inode **inode) {
  char part[NAME_MAX + 1] = {'\0'};
  const char *_name = path;
  const char **srcp = &_name;
  bool look_up_result = false;
  // 需在此Inode中查找指定名称的目录表项
  struct inode *prev_inode = NULL;
  // 需在此目录中查找指定名称的目录表项
  struct dir *curr_dir = NULL;
  // 目标名称的Inode
  struct inode *curr_inode = NULL;
  // 返回结果
  *inode = NULL;

  switch (get_next_part(part, srcp)) {
  case 1:
    /* success */
    if (path[0] == '/') {
      /* 路径以`/`起头（绝对路径），从根目录开始路径处理处理 */
      curr_dir = dir_open_root();
    } else {
      /* 路径并非以`/`起头（相对路径或来自于`fsutil`），检查`tcb->pcb`
         因为`dir_lookup`可以很好地应对目录项是`..`或者`.`的情况，
         寻找某个目录的`.`以及`..`表项。同时"../"也可以被理解为寻找某个目录的
         上一级目录，因此可以将`curr_dir`设定为工作目录之后再在其中寻找目录
       */
      curr_dir = dir_reopen(get_working_dir());
    }

    if (curr_dir == NULL) {
      goto err_dir_open_fail;
    }
    // 打开prev_dir中名为part的inode
    look_up_result = dir_lookup(curr_dir, part, &curr_inode);
    dir_close(curr_dir);
    curr_dir = NULL;
    if (!look_up_result) {
      goto err_path_entry_not_exist;
    }
    break;
  case 0:
    goto done;
  case -1:
    goto err_too_long_file_name;
  default:
    goto err_get_next_part_return_val;
  }
  while (true) {
    // 在上一次的目录项的Inode中查找本次目录项的Inode
    prev_inode = curr_inode;
    curr_inode = NULL;
    switch (get_next_part(part, srcp)) {
    case 1:
      /* 路径中依然存在目录项 */
      if (inode_type(prev_inode) == INODE_FILE) {
        // 上一次目录项类型为普通文件，但路径中依然存在目录项，说明路径中间出现了普通文件
        inode_close(prev_inode);
        goto err_middle_path_entry_is_regular_file;
      } else {
        // 上一次的目录项类型为目录，将其Inode打开为目录
        curr_dir = dir_open(prev_inode);
        if (curr_dir == NULL) {
          inode_close(prev_inode);
          goto err_dir_open_fail;
        }
      }

      look_up_result = dir_lookup(curr_dir, part, &curr_inode);
      dir_close(curr_dir);
      curr_dir = NULL;
      /* 目录中不存在目录项 */
      if (!look_up_result) {
        goto err_path_entry_not_exist;
      }
      break;
    case 0:
      /* 路径中无目录项，上一轮循环读取到的Inode即为路径最后目录项对应的Inode */
      ASSERT(prev_inode != NULL);
      *inode = prev_inode;
      goto done;
    case -1:
      goto err_too_long_file_name;
    default:
      goto err_get_next_part_return_val;
    }
  }

done:
  return *inode != NULL;
err_dir_open_fail:
  /* 目录打开失败 */
  PANIC("dir open fail");
  return false;
err_path_entry_not_exist:
  /* 路径中有一段不存在 */
  return false;
err_middle_path_entry_is_regular_file:
  /* 路径中间有一段是普通文件 */
  return false;
err_get_next_part_return_val:
  PANIC("err_get_next_part_return_val");
  return false;
err_too_long_file_name:
  PANIC("too-long file name");
  return false;
}

/* Extracts a file name part from *SRCP into PART, and updates *SRCP so that the
next call will return the next file name part. Returns 1 if successful, 0 at
end of string, -1 for a too-long file name part. */
static int get_next_part(char part[NAME_MAX + 1], const char **srcp) {
  const char *src = *srcp;
  char *dst = part;

  /* Skip leading slashes. If it's all slashes, we're done. */
  while (*src == '/')
    src++;
  if (*src == '\0')
    return 0;

  /* Copy up to NAME_MAX character from SRC to DST. Add null terminator. */
  while (*src != '/' && *src != '\0') {
    if (dst < part + NAME_MAX)
      *dst++ = *src;
    else
      return -1;
    src++;
  }
  *dst = '\0';

  /* Advance source pointer. */
  *srcp = src;
  return 1;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}

/**
 * @brief
 * 解析`path`，如果成功返回`true`，同时将`name`设置为路径末尾的文件名，将`dir`设置为指向文件上一级目录的指针
 *
 * @param path
 * @param name
 * @param dir
 * @return true
 * @return false
 */
static bool parse_path(const char *path, const char **name, struct dir **dir) {
  ASSERT(path != NULL);
  ASSERT(name != NULL);
  ASSERT(dir != NULL);
  size_t path_len = strlen(path);
  if (*path == '\0' || path[path_len] == '/')
    return false;

  /* 计算目录路径并复制 */
  char *last_slash = strrchr(path, '/');
  if (last_slash != NULL) {
    /* path中有 / 存在 */

    char *dir_path = malloc(path_len);
    /* 复制目录路径 */
    memcpy(dir_path, path, last_slash - path);
    struct inode *dir_inode = NULL;

    /* 查找目录文件的Inode */
    if (!lookup(dir_path, &dir_inode) || inode_type(dir_inode) == INODE_FILE) {
      free(dir_path);
      return false;
    }

    ASSERT(dir_inode != NULL);

    *name = last_slash + 1;
    *dir = dir_open(dir_inode);
    free(dir_path);
  } else {
    /* path只有一个文件名，需使用当前工作目录 */
    *name = path;
    /* 注意套上一个dir_reopen */
    *dir = dir_reopen(get_working_dir());
  }
  return true;
}