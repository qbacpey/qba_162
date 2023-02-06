#ifndef FILESYS_FILESYS_H
#define FILESYS_FILESYS_H

#include <stdbool.h>
#include "filesys/off_t.h"

/* Sectors of system file inodes. */
#define FREE_MAP_SECTOR 0 /* Free map file inode sector. */
#define ROOT_DIR_SECTOR 1 /* Root directory file inode sector. */
#define SPARSE_FILE_SECTOR 2 /* Sparse file sector (fill with zero) */

/* Block device that contains the file system. */
extern struct block* fs_device;

void filesys_init(bool format);
void filesys_done(void);

bool filesys_create(const char* path, off_t initial_size);
bool filesys_mkdir(const char* path);

struct file* filesys_open_file(const char* path);
struct dir* filesys_open_dir(const char* path);

bool filesys_remove(const char* path);

#endif /* filesys/filesys.h */
