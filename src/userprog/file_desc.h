#ifndef USERPROG_FILE_DESC_H
#define USERPROG_FILE_DESC_H

#include "userprog/process.h"

bool file_desc_create(const char *name, off_t initial_size, struct process *pcb);
bool file_desc_remove(const char *name, struct process *);
bool file_desc_chdir(const char *dir, struct process *);
bool file_desc_mkdir(const char *dir, struct process *);
bool file_desc_readdir(uint32_t fd, char *name, struct process *);
bool file_desc_isdir(uint32_t fd, struct process *);
int file_desc_inumber(uint32_t fd, struct process *);
int file_desc_open(const char *name, struct process *);
bool file_desc_close(uint32_t fd, struct process *);
void file_desc_destroy(struct process *);
int file_desc_size(uint32_t fd, struct process *);
int file_desc_tell(uint32_t fd, struct process *);
bool file_desc_seek(uint32_t fd, unsigned pos, struct process *);
int file_desc_read(uint32_t fd,void *buffer, unsigned size, struct process *);
int file_desc_write(uint32_t fd,const void *buffer, unsigned size, struct process *);

#endif /* userprog/file_desc.h */
