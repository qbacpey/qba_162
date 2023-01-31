#include "filesys/inode.h"
#include "filesys/filesys.h"
#include "filesys/free-map.h"
#include "threads/malloc.h"
#include "threads/synch.h"
#include <debug.h>
#include <list.h>
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

/* Inode中各种块指针的数目 */
#define INODE_DIRECT_COUNT 64
#define INODE_INDIRECT_COUNT 48
#define INODE_DB_INDIRECT_COUNT 1
/* 间接块可容纳的指针数量 */
#define INDIRECT_BLOCK_PTR_COUNT 128
/* 二级间接块可容纳的指针数量 */
#define DB_INDIRECT_BLOCK_PTR_COUNT 128

/* On-disk inode.
   Must be exactly BLOCK_SECTOR_SIZE bytes long. */
struct inode_disk {
  off_t length;                                 /* File size in bytes. */
  unsigned magic;                               /* Magic number. */
  block_sector_t dr_arr[INODE_DIRECT_COUNT];    /* 直接块的下标 */
  block_sector_t idr_arr[INODE_INDIRECT_COUNT]; /* 间接块的下标 */
  block_sector_t dbi_arr;                       /* 二级间接块的下标 */
  uint32_t unused[126 - INODE_DIRECT_COUNT - INODE_INDIRECT_COUNT - INODE_DB_INDIRECT_COUNT]; /* Not used. */
};

/* 二级间接块 */
typedef struct dbi_blk {
  block_sector_t arr[INDIRECT_BLOCK_PTR_COUNT];
} dbi_block_t;

/* 间接块 */
typedef struct idr_block {
  block_sector_t arr[INDIRECT_BLOCK_PTR_COUNT];
} idr_block_t;

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

/*
TODO 需要添加同步措施
In-memory inode. */
struct inode {
  struct list_elem elem; /* Element in inode list. */
  block_sector_t sector; /* Sector number of disk location. */
  int open_cnt;          /* Number of openers. */
  off_t length;          /* File size in bytes. */
  bool removed;          /* True if deleted, false otherwise. */
  int deny_write_cnt;    /* 0: writes ok, >0: deny writes. */
};

/* Cache Buffer各Block的元数据，用于维护SC队列 */
typedef struct meta {
  struct lock meta_lock;  /* 元数据锁 */
  block_sector_t sector;  /* Block对应扇区的下标 */
  int worker_cnt;         /* 等待或正在使用Block的线程数 */
  struct lock block_lock; /* Block的锁 */
  bool dirty;             /* Dirty Bit，读写前先获取block_lock */
  uint8_t *block;         /* 元数据对应的Block */
  struct list_elem elem;  /* 必然位于SC或者Active中 */
} meta_t;

/* 64个meta的位置 */
static meta_t *meta_buffer;

static void read_from_sector(block_sector_t sector, uint8_t *buffer);
static void write_to_sector(block_sector_t sector, uint8_t *buffer);
static meta_t *search_sector(block_sector_t sector, bool load);

/**
 * @brief 线程安全地对META指向的Cache Block执行READ_ACTION，读取其中数据，完成后递减worker_cnt
 *
 * @param __meta 待读取的meta
 * @param read_action 读取操作（不会递增Dirty Bit）
 *
 * @note __meta->worker_cnt必须大于1
 */
#define safe_sector_read(__meta, read_action)                                                                \
  do {                                                                                                       \
    lock_acquire(&__meta->block_lock);                                                                       \
    { read_action; }                                                                                         \
    lock_release(&__meta->block_lock);                                                                       \
    lock_acquire(&__meta->meta_lock);                                                                        \
    __meta->worker_cnt--;                                                                                    \
    lock_release(&__meta->meta_lock);                                                                        \
  } while (0)

/**
 * @brief 线程安全地对META指向的Cache Block执行WRITE_ACTION，执行数据写入操作，完成后递减worker_cnt
 *
 * @param __meta 待写入的meta
 * @param write_action 写入操作（设置Dirty Bit）
 *
 * @note __meta->worker_cnt必须大于1
 */
