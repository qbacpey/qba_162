#include "filesys/directory.h"
#include "filesys/filesys.h"
#include "filesys/inode.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <list.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

/* 读写任何目录文件的时候，必须持有对应的Inode读写锁 */

/* A directory. */
struct dir {
  struct inode *inode; /* Backing store. */
  off_t pos;           /* Current position. */
};

/* A single directory entry. */
struct dir_entry {
  block_sector_t inode_sector; /* Sector number of header. */
  char name[NAME_MAX + 1];     /* Null terminated file name. */
  bool in_use;                 /* In use or free? */
};

/* 目录元数据结构体，起点必然为目录文件偏移量为0的为止 */
struct dir_meta_entry {
  block_sector_t dir_entry_ent; /* 目录中的表项数目 */
  block_sector_t curr_sector;   /* 目录Inode所在扇区 */
  block_sector_t parent_sector; /* 上一级目录Inode所在扇区 */
  bool removed;                 /* 当前目录是否被删除 */
};

/* 目录第一个表项的起始位置 */
#define SLOT_START_OFFSET sizeof(struct dir_meta_entry)

/**
 * @brief 创建一个初始就拥有`entry_cnt`个Slot的目录文件
 *
 * @param curr_sector 目录Inode所在扇区
 * @param parent_sector 上一级目录所在扇区
 * @param entry_cnt 目录初始拥有的Slot数目
 * @return true
 * @return false
 */
bool dir_create(block_sector_t curr_sector, block_sector_t parent_sector, size_t entry_cnt) {
  bool success = false;
  if (!inode_create(curr_sector, SLOT_START_OFFSET + entry_cnt * sizeof(struct dir_entry), INODE_DIR))
    goto done;

  /* 初始化目录文件 */
  struct dir_meta_entry init_meta;
  init_meta.removed = false;
  init_meta.curr_sector = curr_sector;
  init_meta.parent_sector = parent_sector;
  init_meta.dir_entry_ent = 0;
  struct inode *inode = inode_open(curr_sector);
  if (inode == NULL) {
    PANIC("Memory run out");
    goto done;
  }
  success =
      inode_write_at(inode, &init_meta, sizeof(struct dir_meta_entry), 0) == sizeof(struct dir_meta_entry);
  inode_close(inode);
done:
  return success;
}

/* Opens and returns the directory for the given INODE, of which
   it takes ownership.  Returns a null pointer on failure. */
struct dir *dir_open(struct inode *inode) {
  struct dir *dir = calloc(1, sizeof *dir);
  if (inode != NULL && dir != NULL) {
    dir->inode = inode;
    dir->pos = 0;
    return dir;
  } else {
    inode_close(inode);
    free(dir);
    return NULL;
  }
}

/* Opens the root directory and returns a directory for it.
   Return true if successful, false on failure. */
struct dir *dir_open_root(void) { return dir_open(inode_open(ROOT_DIR_SECTOR)); }

/* Opens and returns a new directory for the same inode as DIR.
   Returns a null pointer on failure. */
struct dir *dir_reopen(struct dir *dir) { return dir_open(inode_reopen(dir->inode)); }

/* Destroys DIR and frees associated resources. */
void dir_close(struct dir *dir) {
  if (dir != NULL) {
    inode_close(dir->inode);
    free(dir);
  }
}

/* Returns the inode encapsulated by DIR. */
struct inode *dir_get_inode(struct dir *dir) { return dir->inode; }

/**
 * @brief Searches DIR for a file with the given NAME. If successful, returns true, sets *EP to the directory
 * entry if EP is non-null, and sets *OFSP to the byte offset of the directory entry if OFSP is non-null.
 * otherwise, returns false and ignores EP and OFSP.
 *
 * @param dir 待查找目录
 * @param name 目标文件名称
 * @param ep entry position, 如果不为NULL，那么查找到目录的表项会被读取到这个位置
 * @param ofsp offset position, 如果不为NULL，那么目录表项在目录文件中的偏移量会被读取到这个位置
 * @return true
 * @return false
 */
static bool lookup(const struct dir *dir, const char *name, struct dir_entry *ep, off_t *ofsp) {
  struct dir_entry e;
  size_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);
  /* 处理不是`..`和`.`的情况 */
  for (ofs = SLOT_START_OFFSET; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (e.in_use && !strcmp(name, e.name)) {
      if (ep != NULL)
        *ep = e;
      if (ofsp != NULL)
        *ofsp = ofs;
      return true;
    }
  return false;
}

/* Searches DIR for a file with the given NAME
   and returns true if one exists, false otherwise.
   On success, sets *INODE to an inode for the file, otherwise to
   a null pointer.  The caller must close *INODE. */
bool dir_lookup(const struct dir *dir, const char *name, struct inode **inode) {
  struct dir_entry e;
  struct dir_meta_entry init_meta;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  // 确保在文件被打开之前，目录仍处于未被移除状态
  inode_lock_shared(dir_get_inode((struct dir *)dir));

  if (inode_read_at(dir->inode, &init_meta, sizeof init_meta, 0) != sizeof init_meta)
    PANIC("Inode read error");

  // 检查目录是否已经被移除
  if (init_meta.removed) {
    *inode = NULL;
  } else {
    // 考虑到文件名可能以..开始，因此只有name完全相当的时候才认为是两个特殊路径
    if (strcmp(name, "..") == 0)
      *inode = inode_open(init_meta.parent_sector);
    else if (strcmp(name, ".") == 0)
      *inode = inode_reopen(dir->inode);
    else if (lookup(dir, name, &e, NULL))
      *inode = inode_open(e.inode_sector);
    else
      *inode = NULL;
  }

  inode_unlock_shared(dir_get_inode((struct dir *)dir));

  return *inode != NULL;
}

