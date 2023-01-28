#include "filesys/inode.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include <debug.h>
#include <list.h>
#include <synch.h>
#include <round.h>
#include <string.h>

/* Identifies an inode. */
#define INODE_MAGIC 0x494e4f44

/* Cache Buffer Block Count */
#define BUFFER_BLOCK_COUNT 64

/* 一个扇区中最多可容纳的指针数目 */
#define POINTER_IN_SECTOR 128

/* Active Block Queue Length */
#define AT_LENGTH 32

/* Second Chance Queue Length */
#define SC_LENGTH 32

/* 1.6 TB */
#define INIT_SECTOR 0xCCCCCCCC

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  uint32_t unused[125]; /* Not used. */
};

/* Returns the number of sectors to allocate for an inode SIZE
   bytes long. */
static inline size_t bytes_to_sectors(off_t size) { return DIV_ROUND_UP(size, BLOCK_SECTOR_SIZE); }

/* 绿色队列，用于保存活跃Block，最大长度为32 */
static struct list active_blocks;

/* 黄色队列，用于保存Evict候选Block，最大长度为32 */
static struct list sc_blocks;

/* Cache Buffer 数组，指向一个长度为 64 * 512 Byte 的数组 */
uint8_t *cache_buffer;

/* 队列共用锁 */
static struct lock queue_lock;

/* In-memory inode. */
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  struct inode_disk data; /* Inode content. */
};

/* Cache Buffer各Block的元数据，用于维护SC队列 */
typedef struct meta {
  struct lock meta_lock;     /* 元数据锁 */
  block_sector_t sector;     /* Block对应扇区的下标 */
  int worker_cnt;            /* 等待或正在使用Block的线程数 */
  bool dirty;                /* Dirty Bit */
  struct rw_lock block_lock; /* Block的RW锁 */
  uint8_t *block;            /* 元数据对应的Block */
  struct list_elem elem;     /* 必然位于SC或者Active中 */
} meta_t;

/* 64个meta的位置 */
static meta_t *meta_buffer;

/* TODO 实现需修改，估计需要加一段“从Buffer Cache中获取Inode”的代码

  Returns the block device sector that contains byte offset POS
   within INODE.
   计算POS在文件INODE中位于哪一个扇区中，返回该扇区下标

   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);
  if (pos < inode->data.length)
    return inode->data.start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Initializes the inode module. */
void inode_init(void) {
  list_init(&open_inodes);
  /* Cache Buffer */
  cache_buffer = malloc(BUFFER_BLOCK_COUNT * BLOCK_SECTOR_SIZE);
  if (cache_buffer == NULL)
    PANIC("inode_init malloc fail");
  
  lock_init(&queue_lock);

  /* initialize 64 struct meta */
  meta_buffer = malloc(BUFFER_BLOCK_COUNT * sizeof(meta_t));
  if (cache_buffer == NULL)
    PANIC("inode_init malloc fail");
  /* add to at queue */
  list_init(&sc_blocks);
  list_init(&active_blocks);
  for (int i = 0; i < BUFFER_BLOCK_COUNT; i++) {
    lock_init(&(meta_buffer[i].meta_lock));
    rw_lock_init(&(meta_buffer[i].block_lock));
    meta_buffer[i].block = cache_buffer + i;
    meta_buffer[i].dirty = false;
    meta_buffer[i].worker_cnt = 0;
    meta_buffer[i].sector = INIT_SECTOR;
    if (i < AT_LENGTH)
      list_push_back(&active_blocks, &(meta_buffer[i].elem));
    else
      list_push_back(&sc_blocks, &(meta_buffer[i].elem));
  }
}

/* Initializes an inode with LENGTH bytes of data and
   writes the new inode to sector SECTOR on the file system
   device.
   Returns true if successful.
   Returns false if memory or disk allocation fails. */
bool inode_create(block_sector_t sector, off_t length) {
  struct inode_disk *disk_inode = NULL;
  bool success = false;

  ASSERT(length >= 0);

  /* If this assertion fails, the inode structure is not exactly
     one sector in size, and you should fix that. */
  ASSERT(sizeof *disk_inode == BLOCK_SECTOR_SIZE);

  disk_inode = calloc(1, sizeof *disk_inode);
  if (disk_inode != NULL) {
    /* 所需使用的扇区个数 */
    size_t sectors = bytes_to_sectors(length);
    disk_inode->length = length;
    disk_inode->magic = INODE_MAGIC;
    /* 在Free Map中分配指定数目的的扇区，开始位置保存为disk_inode->start */
    if (free_map_allocate(sectors, &disk_inode->start)) {
      /* 将inode_disk写入到设备中 */
      block_write(fs_device, sector, disk_inode);
      if (sectors > 0) {
        static char zeros[BLOCK_SECTOR_SIZE];
        size_t i;

        /* 将所有的数据扇区写入到文件中 */
        for (i = 0; i < sectors; i++)
          block_write(fs_device, disk_inode->start + i, zeros);
      }
      success = true;
    }
    free(disk_inode);
  }
  return success;
}

