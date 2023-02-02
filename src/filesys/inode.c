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
/* 8 MB */
#define MAX_FILE_LENGTH 8 * 1024 * 1024

/* Inode直属直接块指针的数目 */
#define INODE_DIRECT_COUNT 64
/* Inode直属间接块指针的数目 */
#define INODE_INDIRECT_COUNT 48
/* Inode直属间接块部分可表示的扇区数目 */
#define INODE_INDIRECT_SECTOR_COUNT INODE_INDIRECT_COUNT *BLOCK_SECTOR_SIZE
/* Inode直属直接块与直属间接块部分可表示的扇区数目 */
#define INODE_DIRECT_INDIRECT_SECTOR_COUNT INODE_DIRECT_COUNT + INODE_INDIRECT_SECTOR_COUNT
/* Inode直属二级间接块指针的数目 */
#define INODE_DB_INDIRECT_COUNT 1
/* 间接块可容纳的指针数量 */
#define INDIRECT_BLOCK_PTR_COUNT 128
/* 二级间接块可容纳的指针数量 */
#define DB_INDIRECT_BLOCK_PTR_COUNT 128

#ifndef max
#define max(a, b) (((a) > (b)) ? (a) : (b))
#endif

#ifndef min
#define min(a, b) (((a) < (b)) ? (a) : (b))
#endif

/* 长度为512的全零数组 */
static const uint8_t zeros[BLOCK_SECTOR_SIZE];
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
  struct list_elem elem; /* Element in inode list. 修改时需要获取Inode队列锁 */
  block_sector_t sector; /* Sector number of disk location. 由于只会在初始化时被写入，因此不需要同步措施 */
  struct lock lock;   /* Inode的锁 */
  int open_cnt;       /* Number of openers. */
  bool removed;       /* True if deleted, false otherwise. */
  int deny_write_cnt; /* 0: writes ok, >0: deny writes. */
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
static void write_to_sector(block_sector_t sector, const uint8_t *buffer);
static meta_t *search_sector_head(block_sector_t sector, bool load);
static void search_sector_tail(meta_t *);
static bool enlarge_inode(struct inode_disk *, off_t size, off_t offset);
static void dealloc_inode(struct inode *);

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
    search_sector_tail(__meta);                                                                              \
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
    search_sector_tail(__meta);                                                                              \
  } while (0)

/* Returns the block device sector that contains byte offset POS
   within INODE.
   计算POS在文件INODE中位于哪一个扇区中，返回该扇区下标

   Returns INIT_SECTOR if INODE does not contain data for a byte at offset
   POS. */