/* Adds a file named NAME to DIR, which must not already contain a
   file by that name.  The file's inode is in sector
   INODE_SECTOR.

   检查目录是否被移除之前需要获取写者锁

   Returns true if successful, false on failure.
   Fails if NAME is invalid (i.e. too long) or a disk or memory
   error occurs. */
bool dir_add(struct dir *dir, const char *name, block_sector_t inode_sector) {
  bool success = false;
  struct dir_meta_entry dir_meta;

  // 确保在递增目录表项数目之前，目录仍处于未被移除状态
  inode_lock(dir->inode);

  // 读取目录元数据
  if (inode_read_at(dir->inode, &dir_meta, sizeof dir_meta, 0) != sizeof dir_meta)
    goto done;

  // 检查目录是否已经被移除，没有移除的话就递增表项数目
  if (!dir_meta.removed) {
    dir_meta.dir_entry_ent++;
    // 将更改写入到目录文件中
    if (inode_write_at(dir->inode, &dir_meta, sizeof dir_meta, 0) != sizeof dir_meta)
      goto done;
  } else {
    goto done;
  }

  struct dir_entry e;
  off_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* Check NAME for validity. */
  if (*name == '\0' || strlen(name) > NAME_MAX)
    return false;

  /* Check that NAME is not in use. */
  if (lookup(dir, name, NULL, NULL))
    goto done;

  /* Set OFS to offset of free slot.
     If there are no free slots, then it will be set to the
     current end-of-file.

     inode_read_at() will only return a short read at end of file.
     Otherwise, we'd need to verify that we didn't get a short
     read due to something intermittent such as low memory. */
  for (ofs = SLOT_START_OFFSET; inode_read_at(dir->inode, &e, sizeof e, ofs) == sizeof e; ofs += sizeof e)
    if (!e.in_use)
      break;

  /* Write slot. */
  e.in_use = true;
  strlcpy(e.name, name, sizeof e.name);
  e.inode_sector = inode_sector;
  success = inode_write_at(dir->inode, &e, sizeof e, ofs) == sizeof e;

done:
  inode_unlock(dir->inode);
  return success;
}

/* Removes any entry for NAME in DIR.
   Returns true if successful, false on failure,
   which occurs only if there is no file with the given NAME. */
bool dir_remove(struct dir *dir, const char *name) {
  struct dir_entry e;
  struct inode *inode = NULL;
  bool success = false;
  off_t ofs;

  ASSERT(dir != NULL);
  ASSERT(name != NULL);

  /* 修改目录表项数目时，需持有目录读写锁 */
  inode_lock(dir->inode);
  /* Find directory entry. */
  if (!lookup(dir, name, &e, &ofs))
    goto done;

  /* Open inode. */
  inode = inode_open(e.inode_sector);
  if (inode == NULL)
    goto done;

  /* 如果待删除文件类型是目录的话，需要检查可否将其删除 */
  if (inode_type(inode) == INODE_DIR) {
    struct dir_meta_entry e_meta;
    if (inode_read_at(inode, &e_meta, sizeof e_meta, 0) != sizeof e_meta)
      goto done;
    /* 只有在目录中表项数目为0时才删除目录 */
    if (e_meta.dir_entry_ent == 0 && e_meta.removed != true) {
      /* 将removed置为true，避免重复删除 */
      e_meta.removed = true;

      if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
        goto done;
    } else {
      /* 如果文件在此之前已经被移除，也算是执行成功 */
      success = e_meta.removed == true;
      goto done;
    }
  }

  /* Erase directory entry. */
  e.in_use = false;
  if (inode_write_at(dir->inode, &e, sizeof e, ofs) != sizeof e)
    goto done;

  /* Remove inode. */
  inode_remove(inode);

  /* 当前目录 */
  struct dir_meta_entry dir_meta;

  if (inode_read_at(dir->inode, &dir_meta, sizeof dir_meta, 0) != sizeof dir_meta)
    PANIC("Inode read error");

  /* 递减目录表项数目 */
  dir_meta.dir_entry_ent--;
  success = inode_write_at(dir->inode, &dir_meta, sizeof dir_meta, 0) == sizeof dir_meta;
done:
  inode_unlock(dir->inode);
  inode_close(inode);
  return success;
}

/* Reads the next directory entry in DIR and stores the name in
   NAME.  Returns true if successful, false if the directory
   contains no more entries. */
bool dir_readdir(struct dir *dir, char name[NAME_MAX + 1]) {
  struct dir_entry e;
  inode_lock_shared(dir_get_inode(dir));
  while (inode_read_at(dir->inode, &e, sizeof e, dir->pos) == sizeof e) {
    dir->pos += sizeof e;
    if (e.in_use) {
      strlcpy(name, e.name, NAME_MAX + 1);
      return true;
    }
  }
  inode_unlock_shared(dir_get_inode(dir));
  return false;
}
