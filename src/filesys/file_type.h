#ifndef FILESYS_INODE_FILE_TYPE
#define FILESYS_INODE_FILE_TYPE
/**
 * @brief 用于表示struct file_in_desc中文件的类型为普通文件还是目录文件
 *
 */
enum file_type { INODE_FILE, INODE_DIR };
#endif /* filesys/file_type.h */
