
#include "userprog/file_desc.h"

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
 * @return uint32_t 文件描述符，失败时返回-1
 */
uint32_t file_desc_open(const char *name, struct process *pcb) { return 0; }

/**
 * @brief 关闭`pcb`文件打开表中文件描述符为`fd`的文件
 *
 * @param fd
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_close(uint32_t fd, struct process *pcb) { return false; }

/**
 * @brief 移除名为`name`的文件
 *
 * @param name 文件路径，既可以是绝对路径，也可以是相对路径
 * @param pcb
 * @return true
 * @return false
 */
bool file_desc_remove(const char *name, struct process *pcb) { return false; }

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
int file_desc_size(uint32_t fd, struct process *pcb) { return -1; }

/**
 * @brief Returns the position of the next byte to be read or written in open ﬁle fd, expressed in bytes from
 * the beginning of the ﬁle.
 *
 * @param fd
 * @param pcb
 * @return int 如果不是普通文件返回-1
 */
int file_desc_tell(uint32_t fd, struct process *pcb) { return -1; }

/**
 * @brief Changes the next byte to be read or written in open ﬁle fd to position, expressed in bytes from the
 * beginning of the ﬁle. Thus, a position of 0 is the ﬁle’s start.
 *
 * @param fd
 * @param pcb
 * @return int 如果不是普通文件返回-1
 */
int file_desc_seek(uint32_t fd, struct process *pcb) { return -1; }

/**
 * @brief 读取`pcb`文件打开表中的普通文件`fd`
 *
 * @param fd
 * @param pcb
 * @return int 实际读取到的Byte数目，如果不是普通文件返回-1
 */
int file_desc_read(uint32_t fd, struct process *pcb) { return -1; }

/**
 * @brief 写入`pcb`文件打开表中的普通文件`fd`
 *
 * @param fd
 * @param pcb
 * @return int 实际写入到的Byte数目，如果不是普通文件返回-1
 */
int file_desc_write(uint32_t fd, struct process *pcb) { return -1; }