/* Reads an inode from SECTOR
   and returns a `struct inode' that contains it.
   Returns a null pointer if memory allocation fails. */
struct inode *inode_open(block_sector_t sector) {
  struct list_elem *e;
  struct inode *inode;

  /* Check whether this inode is already open. */
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      return inode;
    }
  }

  /* Allocate memory. */
  inode = malloc(sizeof *inode);
  if (inode == NULL)
    return NULL;

  /* Initialize. */
  list_push_front(&open_inodes, &inode->elem);
  inode->sector = sector;
  inode->open_cnt = 1;
  inode->deny_write_cnt = 0;
  inode->removed = false;
  block_read(fs_device, inode->sector, &inode->data);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen(struct inode *inode) {
  if (inode != NULL)
    inode->open_cnt++;
  return inode;
}

/* Returns INODE's inode number. */
block_sector_t inode_get_inumber(const struct inode *inode) { return inode->sector; }

/* Closes INODE and writes it to disk.
   If this was the last reference to INODE, frees its memory.
   If INODE was also a removed inode, frees its blocks. */
void inode_close(struct inode *inode) {
  /* Ignore null pointer. */
  if (inode == NULL)
    return;

  /* Release resources if this was the last opener. */
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);

    /* Deallocate blocks if removed. */
    if (inode->removed) {
      free_map_release(inode->sector, 1);
      free_map_release(inode->data.start, bytes_to_sectors(inode->data.length));
    }

    free(inode);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode) {
  ASSERT(inode != NULL);
  inode->removed = true;
}

/* Reads SIZE bytes from INODE into BUFFER, starting at position OFFSET.
   Returns the number of bytes actually read, which may be less
   than SIZE if an error occurs or end of file is reached.

   简而言之就是将文件INODE中从OFFSET开始，长度为SIZE的数据读取到BUFFER中

   实现的关键在于OFFSET不仅可能是文件的开始，也可能是中间某个扇区的起始位置
   还有可能是位于某个扇区的中间，这时候就要注意从OFFSET开始将数据读取

   文件的结束位置和扇区边界之间也需要注意，文件可能在扇区的边界结束，也有可能
   在某个扇区的中间位置结束，这时候就需要注意不要将文件结尾与扇区的边界之间的
   垃圾值读取到Buffer中（Buffer的大小可能只有SIZE，读取过多数据可能会溢出）

   此外，还有可能OFFSET+SIZE结束在某个扇区的中间，这时候也要注意不要将垃圾值
   读取到Buffer中

   综上，此函数整体读取过程可被归结如下：
   1. 计算chunk_size：本轮循环中需要读取到Buffer中、位于当前聚焦扇区中的数据量：
    1. 如果chunk_size恰好为BLOCK_SECTOR_SIZE，那么可以直接将聚焦扇区的所有
       内容都读取到Buffer中，此时聚焦扇区的所有内容都需要读取到文件中；
    2. 如果chunk_size不为BLOCK_SECTOR_SIZE，那么聚焦扇区可能有如下三种可能性：
       1. OFFSET开始于聚焦扇区的中间；
       2. 聚焦扇区包含文件的末尾；
       3. OFFSET+SIZE的结束位置位于聚焦扇区的中间；
       总而言之，只需要将聚焦扇区的一部分读取到Buffer中
   2. 如果chunk_size恰好为BLOCK_SECTOR_SIZE，那么可以直接调用block_read将
      整个扇区的数据都读取到Buffer中。相对的，需要将bounce中的内容读取到Buffer中；*/
