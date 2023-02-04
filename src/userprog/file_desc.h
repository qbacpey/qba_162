#ifndef USERPROG_FILE_DESC_H
#define USERPROG_FILE_DESC_H

#include "userprog/process.h"

/**
 * @brief 用于表示struct file_in_desc中文件的类型为普通文件还是目录文件
 *
 */
enum file_type { FILE, DIR };

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

bool file_desc_create(const char *name, off_t initial_size, struct process *pcb);
bool file_desc_remove(const char *name, struct process *);
bool file_desc_chdir(const char *dir, struct process *);
bool file_desc_mkdir(const char *dir, struct process *);
bool file_desc_readdir(uint32_t fd, char *name, struct process *);
bool file_desc_isdir(uint32_t fd, struct process *);
int file_desc_inumber(uint32_t fd, struct process *);
uint32_t file_desc_open(const char *name, struct process *);
bool file_desc_close(uint32_t fd, struct process *);
int file_desc_size(uint32_t fd, struct process *);
int file_desc_tell(uint32_t fd, struct process *);
int file_desc_seek(uint32_t fd, struct process *);
int file_desc_read(uint32_t fd, struct process *);
int file_desc_write(uint32_t fd, struct process *);

#endif /* userprog/file_desc.h */