static block_sector_t byte_to_sector(const struct inode *inode, off_t pos) {
  ASSERT(inode != NULL);
  meta_t *meta = search_sector_head(inode->sector, true);
  block_sector_t result = INIT_SECTOR;
  struct inode_disk *inode_disk = (struct inode_disk *)meta->block;
  // 由于Pos是下标，因此需要递增
  size_t sectors = bytes_to_sectors(pos + 1);
  lock_acquire(&meta->block_lock);
  // 如果大于现有文件大小，直接返回INIT_SECTOR
  if (pos >= inode_disk->length)
    goto done;

  /* POS位于直接块部分 */
  if (sectors <= INODE_DIRECT_COUNT) {
    result = inode_disk->dr_arr[sectors - 1];
    goto done;
  }

  /* Inode直属间接块 */
  if (sectors <= INODE_DIRECT_INDIRECT_SECTOR_COUNT) {
    block_sector_t idr_portion_sector_cnt = sectors - INODE_DIRECT_COUNT;
    ASSERT(idr_portion_sector_cnt >= 1);
    size_t idr_block_idx = (idr_portion_sector_cnt - 1) / INDIRECT_BLOCK_PTR_COUNT;
    size_t in_block_idx = (idr_portion_sector_cnt - 1) % INDIRECT_BLOCK_PTR_COUNT;
    meta_t *idr_meta = search_sector_head(inode_disk->idr_arr[idr_block_idx], true);
    safe_sector_read(idr_meta, {
      idr_block_t *idr_block = (idr_block_t *)idr_meta->block;
      result = idr_block->arr[in_block_idx];
    });
  }
  /* 二级间接块 */
  else {
    ASSERT(inode_disk->dbi_arr != 0);
    block_sector_t db_idr_portion_sector_cnt = sectors - INODE_DIRECT_INDIRECT_SECTOR_COUNT;
    ASSERT(db_idr_portion_sector_cnt >= 1);
    size_t db_idr_block_idx = (db_idr_portion_sector_cnt - 1) / INDIRECT_BLOCK_PTR_COUNT;
    size_t in_block_idx = (db_idr_portion_sector_cnt - 1) % INDIRECT_BLOCK_PTR_COUNT;
    // POS所在间接块的扇区
    block_sector_t idr_block_sector = 0;
    // 首先将二级间接块读取出来
    meta_t *db_meta = search_sector_head(inode_disk->dbi_arr, true);
    safe_sector_read(db_meta, {
      dbi_block_t *db_block = (dbi_block_t *)db_meta->block;
      idr_block_sector = db_block->arr[db_idr_block_idx];
    });
    ASSERT(idr_block_sector != 0);
    //  再从间接块中读取直接块的扇区
    meta_t *idr_meta = search_sector_head(idr_block_sector, true);
    safe_sector_read(idr_meta, {
      idr_block_t *idr_block = (idr_block_t *)idr_meta->block;
      result = idr_block->arr[in_block_idx];
    });
  }
done:
  lock_release(&meta->block_lock);
  search_sector_tail(meta);
  ASSERT(result != 0);

  return result;
}

/* List of open inodes, so that opening a single inode twice
   returns the same `struct inode'. */
static struct list open_inodes;

/* Inode列表锁，不可在持有Inode锁时获取此锁 */
static struct lock inodes_lock;

