#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/**
 * @brief 用于表示struct file_in_desc中文件的类型为普通文件还是目录文件
 *
 */
enum file_type { FILE, DIR };

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */
#define SPARSE_FILE_SECTOR 2 /* Sparse file sector (fill with zero) */

/* Block device that contains the file system. */
extern struct block* fs_device;

void filesys_init(bool format);
void filesys_done(void);
bool filesys_create(const char* name, off_t initial_size);
struct file* filesys_open(const char* name);
bool filesys_remove(const char* name);

#endif /* filesys/filesys.h */