off_t inode_read_at(struct inode *inode, void *buffer_, off_t size, off_t offset) {
  uint8_t *buffer = buffer_;
  off_t bytes_read = 0;
  uint8_t *bounce = NULL;

  while (size > 0) {
    /* Disk sector to read, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    /* 需从聚焦扇区内部的什么位置开始读取数据 */
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually copy out of this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Read full sector directly into caller's buffer. */
      block_read(fs_device, sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      block_read(fs_device, sector_idx, bounce);
      memcpy(buffer + bytes_read, bounce + sector_ofs, chunk_size);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_read += chunk_size;
  }
  free(bounce);

  return bytes_read;
}

/* Writes SIZE bytes from BUFFER into INODE, starting at OFFSET.
   Returns the number of bytes actually written, which may be
   less than SIZE if end of file is reached or an error occurs.

   现在的实现如果在文件末尾继续写入数据的话，不会对文件进行拓展
   (Normally a write at end of file would extend the inode, but
   growth is not yet implemented.) */
off_t inode_write_at(struct inode *inode, const void *buffer_, off_t size, off_t offset) {
  const uint8_t *buffer = buffer_;
  off_t bytes_written = 0;
  uint8_t *bounce = NULL;

  if (inode->deny_write_cnt)
    return 0;

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    int sector_ofs = offset % BLOCK_SECTOR_SIZE;

    /* Bytes left in inode, bytes left in sector, lesser of the two. */
    off_t inode_left = inode_length(inode) - offset;
    int sector_left = BLOCK_SECTOR_SIZE - sector_ofs;
    int min_left = inode_left < sector_left ? inode_left : sector_left;

    /* Number of bytes to actually write into this sector. */
    int chunk_size = size < min_left ? size : min_left;
    if (chunk_size <= 0)
      break;

    if (sector_ofs == 0 && chunk_size == BLOCK_SECTOR_SIZE) {
      /* Write full sector directly to disk. */
      block_write(fs_device, sector_idx, buffer + bytes_written);
    } else {
      /* We need a bounce buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }

      /* If the sector contains data before or after the chunk
             we're writing, then we need to read in the sector
             first.  Otherwise we start with a sector of all zeros. */
      if (sector_ofs > 0 || chunk_size < sector_left)
        block_read(fs_device, sector_idx, bounce);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      block_write(fs_device, sector_idx, bounce);
    }

    /* Advance. */
    size -= chunk_size;
    offset += chunk_size;
    bytes_written += chunk_size;
  }
  free(bounce);

  return bytes_written;
}

/* Disables writes to INODE.
   May be called at most once per inode opener.
   每个开启此文件的进程只可调用一次此函数 */
void inode_deny_write(struct inode *inode) {
  inode->deny_write_cnt++;
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
}

/* Re-enables writes to INODE.
   Must be called once by each inode opener who has called
   inode_deny_write() on the inode, before closing the inode. */
void inode_allow_write(struct inode *inode) {
  ASSERT(inode->deny_write_cnt > 0);
  ASSERT(inode->deny_write_cnt <= inode->open_cnt);
  inode->deny_write_cnt--;
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) { return inode->data.length; }

/**
 * @brief 将META从sc队列移动到at队列
 * 
 * @param meta 
 * @pre 当前线程持有队列锁以及meta的锁，同时worker_cnt不为0
 */
static void move_meta_to_at(meta_t* meta){
  ASSERT(lock_held_by_current_thread(&queue_lock));
  ASSERT(lock_held_by_current_thread(&(meta->meta_lock)));
  ASSERT(meta->worker_cnt != 0);
  
  // 移除AT末尾元素，将其插入到SC头部，再将meta插入到AT头部
  list_remove(&meta->elem);
  list_push_front(&active_blocks, &meta->elem);
  struct list_elem *old_st_back = list_pop_back(&active_blocks);
  list_push_front(&sc_blocks, old_st_back);

  ASSERT(list_size(&sc_blocks) == SC_LENGTH);
  ASSERT(list_size(&active_blocks) == AT_LENGTH);
}

/**
 * @brief 正向遍历队列，查找是否已经将目标扇区加载到Cache Buffer中
 * 
 * @param sector 
 * @return meta_t* 
 *  已加载：将`meta`移动到合适位置并返回指针；
 *  未加载：返回`NULL`
 */
static meta_t* find_sector(off_t sector){
  ASSERT(lock_held_by_current_thread(&queue_lock));

  struct lock* queue_lock = &queue_lock;
  struct list* active_queue = &active_blocks;
  meta_t *pos = NULL;
  meta_t *result = NULL;

  // 在active list中寻找sector
  list_for_each_entry(pos, active_queue, elem){
    lock_acquire(&pos->meta_lock);
    if(pos->sector == sector){
      result = pos;
      pos->worker_cnt++;
      lock_release(&pos->meta_lock);
      goto done;
    }
    lock_release(&pos->meta_lock);
  }

  struct list* sc_list = &sc_blocks;
  // 在sc list中寻找sector，执行队列移动
  list_for_each_entry(pos, sc_list, elem){
    lock_acquire(&pos->meta_lock);
    if(pos->sector == sector){
      result = pos;
      pos->worker_cnt++;
      move_meta_to_at(pos);
      lock_release(&pos->meta_lock);
      goto done;
    }
    lock_release(&pos->meta_lock);
  }

  done:
    return result;
}

/**
 * @brief 确保指定扇区被加载到Cache Buffer中，返回与该扇区对应的`meta`指针
 * 
 * @param sector 待查找的扇区；
 * @param load 读取旗标，是否要将该扇区的内容读取到`meta`中；
 * @return meta_t* 指向的`meta`为指定扇区中的内容
 */
static meta_t* search_sector(off_t sector, bool load){
  lock_acquire(&queue_lock);

  meta_t *result = NULL;
  // 正向遍历两个队列
  result = find_sector(sector);
  if(result != NULL){

  }


  
  lock_release(&queue_lock);
}