/* Initializes the inode module. */
void inode_init(void) {
  list_init(&open_inodes);
  lock_init(&inodes_lock);
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
    disk_inode->length = 0;
    disk_inode->magic = INODE_MAGIC;
    /* 在Free Map中分配指定数目的的扇区，开始位置保存为disk_inode->start */
    if (!enlarge_inode(disk_inode, 0, length)) {
      goto done;
    }
    write_to_sector(sector, (const uint8_t *)disk_inode);
    success = true;
  done:
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
  lock_acquire(&inodes_lock);
  for (e = list_begin(&open_inodes); e != list_end(&open_inodes); e = list_next(e)) {
    inode = list_entry(e, struct inode, elem);
    if (inode->sector == sector) {
      inode_reopen(inode);
      goto done;
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
  lock_init(&inode->lock);
done:
  lock_release(&inodes_lock);
  return inode;
}

/* Reopens and returns INODE. */
struct inode *inode_reopen(struct inode *inode) {
  if (inode != NULL) {
    lock_acquire(&inode->lock);
    inode->open_cnt++;
    lock_release(&inode->lock);
  }
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
  lock_acquire(&inodes_lock);
  lock_acquire(&inode->lock);
  if (--inode->open_cnt == 0) {
    /* Remove from inode list and release lock. */
    list_remove(&inode->elem);
    lock_release(&inodes_lock);
    /* Deallocate blocks if removed. */
    if (inode->removed) {
      dealloc_inode(inode);
    }

    free(inode);
  } else {
    // 由于可能会直接将Inode对应的内存释放掉，因此需要将释放锁的语句放在else中
    lock_release(&inode->lock);
    lock_release(&inodes_lock);
  }
}

/* Marks INODE to be deleted when it is closed by the last caller who
   has it open. */
void inode_remove(struct inode *inode) {
  ASSERT(inode != NULL);
  lock_acquire(&inode->lock);
  inode->removed = true;
  lock_release(&inode->lock);
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
    if (sector_idx == INIT_SECTOR)
      break;
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

  // 防止多个线程同时对正在执行文件延长的Inode做出修改
  lock_acquire(&inode->lock);
  if (inode->deny_write_cnt) {
    lock_release(&inode->lock);
    return 0;
  }

  if (inode_length(inode) < size + offset) {
    // 扩大文件
    meta_t *inode_meta = search_sector_head(inode->sector, true);
    bool success = false;
    safe_sector_write(inode_meta,
                      { success = enlarge_inode((struct inode_disk *)inode_meta->block, size, offset); });
    ASSERT(inode_length(inode) == size + offset);
    if (!success) {
      lock_release(&inode->lock);
      return 0;
    }
  }
  lock_release(&inode->lock);

  while (size > 0) {
    /* Sector to write, starting byte offset within sector. */
    block_sector_t sector_idx = byte_to_sector(inode, offset);
    if (sector_idx == INIT_SECTOR)
      break;
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
      //  TODO 删掉`bounce`
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
  if (bounce != NULL) {
    free(bounce);
  }
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
void flush_buffer_cache(void) {
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
off_t inode_length(const struct inode *inode) {
  meta_t *meta = search_sector_head(inode->sector, true);
  size_t length = 0;
  safe_sector_read(meta, { length = ((struct inode_disk *)meta->block)->length; });
  return length;
}

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
static meta_t *find_sector(block_sector_t sector) {
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
static meta_t *evict_meta_top_half(void) {
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
static void evict_meta_bottom_half(meta_t *meta, block_sector_t sector, bool load) {
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
static meta_t *search_sector_head(block_sector_t sector, bool load) {
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
  result = evict_meta_top_half();
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
 * @brief `search_sector_head`执行完毕之后以此函数收尾，中途可以插入获取meta->block并操作其中内容的代码
 *
 * @param meta
 */
static void search_sector_tail(meta_t *meta) {
  lock_acquire(&meta->meta_lock);
  meta->worker_cnt--;
  lock_release(&meta->meta_lock);
}

/**
 * @brief 将BUFFER中的内容覆写到SECTOR扇区中
 *
 * @param sector 目标扇区
 * @param buffer 待覆写内容，大小至少为512 Byte
 */
static void write_to_sector(block_sector_t sector, const uint8_t *buffer) {
  meta_t *meta = search_sector_head(sector, false);
  safe_sector_write(meta, { memcpy(meta->block, buffer, BLOCK_SECTOR_SIZE); });
}

/**
 * @brief 将SECTOR扇区读取到BUFFER中
 *
 * @param sector 目标扇区
 * @param buffer 缓存，大小至少为512 Byte
 */
static void read_from_sector(block_sector_t sector, uint8_t *buffer) {
  meta_t *meta = search_sector_head(sector, true);
  safe_sector_read(meta, { memcpy(buffer, meta->block, BLOCK_SECTOR_SIZE); });
}

/**
 * @brief 创建一个扇区并将Buffer中的内容写入其中
 *
 * @param buffer
 * @return block_sector_t 成功时返回扇区下标，失败时返回0
 */
static block_sector_t create_write_sector(const uint8_t *buffer) {
  block_sector_t sector = 0;
  // 为新扇区分配空间
  if (!free_map_allocate(1, &sector)) {
    return 0;
  }
  ASSERT(sector != 0);

  // 腾出一个meta
  lock_acquire(&queue_lock);
  meta_t *meta = NULL;
  meta = evict_meta_top_half();
  lock_release(&queue_lock);
  evict_meta_bottom_half(meta, sector, false);

  // 写入新数据
  safe_sector_write(meta, { memcpy(meta->block, buffer, BLOCK_SECTOR_SIZE); });

  return sector;
}

/**
 * @brief 仿照Dic中的实例设计的扇区分配函数，将`sectors`中直接块指针的数目变更为`except_sector_cnt`
 * 如果`except_sector_cnt` >= length，那么会将`sectors`中所有空余位置都使用新扇区填补。
 * 同时，假设`sectors`中已分配扇区的位置非零，未分配扇区的位置为零。
 *
 * @note 函数会确保每一个实际分配的新扇区都被清零
 *
 * @param sectors 待修改的直接块指针数组
 * @param length `sectors`长度
 * @param except_sector_cnt 成果物中应当含有多少个扇区，上限为128
 * @param spare_sector 是否使用Spare Sector填补空间？
 * @return true
 * @return false
 */
static bool resize_direct_ptr_portion(block_sector_t *sectors, block_sector_t length,
                                      block_sector_t except_sector_cnt, bool spare_sector) {
  // 所分配的第一个新扇区在idr_block中的下标
  block_sector_t begin_idx = INIT_SECTOR;
  bool success = false;
  for (block_sector_t i = 0; i < length; i++) {
    if (sectors[i] == 0 && i < except_sector_cnt) {
      // Grow
      if (begin_idx == INIT_SECTOR) {
        // Beginning of new free sector
        begin_idx = i;
      }
      if (spare_sector) {
        sectors[i] = SPARSE_FILE_SECTOR;
      } else {
        sectors[i] = create_write_sector(zeros);
        if (sectors[i] == 0) {
          goto done;
        }
      }
    } else if (sectors[i] != 0 && i >= except_sector_cnt) {
      // Shrink
      if (sectors[i] != SPARSE_FILE_SECTOR) {
        free_map_release(sectors[i], 1);
      }
      sectors[i] = 0;
    }
  }
  success = true;
done:
  // 如果分配新扇区失败，需要将所有新分配的扇区都释放
  if (!success) {
    for (block_sector_t i = begin_idx; i < length; i++) {
      if (sectors[i] != 0) {
        if (sectors[i] != SPARSE_FILE_SECTOR) {
          free_map_release(sectors[i], 1);
        }
        sectors[i] = 0;
      } else {
        break;
      }
    }
  }
  return success;
}

/**
 * @brief 释放`idr_arr`中 [begin_idx, tail_idx) 之间的所有间接块扇区以及其中所含的直接块扇区
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
  for (block_sector_t j = begin_idx; j != tail_idx; j++) {
    if (idr_arr[j] != 0 && idr_arr[j] != SPARSE_FILE_SECTOR) {
      meta_t *meta = search_sector_head(idr_arr[j], true);
      lock_acquire(&meta->block_lock);
      // 将间接块中的所有直接块清空
      resize_direct_ptr_portion((block_sector_t *)meta->block, INDIRECT_BLOCK_PTR_COUNT, 0, false);
      meta->dirty = false;
      meta->sector = INIT_SECTOR;

      lock_release(&meta->block_lock);
      search_sector_tail(meta);
      free_map_release(idr_arr[j], 1);
    }
    idr_arr[j] = 0;
  }
}

/**
 * @brief 在`idr_block_arr`指向的间接块区域中分配足够容纳`except_sector_cnt`个扇区的间接块和直接块。
 * 此函数可被递归调用一次，如果`root`为`true`，那么此函数会分配一个二级间接块并在其基础上递归一次，反之不会
 * 执行递归逻辑
 *
 * TODO 如果在此处添加一个用于描述`except_sector_cnt`中有多少个spare sector的参数的话，就可以实现spare file了
 *
 * @note 此函数的特定是确保执行失败时不会修改`idr_block_arr`中的数据
 *
 * @pre `except_sector_cnt`, `alloc_sector_cnt`的单位都是扇区
 * @pre alloc_sector_cnt > INODE_DIRECT_COUNT
 * @pre except_sector_cnt > INODE_DIRECT_COUNT
 *
 * @param idr_block_arr
 * 指向间接块数组的第一个元素，既可以是Inode中原生的间接块数组，也可以是二级间接块中的数组
 * @param arr_length 数组的长度
 * @param alloc_sector_cnt 之前在此Inode区域中所分配的扇区数量
 * @param except_sector_cnt 期望在此区域中达到的扇区分配数量情况
 * @param root 是否分配二级间接块并执行递归？
 * @param db_block 二级间接块指针地址，如果root == true，其必须不为NULL
 * @return true 分配成功
 * @return false 分配失败
 */
static bool alloc_indirect_portion(block_sector_t *idr_block_arr, block_sector_t arr_length,
                                   block_sector_t alloc_sector_cnt, block_sector_t except_sector_cnt,
                                   bool root, block_sector_t *db_block) {

  ASSERT(alloc_sector_cnt != except_sector_cnt);
  bool success = false;
  // EOF所在间接块中有多少个扇区已分配
  int in_block_alloced_sec_cnt =
      alloc_sector_cnt == 0 ? -1 : (int)(alloc_sector_cnt % INDIRECT_BLOCK_PTR_COUNT);
  // EOF所在间接块的下标
  int indirect_blk_idx = alloc_sector_cnt == 0 ? -1 : (int)(alloc_sector_cnt / INDIRECT_BLOCK_PTR_COUNT);
  // 第一个被成功分配的新间接块
  block_sector_t first_idr_block = 0;
  // 最后一个被成功分配的新间接块的尾后下标
  block_sector_t last_idr_block = 0;
  for (int i = 0; i != arr_length && except_sector_cnt != 0; i++) {
    /* 需要在此位置分配新的间接块 */
    if (idr_block_arr[i] == 0 && except_sector_cnt > 0) {
      if (first_idr_block == 0) {
        first_idr_block = i;
      }
      last_idr_block = i;
      idr_block_arr[i] = create_write_sector(zeros);
      if (idr_block_arr[i] == 0) {
        goto done;
      }
      // 由于idr_block_arr[i]分配成功，需要last_idr_block++才是尾后间接块下标
      last_idr_block++;
      // 等待分配的扇区数量
      block_sector_t pending_alloc_sector_cnt = min(except_sector_cnt, INDIRECT_BLOCK_PTR_COUNT);
      meta_t *meta = search_sector_head(idr_block_arr[i], false);
      idr_block_t *indirect_block = (idr_block_t *)meta->block;
      safe_sector_write(meta, {
        success = resize_direct_ptr_portion(indirect_block->arr, INDIRECT_BLOCK_PTR_COUNT,
                                            pending_alloc_sector_cnt, false);
      });
      if (!success)
        goto done;

      except_sector_cnt -= pending_alloc_sector_cnt;
    }
    /* 文件旧的EOF位于此间接块中 */
    else if (i == indirect_blk_idx && idr_block_arr[i] != 0) {
      meta_t *meta = search_sector_head(idr_block_arr[i], true);
      idr_block_t *indirect_block = (idr_block_t *)meta->block;
      // 等待分配的扇区数量
      block_sector_t pending_alloc_sector_cnt = min(except_sector_cnt, INDIRECT_BLOCK_PTR_COUNT);

      safe_sector_write(meta, {
        success = resize_direct_ptr_portion(indirect_block->arr, INDIRECT_BLOCK_PTR_COUNT,
                                            pending_alloc_sector_cnt, false);
      });
      if (!success)
        goto done;

      if (except_sector_cnt < INDIRECT_BLOCK_PTR_COUNT) {
        except_sector_cnt = 0;
      } else {
        except_sector_cnt -= INDIRECT_BLOCK_PTR_COUNT;
      }
    }
    /* 此间接块已满 */
    else {
      except_sector_cnt -= INDIRECT_BLOCK_PTR_COUNT;
    }
  }
  // 是否需要递归一层二级间接块
  if (root) {
    ASSERT(db_block != NULL);
    ASSERT(*db_block == 0);
    ASSERT(except_sector_cnt > 0);
    // 为新扇区分配空间
    *db_block = create_write_sector(zeros);
    if (*db_block == 0) {
      goto done;
    }

    // 腾出一个meta
    lock_acquire(&queue_lock);
    meta_t *meta = NULL;
    meta = evict_meta_top_half();
    lock_release(&queue_lock);
    evict_meta_bottom_half(meta, *db_block, false);

    safe_sector_write(meta, {
      success = alloc_indirect_portion((block_sector_t *)meta->block, INODE_DB_INDIRECT_COUNT, 0,
                                       except_sector_cnt, false, NULL);
    });
    if (!success) {
      goto done;
    }
  }
  success = true;
done:
  if (!success) {
    // 释放所有新分配的间接块
    release_full_indirect_block(idr_block_arr, first_idr_block, last_idr_block);
    // 需要将EOF所在间接块回归原样
    if (indirect_blk_idx > 0) {
      meta_t *meta = search_sector_head(idr_block_arr[indirect_blk_idx], true);
      idr_block_t *indirect_block = (idr_block_t *)meta->block;
      // 由于需要保留EOF原来所在的扇区，因此可以直接使用（in_block_alloced_sec_cnt可作为原EOF的尾后下标）
      safe_sector_write(meta, {
        resize_direct_ptr_portion(indirect_block->arr, INDIRECT_BLOCK_PTR_COUNT, in_block_alloced_sec_cnt,
                                  false);
      });
    }
  }
  return success;
}

/**
 * @brief 释放`inode`
 *
 * @param inode
 * @param arr_length
 */
static void dealloc_inode(struct inode *inode) {
  meta_t *meta = search_sector_head(inode->sector, true);
  struct inode_disk *inode_disk = (struct inode_disk *)meta->block;
  lock_acquire(&meta->block_lock);
  // 释放直属直接块部分
  resize_direct_ptr_portion(inode_disk->dr_arr, INODE_DIRECT_COUNT, 0, false);
  // 释放直属间接块部分
  release_full_indirect_block(inode_disk->idr_arr, 0, INODE_INDIRECT_COUNT);
  if (inode_disk->dbi_arr != 0) {
    // 释放二级间接块
    meta_t *db_block_meta = search_sector_head(inode_disk->dbi_arr, true);
    dbi_block_t *db_block = (dbi_block_t *)db_block_meta->block;
    lock_acquire(&db_block_meta->block_lock);
    release_full_indirect_block(db_block->arr, 0, INDIRECT_BLOCK_PTR_COUNT);
    // 注意需要确保`meta`中的数据不会造成危害
    db_block_meta->dirty = false;
    db_block_meta->sector = INIT_SECTOR;
    lock_release(&db_block_meta->block_lock);
    search_sector_tail(db_block_meta);
    free_map_release(inode_disk->dbi_arr, 1);
    inode_disk->dbi_arr = 0;
  }
  inode_disk->length = 0;
  meta->dirty = false;
  meta->sector = INIT_SECTOR;
  lock_release(&meta->block_lock);
  search_sector_tail(meta);
  free_map_release(inode->sector, 1);
}

/**
 * @brief 将文件扩大到令INODE足以附加大小为 SIZE + OFFSET
 * 的新数据。确保当磁盘分配失败时inode中的数据不会被影响
 * @pre 获取inode
 *
 * @note 会将所有新分配的直接块清空，执行成功时更新`inode_disk->length`
 *
 * @param inode_disk
 * @param size
 * @param offset
 */
static bool enlarge_inode(struct inode_disk *inode_disk, off_t size, off_t offset) {
  // 首先将所有需要的间接块分配完毕之后（当然是货真价实地调用create_write_sector分配间接块）
  // 整体计算还是采用扇区数量进行计算好了，每当调用create_write_sector递减本次分配的扇区数量
  // 再调用indirect_block_resize为各间接块分配直接块

  // 如果EOF本身就位于某个间接块的末尾，可以通过取整128进行判断
  if (size + offset > MAX_FILE_LENGTH) {
    return false;
  }
  bool success = false;

  // 如果EOF位于扇区的中段，检查能否直接通过移动EOF实现扩大文件
  if(bytes_to_sectors(inode_disk->length) == bytes_to_sectors(size + offset)){
    success = true;
    goto done;
  }

  block_sector_t prev_sector_cnt = bytes_to_sectors(inode_disk->length);
  block_sector_t except_sector_cnt = bytes_to_sectors(size + offset);

  /*
    下列注释中的[]指的是Inode中的指针区域：
    [ Inode中直接块指针区域 ]：[0, INODE_DIRECT_COUNT)
    [ Inode中间接块指针区域 ]：[INODE_DIRECT_COUNT, INODE_INDIRECT_SECTOR_COUNT)
    [ 二级间接块中的间接指针区域 ]：
      [INODE_INDIRECT_SECTOR_COUNT, MAX_FILE_LENGTH / BLOCK_SECTOR_SIZE)
  */
  // 间接块区域中原有的扇区数目
  block_sector_t idr_prev_sector_cnt =
      prev_sector_cnt > INODE_DIRECT_COUNT ? prev_sector_cnt - INODE_DIRECT_COUNT : 0;
  // 间接块区域中目标应有扇区数目
  block_sector_t idr_except_sector_cnt =
      except_sector_cnt > INODE_DIRECT_COUNT ? except_sector_cnt - INODE_DIRECT_COUNT : 0;
  if (prev_sector_cnt <= INODE_DIRECT_COUNT) {
    /* Case 1: [ EOF, New EOF] [ ] [ ] */
    if (except_sector_cnt <= INODE_DIRECT_COUNT) {
      success = resize_direct_ptr_portion(inode_disk->dr_arr, INODE_DIRECT_COUNT, except_sector_cnt, false);
    }
    /* Case 2: [ EOF ] [ New EOF ] [ ] */
    else if (except_sector_cnt <= INODE_DIRECT_INDIRECT_SECTOR_COUNT) {
      success = resize_direct_ptr_portion(inode_disk->dr_arr, INODE_DIRECT_COUNT, INODE_DIRECT_COUNT, false);
      if (!success) {
        resize_direct_ptr_portion(inode_disk->dr_arr, INODE_DIRECT_COUNT, prev_sector_cnt, false);
        goto done;
      }
      success = alloc_indirect_portion(inode_disk->idr_arr, INODE_INDIRECT_COUNT, idr_prev_sector_cnt,
                                       idr_except_sector_cnt, false, NULL);
    }
    /* Case 3: [ EOF ] [ ] [ New EOF ] */
    else {
      success = resize_direct_ptr_portion(inode_disk->dr_arr, INODE_DIRECT_COUNT, INODE_DIRECT_COUNT, false);
      if (!success) {
        resize_direct_ptr_portion(inode_disk->dr_arr, INODE_DIRECT_COUNT, prev_sector_cnt, true);
        goto done;
      }
      success = alloc_indirect_portion(inode_disk->idr_arr, INODE_INDIRECT_COUNT, idr_prev_sector_cnt,
                                       idr_except_sector_cnt, true, &inode_disk->dbi_arr);
    }

  } else if (prev_sector_cnt <= INODE_DIRECT_INDIRECT_SECTOR_COUNT) {
    /* Case 4: [ ] [ EOF, New EOF ] [  ] */
    if (except_sector_cnt <= INODE_DIRECT_INDIRECT_SECTOR_COUNT) {
      success = alloc_indirect_portion(inode_disk->idr_arr, INODE_INDIRECT_COUNT, idr_prev_sector_cnt,
                                       idr_except_sector_cnt, false, NULL);
    }
    /* Case 5: [ ] [ EOF ] [ New EOF ] */
    else {
      success = alloc_indirect_portion(inode_disk->idr_arr, INODE_INDIRECT_COUNT, idr_prev_sector_cnt,
                                       idr_except_sector_cnt, true, &inode_disk->dbi_arr);
    }
  }
  /* Case 6: [ ] [ ] [ EOF, New EOF ] */
  else {
    block_sector_t db_prev_sector_cnt = idr_prev_sector_cnt > INODE_INDIRECT_SECTOR_COUNT
                                            ? idr_prev_sector_cnt - INODE_INDIRECT_SECTOR_COUNT
                                            : 0;
    block_sector_t db_except_sector_cnt = idr_except_sector_cnt > INODE_INDIRECT_SECTOR_COUNT
                                              ? idr_except_sector_cnt - INODE_INDIRECT_SECTOR_COUNT
                                              : 0;
    success = alloc_indirect_portion(inode_disk->idr_arr, INODE_DB_INDIRECT_COUNT, db_prev_sector_cnt,
                                     db_except_sector_cnt, false, NULL);
  }
done:
  if (success)
    inode_disk->length = offset + size;
  return success;
}