#define safe_sector_write(__meta, write_action)                                                              \
  do {                                                                                                       \
    lock_acquire(&__meta->block_lock);                                                                       \
    { write_action; }                                                                                        \
    __meta->dirty = true;                                                                                    \
    lock_release(&__meta->block_lock);                                                                       \
    lock_acquire(&__meta->meta_lock);                                                                        \
    __meta->worker_cnt--;                                                                                    \
    lock_release(&__meta->meta_lock);                                                                        \
  } while (0)

/* TODO 实现需修改，估计需要加一段“从Buffer Cache中获取Inode”的代码

  Returns the block device sector that contains byte offset POS
   within INODE.
   计算POS在文件INODE中位于哪一个扇区中，返回该扇区下标

   Returns -1 if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);
  block_sector_t start;
  meta_t *meta = search_sector(inode->sector, true);
  safe_sector_read(meta, { start = ((struct inode_disk *)meta->block)->start; });
  if (pos < inode->length)
    return start + pos / BLOCK_SECTOR_SIZE;
  else
    return -1;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'.
   TODO 需要添加同步措施
    */
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
    lock_init(&(meta_buffer[i].block_lock));
    meta_buffer[i].block = cache_buffer + i * BLOCK_SECTOR_SIZE;
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
      write_to_sector(sector, (uint8_t *)disk_inode);

      if (sectors > 0) {
        static char zeros[BLOCK_SECTOR_SIZE];
        size_t i;

        /* 将所有的数据扇区写入到文件中 */
        for (i = 0; i < sectors; i++)
          write_to_sector(disk_inode->start + i, (uint8_t *)zeros);
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

  meta_t *meta = search_sector(inode->sector, true);
  safe_sector_read(meta, { inode->length = ((struct inode_disk *)meta->block)->length; });

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
      meta_t *meta = search_sector(inode->sector, true);
      block_sector_t start;
      safe_sector_read(meta, { start = ((struct inode_disk *)meta->block)->start; });
      free_map_release(start, bytes_to_sectors(inode->length));
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
      read_from_sector(sector_idx, buffer + bytes_read);
    } else {
      /* Read sector into bounce buffer, then partially copy
             into caller's buffer. */
      if (bounce == NULL) {
        bounce = malloc(BLOCK_SECTOR_SIZE);
        if (bounce == NULL)
          break;
      }
      read_from_sector(sector_idx, bounce);
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
      write_to_sector(sector_idx, buffer + bytes_written);
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
        read_from_sector(sector_idx, bounce);
      else
        memset(bounce, 0, BLOCK_SECTOR_SIZE);
      memcpy(bounce + sector_ofs, buffer + bytes_written, chunk_size);
      write_to_sector(sector_idx, bounce);
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

/**
 * @brief 将队列中所有的Dirty Block写入磁盘
 *
 * @note 只能在文件系统被关闭时调用
 *
 */
void flush_buffer_cache() {
  struct list *active_queue = &active_blocks;
  struct list *sc_queue = &sc_blocks;
  meta_t *pos = NULL;
  list_reverse_for_each_entry(pos, sc_queue, elem) {
    if (pos->dirty) {
      block_write(fs_device, pos->sector, pos->block);
      pos->dirty = false;
    }
  }
  list_reverse_for_each_entry(pos, active_queue, elem) {
    if (pos->dirty) {
      block_write(fs_device, pos->sector, pos->block);
      pos->dirty = false;
    }
  }
}

/* Returns the length, in bytes, of INODE's data. */
off_t inode_length(const struct inode *inode) { return inode->length; }

/**
 * @brief 将META从sc队列移动到at队列
 *
 * @param meta
 * @pre 当前线程持有队列锁以及meta的锁，同时worker_cnt不为0
 */
static void move_meta_to_at(meta_t *meta) {
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
static meta_t *find_sector(off_t sector) {
  ASSERT(lock_held_by_current_thread(&queue_lock));

  struct list *active_queue = &active_blocks;
  meta_t *pos = NULL;
  meta_t *result = NULL;

  // 在active list中寻找sector
  list_for_each_entry(pos, active_queue, elem) {
    lock_acquire(&pos->meta_lock);
    if (pos->sector == sector) {
      result = pos;
      pos->worker_cnt++;
      lock_release(&pos->meta_lock);
      goto done;
    }
    lock_release(&pos->meta_lock);
  }

  struct list *sc_queue = &sc_blocks;
  pos = NULL;
  // 在sc list中寻找sector，执行队列移动
  list_for_each_entry(pos, sc_queue, elem) {
    lock_acquire(&pos->meta_lock);
    if (pos->sector == sector) {
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
 * @brief 反向遍历队列，寻找可被移出Cache Buffer的`meta`，将其移动到队列中合适的位置。
 *        meta.sector和meta.dirty会维持不变
 *
 * @note
 * 执行完毕后，当前线程同时持有返回值的meta_lock以及block_lock。返回Caller之后，应当释放队列锁并调用evict_meta_bottom_half
 *
 * @param sector
 * @return meta_t*
 */
static meta_t *evict_meta_top_half(off_t sector) {
  ASSERT(lock_held_by_current_thread(&queue_lock));

  struct list *active_queue = &active_blocks;
  struct list *sc_queue = &sc_blocks;
  meta_t *pos = NULL;
  meta_t *result = NULL;
  struct lock *meta_lock = NULL;
  while (result == NULL) {
    // 在sc list中寻找可移出的meta，执行队列移动
    list_reverse_for_each_entry(pos, sc_queue, elem) {
      meta_lock = &pos->meta_lock;
      lock_acquire(meta_lock);
      if (pos->worker_cnt == 0) {
        result = pos;
        pos->worker_cnt++;
        move_meta_to_at(pos);
        goto done;
      }
      lock_release(meta_lock);
    }

    // 在at list中寻找可移出的meta
    list_reverse_for_each_entry(pos, active_queue, elem) {
      meta_lock = &pos->meta_lock;
      lock_acquire(meta_lock);
      if (pos->worker_cnt == 0) {
        result = pos;
        pos->worker_cnt++;
        goto done;
      }
      lock_release(meta_lock);
    }
  }

done:
  // 防止其他线程读取到垃圾值，需要先将与之对应的Cache Buffer锁住
  lock_acquire(&result->block_lock);
  // 之后还需要检查Dirty Bit并设置sector，因此不能释放meta_lock
  // lock_release(meta_lock);
  return result;
}

/**
 * @brief 执行磁盘IO：
 * 1. 如果meta->dirty == true，将Cache Block中的数据写入到扇区meta->sector；
 * 2. meta->dirty = false，meta->sector = sector；
 * 3. 按照读取旗标的要求，将新meta->sector中的内容从磁盘读取到Cache Buffer；
 *
 * @param meta 待处理的meta，应位于绿色队列，其中的dirty和sector维持原值
 * @param sector 需要替换到meta中的扇区
 * @param load 是否需要将sector位于磁盘中的数据加载到Cache Buffer中
 *
 * @pre 持有meta_lock，block_lock，不持有队列锁
 */
static void evict_meta_bottom_half(meta_t *meta, off_t sector, bool load) {
  ASSERT(!lock_held_by_current_thread(&queue_lock));
  ASSERT(lock_held_by_current_thread(&meta->meta_lock));
  ASSERT(lock_held_by_current_thread(&meta->block_lock));

  if (meta->dirty) {
    block_write(fs_device, meta->sector, meta->block);
    meta->dirty = false;
  }

  meta->sector = sector;

  if (load) {
    block_read(fs_device, meta->sector, meta->block);
  }
  lock_release(&meta->block_lock);
  lock_release(&meta->meta_lock);
}

/**
 * @brief 确保指定扇区被加载到Cache Buffer中，返回与该扇区对应的`meta`指针
 *
 * @param sector 待查找的扇区；
 * @param load 读取旗标，是否要将该扇区的内容读取到`meta`中；
 * @return meta_t* 指向的`meta`为指定扇区中的内容
 *
 * @post result->worker_cnt > 0
 * */
static meta_t *search_sector(block_sector_t sector, bool load) {
  lock_acquire(&queue_lock);

  meta_t *result = NULL;
  // 正向遍历两个队列
  result = find_sector(sector);
  if (result != NULL) {
    // 不用管读取旗标
    lock_release(&queue_lock);
    goto done;
  }

  // 反向遍历队列
  result = evict_meta_top_half(sector);
  ASSERT(result != NULL);

  lock_release(&queue_lock);

  // 释放队列锁之后再执行磁盘IO
  evict_meta_bottom_half(result, sector, load);
  ASSERT(result->worker_cnt == 1);

done:
  ASSERT(result != NULL);
  ASSERT(result->sector == sector);
  ASSERT(result->worker_cnt > 0);
  return result;
}

/**
 * @brief 将BUFFER中的内容覆写到SECTOR扇区中
 *
 * @param sector 目标扇区
 * @param buffer 待覆写内容，大小至少为512 Byte
 */
static void write_to_sector(block_sector_t sector, uint8_t *buffer) {
  meta_t *meta = search_sector(sector, false);
  safe_sector_write(meta, { memcpy(meta->block, buffer, BLOCK_SECTOR_SIZE); });
}

/**
 * @brief 将SECTOR扇区读取到BUFFER中
 *
 * @param sector 目标扇区
 * @param buffer 缓存，大小至少为512 Byte
 */
static void read_from_sector(block_sector_t sector, uint8_t *buffer) {
  meta_t *meta = search_sector(sector, true);
  safe_sector_read(meta, { memcpy(buffer, meta->block, BLOCK_SECTOR_SIZE); });
}

/**
 * @brief 创建一个扇区并将Buffer中的内容写入其中
 *
 * @param buffer
 * @return block_sector_t 成功时返回扇区下标，失败时返回0
 */
static block_sector_t create_write_sector(uint8_t *buffer) {
  block_sector_t sector = INIT_SECTOR;
  // 为新扇区分配空间
  if (!free_map_allocate(1, &sector)) {
    return 0;
  }
  ASSERT(sector != INIT_SECTOR);

  // 腾出一个meta
  lock_acquire(&queue_lock);
  meta_t *meta = NULL;
  meta = evict_meta_top_half(sector);
  lock_release(&queue_lock);
  evict_meta_bottom_half(meta, sector, false);

  // 写入新数据
  safe_sector_write(meta, { memcpy(meta->block, buffer, BLOCK_SECTOR_SIZE); });

  return sector;
}

/**
 * @brief 仿照Dic中的实例设计的扇区分配函数，为IDR_BLOCK增加到SECTOR_CNT个扇区。
 * 如果SECTOR_CNT > INDIRECT_BLOCK_PTR_COUNT（128），那么会将IDR_BLOCK中所有空余位置都使用新扇区填补。
 * 同时，假设IDR_BLOCK中已分配扇区的位置非零，未分配扇区的位置为零。
 *
 * @note 此函数只不会执行任何同步操作
 *
 * @param idr_block 待修改的间接块
 * @param sector_cnt 成果物中应当含有多少个扇区
 * @param spare_sector 是否使用Spare Sector填补空间？
 * @return true
 * @return false
 */
static bool resize_indirect_block(idr_block_t *idr_block, block_sector_t sector_cnt, bool spare_sector) {
  // 所分配的第一个新扇区在idr_block中的下标
  block_sector_t tail_of_idr_block = INIT_SECTOR;
  bool success = false;
  for (int i = 0; i < INDIRECT_BLOCK_PTR_COUNT; i++) {
    if (idr_block->arr[i] == 0 && i < sector_cnt) {
      // Grow
      if (tail_of_idr_block == INIT_SECTOR) {
        // Beginning of new free sector
        tail_of_idr_block = i;
      }
      if (spare_sector) {
        idr_block->arr[i] = SPARSE_FILE_SECTOR;
      } else {
        if (!free_map_allocate(1, idr_block->arr + i)) {
          goto done;
        }
      }
    } else if (idr_block->arr[i] != 0 && i >= sector_cnt) {
      // Shrink
      if (idr_block->arr[i] != SPARSE_FILE_SECTOR) {
        free_map_release(idr_block->arr + i, 1);
      }
      idr_block->arr[i] = 0;
    }
  }
  success = true;
done:
  // 如果分配新扇区失败，需要将所有新分配的扇区都释放
  if (!success) {
    for (int i = tail_of_idr_block; i < INDIRECT_BLOCK_PTR_COUNT; i++) {
      if (idr_block->arr[i] != 0) {
        // TODO 其实不用检测，直接释放就可以
        if (!spare_sector) {
          free_map_release(idr_block->arr + i, 1);
        }
      } else {
        break;
      }
    }
  }
  return success;
}

/**
 * @brief 释放`idr_arr`中 [begin_idx, tail_idx) 之间的所有间接块扇区
 *
 * @param inode 指向Inode中间接块的部分的指针
 * @param begin_idx idr_arr中第一个被新分配的间接块的下标
 * @param tail_idx idr_arr中最后一个被新分配的间接块的尾后下标
 */
static void release_full_indirect_block(block_sector_t *idr_arr, block_sector_t begin_idx,
                                        block_sector_t tail_idx) {
  ASSERT(idr_arr != NULL);
  ASSERT(begin_idx < INODE_DIRECT_COUNT);
  ASSERT(tail_idx < INODE_DIRECT_COUNT);
  for (int j = begin_idx; j != tail_idx; j++) {
    meta_t *meta = search_sector(idr_arr[j], true);
    safe_sector_write(meta, { resize_indirect_block(idr_arr[j], 0, false); });
    free_map_release(idr_arr[j], 1);
  }
}

/**
 * @brief 如果本次磁盘分配分配了新的二级间接块，需要将其释放
 *
 * @param inode
 * @param old_ib_cnt 文件中原有的间接块数目
 */
static void handle_phrase_2_error(struct inode_disk *inode, block_sector_t old_ib_cnt) {
  if (old_ib_cnt <= INODE_DIRECT_COUNT) {
    if (inode->dbi_arr != 0) {
      free_map_release(inode->dbi_arr, 1);
      inode->dbi_arr = 0;
    }
  }
}

/**
 * @brief 释放db_indirect_block中所有新分配的间接块
 *
 * @param dbib double indirect block
 * @param old_ib_in_db 文件中原有的、位于二级间接块中的间接块数目
 * @param curr_eof 指向db_indirect_block中最后一个被分配的间接块
 */
static void handle_phrase_3_error(dbi_block_t *dbib, block_sector_t old_ib_in_db, block_sector_t curr_eof) {
  for (int j = curr_eof; j != old_ib_in_db - 1; j--) {
    free_map_release(dbib->arr[j], 1);
  }
}

/**
 * @brief 计算INODE如果要容纳EOF这么多扇区需要分配多少个间接块，在INODE中分配对应的新扇区作为间接块，
 * 如果要需要，也会分配二级间接块并腾出一个meta
 * 分配失败时会回滚操作，确保进入函数时的Inode和离开函数时的Inode一致。
 * @pre 文件旧扇区数目小于新扇区数目：old_eof < eof
 * @pre 文件的旧大小大于直接块可覆盖范围：old_eof > INODE_DIRECT_COUNT
 * @pre 文件的新大小小于8 MiB：eof < 8 * 1024 * 2
 *
 * @note INODE可以是Cache Buffer中的缓存，
 * 也可以是必须为使用calloc分配之后使用memcpy拷贝过来的对象，但函数不会在对Inode执行拷贝操作时同步
 *
 * @param inode Inode被读取到内存中的Buffer，必须持有对应的写者锁
 * @param old_eof 旧文件的扇区数量，单位为扇区个数（利用byte_to_sector转换）
 * @param eof 新文件的扇区数量，单位为扇区个数（利用byte_to_sector转换）
 * @return true 分配成功，现在持有的间接块足以完全容纳EOF
 * @return false 分配失败，维持INODE不变
 */
static bool alloc_indirect_block(struct inode_disk *inode, block_sector_t old_eof, block_sector_t eof) {
  ASSERT(old_eof < eof);
  ASSERT(eof > INODE_DIRECT_COUNT);
  ASSERT(old_eof > INODE_DIRECT_COUNT);
  ASSERT(eof < 8 * 1024 * 2);
  bool success = false;

  // 每一阶段的循环结束扇区
  block_sector_t phrase_loop_end = INODE_INDIRECT_COUNT;
  // 指向idr_arr中最后一个被分配的间接块
  block_sector_t ib_end_index = old_ib_cnt;

  /* Phrase 1： Alloc Indirect Block */
  if (old_ib_cnt <= INODE_INDIRECT_COUNT) {
    if (eof <= INODE_INDIRECT_COUNT) {
      phrase_loop_end = eof;
    } else {
      phrase_loop_end = INODE_INDIRECT_COUNT;
    }

    // 由于old_eof是扇区数量而不是数组下标，因此可以直接将其作为第一个空下标处理
    for (int i = old_ib_cnt; i != phrase_loop_end; i++) {
      ASSERT(inode->idr_arr[i] == 0);
      if (!free_map_allocate(1, inode->idr_arr + i)) {
        ib_end_index = i - 1;
        release_full_indirect_block(inode->idr_arr, old_ib_cnt, ib_end_index);
        goto done;
      }
    }
  }
  ib_end_index = phrase_loop_end - 1;

  success = true;

  ASSERT(inode->dbi_arr != 0);
  if (eof > INODE_INDIRECT_COUNT) {
    /* Phrase 2： Alloc Double Indirect Block */
    if (inode->dbi_arr == 0 && !free_map_allocate(1, inode->dbi_arr)) {
      handle_phrase_2_error(inode, old_ib_cnt);
      release_full_indirect_block(inode->idr_arr, old_ib_cnt, ib_end_index);
      goto done;
    }

    /* Phrase 3： Alloc Indirect Block inside Double Indirect Block */

    // eof现在反映在二级间接块中分配的间接块数目
    eof -= INODE_INDIRECT_COUNT;

    // 原先位于Double Indirect Block中的Indirect Block
    const block_sector_t old_ib_in_db = old_ib_cnt - INODE_INDIRECT_COUNT;

    // 二级间接块中可能有已经分配的间接块
    meta_t *db_idr_meta = search_sector(inode->dbi_arr, true);
    safe_sector_write(db_idr_meta, {
      dbi_block_t *db_idr_block = (dbi_block_t *)db_idr_meta->block;

      for (int i = old_ib_in_db; i < eof; i++) {
        ASSERT(db_idr_block->arr[i] == 0);
        if (!free_map_allocate(1, db_idr_block->arr + i)) {
          handle_phrase_3_error(db_idr_block, old_ib_in_db, i - 1);
          handle_phrase_2_error(inode, old_ib_cnt);
          release_full_indirect_block(inode->idr_arr, old_ib_cnt, ib_end_index);
          success = false;
          break;
        }
      }
    });
  }
done:
  return success;
}

/**
 * @brief 分配INODE中的idr_arr数组，确保尽可能能容纳得下EOF个扇区。分配失败回滚修改
 *
 * @pre `eof`, `old_eof`的单位都是扇区
 * @pre old_eof > INODE_DIRECT_COUNT
 * @pre eof > INODE_DIRECT_COUNT
 *
 * @param inode
 * @param old_eof INODE中之前所含扇区数目
 * @param eof 目标扇区数目，可大于 INDIRECT_BLOCK_PTR_COUNT * INODE_INDIRECT_COUNT
 * @return true 分配成功
 * @return false 分配失败
 */
static bool alloc_indirect_portion(struct inode_disk *inode, block_sector_t old_eof, block_sector_t eof) {
  ASSERT(eof > INODE_DIRECT_COUNT);
  if (old_eof > INODE_DIRECT_COUNT + INODE_INDIRECT_COUNT * INDIRECT_BLOCK_PTR_COUNT) {
    return true;
  }
  bool success = false;
  // 计算保存EOF需要的扇区数量
  eof -= INODE_DIRECT_COUNT;
  old_eof -= INODE_DIRECT_COUNT;
  // EOF在其间接块内部的下标
  int eof_idr_block = old_eof > 0 ? old_eof : old_eof % INDIRECT_BLOCK_PTR_COUNT;
  // 第一个被成功分配的新间接块
  block_sector_t first_idr_block = 0;
  // 最后一个被成功分配的新间接块的尾后下标
  block_sector_t last_idr_block = 0;
  for (int i = 0; i != INODE_INDIRECT_COUNT; i++) {
    if (eof > 0) {
      if (old_eof < (i + 1) * INDIRECT_BLOCK_PTR_COUNT &&
          inode->idr_arr[i] != 0) { // 文件旧的EOF位于此间接块中
        meta_t *meta = search_sector(inode->idr_arr[i], true);
        idr_block_t *indirect_block = (idr_block_t *)meta->block;
        safe_sector_write(
            meta, { success = resize_indirect_block(indirect_block, INDIRECT_BLOCK_PTR_COUNT, false); });
      } else if (inode->idr_arr[i] == 0) { // 需要在此位置分配新的间接块
        if (first_idr_block == 0) {
          first_idr_block = i;
        }
        last_idr_block = i;
        if (!free_map_allocate(1, inode->idr_arr + i)) {
          goto done;
        }
        // 由于inode->idr_arr[i]分配成功，需要last_idr_block++才是尾后间接块下标
        last_idr_block++;

        meta_t *meta = search_sector(inode->idr_arr[i], false);
        idr_block_t *indirect_block = (idr_block_t *)meta->block;
        safe_sector_write(meta, { success = resize_indirect_block(indirect_block, eof, false); });
      }
      if (!success) {
        goto done;
      }
      eof -= INDIRECT_BLOCK_PTR_COUNT;
    }
  }
  success = true;
done:
  if (!success) {
    release_full_indirect_block(inode->idr_arr, first_idr_block, last_idr_block);
    if(eof_idr_block > 0){
      meta_t *meta = search_sector(inode->idr_arr[eof_idr_block], true);
      safe_sector_write(meta, { resize_indirect_block(inode->idr_arr[eof_idr_block], 0, false); });
      free_map_release(inode->idr_arr[eof_idr_block], 1);
    }
  }
  return success;
}

/**
 * @brief 将文件扩大到令INODE足以附加大小为 SIZE + OFFSET
 * 的新数据。确保当磁盘分配失败时inode中的数据不会被影响
 * @pre 获取inode
 *
 * @note 读取inode.block时，不会执行同步操作
 *
 * @param inode
 * @param size
 * @param offset
 */
void enlarge_inode(struct inode *inode, off_t size, off_t offset) {
  // 首先将所有需要的间接块分配完毕之后（当然是货真价实地调用create_write_sector分配间接块）
  // 整体计算还是采用扇区数量进行计算好了，每当调用create_write_sector递减本次分配的扇区数量
  // 再调用indirect_block_resize为各间接块分配直接块

  // 如果EOF本身就位于某个间接块的末尾，可以通过取整128进行判断
}
