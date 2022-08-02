#include "filesys/filesys.h"
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "filesys/file.h"
#include "filesys/free-map.h"
#include "filesys/inode.h"
#include "filesys/directory.h"

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
 * 
 */

/* Partition that contains the file system. */
struct block* fs_device;

static void do_format(void);

/* Initializes the file system module.
   If FORMAT is true, reformats the file system. */
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
void filesys_done(void) { free_map_close(); }

/* Creates a file named NAME with the given INITIAL_SIZE.
   Returns true if successful, false otherwise.
   Fails if a file named NAME already exists,
   or if internal memory allocation fails. */
bool filesys_create(const char* name, off_t initial_size) {
  block_sector_t inode_sector = 0;
  struct dir* dir = dir_open_root();
  bool success = (dir != NULL && free_map_allocate(1, &inode_sector) &&
                  inode_create(inode_sector, initial_size) && dir_add(dir, name, inode_sector));
  if (!success && inode_sector != 0)
    free_map_release(inode_sector, 1);
  dir_close(dir);

  return success;
}

/* Opens the file with the given NAME.
   Returns the new file if successful or a null pointer
   otherwise.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
struct file* filesys_open(const char* name) {
  struct dir* dir = dir_open_root();
  struct inode* inode = NULL;

  if (dir != NULL)
    dir_lookup(dir, name, &inode);
  dir_close(dir);

  return file_open(inode);
}

/* Deletes the file named NAME.
   Returns true if successful, false on failure.
   Fails if no file named NAME exists,
   or if an internal memory allocation fails. */
bool filesys_remove(const char* name) {
  struct dir* dir = dir_open_root();
  bool success = dir != NULL && dir_remove(dir, name);
  dir_close(dir);

  return success;
}

/* Formats the file system. */
static void do_format(void) {
  printf("Formatting file system...");
  free_map_create();
  if (!dir_create(ROOT_DIR_SECTOR, 16))
    PANIC("root directory creation failed");
  free_map_close();
  printf("done.\n");
}
