# Project File System Design 

----------
## Buffer Cache

所有涉及磁盘扇区的读写操作都需要经过Cache Buffer来完成。也就是说，无论是读还是写，首先都需要将扇区读取到Cache Buffer中作为Cache Block之后，再对其进行修改或读取其中的数据，随后有需要或者Cache Buffer没有空间了，就将Block写回到磁盘中。具体来说，有如下几项要求：
+ 置换算法：Second-chance List（VAX/VMS）即可；
+ 容量：Buffer最大可容纳64个扇区，维护数据结构所需的元数据不计入其中；
+ 同步：线程对Cache Buffer的访问应该被同步；
  + 线程向某个Buffer Block中写入数据或从中读取数据时，其他线程不可将该Block Evict回磁盘；
  + 当某个Block被Evict出Buffer的时候，不可以有任何线程正在访问该Block；
  + 如果某个扇区正在被读取到Buffer中，其他线程不可同时将该Block读取到扇区中；
  + 当扇区被完全加载到Buffer中之前，线程不可对其进行访问；
+ Write-Back Cache：需要使用Dirty Bit将多次磁盘写入操作集中在一起执行；
  + 只有当Block被Evict出Buffer或者系统停机的时候才将Block写入到磁盘；
  + 优化一：可以使用一个`timer_sleep`周期性地将数据写入到磁盘中；
  + 优化二：可以实现Read-ahead，当文件的某个扇区被读取的时候，自动将邻接的下一个扇区也读取到Buffer Cache中；

### Data Structures and Functions

#### Second Chance List

实现此数据结构需添加的字段包括：

1. Active Pages List：
   ~~~c
   /* 绿色队列，用于保存活跃Block，最大长度为32 */
   static struct list active_blocks;
   ~~~

2. SC Victims List：
   ~~~c
   /* 黄色队列，用于保存Evict候选Block，最大长度为32 */
   static struct list sc_blocks;
   ~~~

3. Cache Buffer：
   ~~~c
   /* Cache Buffer 数组，指向一个长度为 64 * 512 Byte 的数组 */
   uint8_t *cache_buffer;
   ~~~

4. 两个队列共用的锁：

   ~~~c
   static struct lock sc_lock; /* 队列共用锁 */
   ~~~

   

#### Buffer Cache Metadata

上述两个队列中的元素为`struct meta`，用于保存Buffer中各Block的元数据：

~~~c
/*  */
/* Cache Buffer各Block的元数据，用于维护SC队列 */
struct meta {
  	struct lock meta_lock; /* 元数据锁 */
    block_sector_t sector; /* Block对应扇区的下标 */
    int worker_cnt; /* 等待或正在使用Block的线程数 */
    bool dirty; /* Dirty Bit */
    struct rw_lock block_lock; /* Block的RW锁 */
    uint8_t *block; /* 元数据对应的Block */
    struct list_elem elem; /* 必然位于SC或者Active中 */
};
~~~

其中各字段作用如下：

- `meta_lock`：修改特定字段之前需要获取此锁；
- `sector`：Block所保存的扇区的扇区号（修改前先获取`meta_lock`）；
- `worker_cnt`：等待读写或正在读写Block中数据的线程数，用于防止意料之外的Evict（修改前先获取`meta_lock`）；
- `dirty`：Dirty Bit（修改前先获取`meta_lock`）；
- `block_lock`：Block的读写锁，读取或写入对应Block中数据之前先获取此锁；
- `block`：指向Cache Buffer中与此元数据结构体对应的Block的指针；
- `elem`：链表字段，由于此结构体必然位于`sc_blocks`或`active_blocks`中，因此可以共用此字段；

### Algorithms

总的设计原则是：

<img src=".\Project File System Design.assets\25250-1674552418284-2-1674552427068-4.jpg" alt="Image #285" style="zoom: 15%;" />

- 如果在绿色队列中找到含有目标扇区的Block：
  - 直接使用该扇区数据，无需将Block移动到队列头部；
  - 如果没有在绿色队列中找到合适的Block，切换到黄色队列头部继续；
- 如果在黄色队列中找到含有目标扇区的Block：
  - 将该Block移动到绿色队列头部；
  - 将绿队列尾部Block移动到黄色队列头部；
- 如果没有在任何队列中找到合适的Block：
  - 将黄色队列尾部元素Evict回磁盘；
  - 从磁盘中读取目标扇区的内容；
  - 将该Block的元数据以及Buffer中的数据更新为该扇区的内容；
  - 将该Block移动到绿色队列头部，将绿色队列尾部Block移动到黄色队列头部；

#### 初始化

初始化Cache Buffer的时候需要做如下工作：

+ Cache Buffer：调用`malloc`在获取大小为`64 * 512`的一块区域，作为扇区的实际缓存位置；
+ 队列相关：
  + 双向链表：
    + 查找Block的时候正向遍历，寻找Evict候选者的时候反向遍历；
    + 不过由于`list`本身就是一个环形链表，因此只需灵活使用`list.rbegin()`和`list.begin()`即可；
  + 队列初始化：
    + 初始化的时候务必使用`list_init`将链表予以初始化之后再向其中添加元素；
    + 可以使用`malloc(64 * sizeof(struct meta))`统一为元数据结构体申请空间；
    + 初始化的时候向两条链表中都添加32个`struct meta`；
    + 确保在任何情况下两条元素个数都为32，同时确保其中元素只会在两条链表中移动，不会被释放；
+ `struct meta`的初始化：
  + 锁：务必对所有锁调用`lock_init`，确保正确初始化；
  + Dirty：初始情况下`dirty`必须显式初始化为`false`，确保不会发生意外的磁盘写入；
  + `block`：令所有`meta`都有一个对应的`struct block`；
  + `worker_cnt`：初始情况下为0；
  + `elem`：将前32个`meta`添加到`active_blocks`中，后32个到`sc_blocks`中；
    + 每当需要将黄色队列的`meta`移动到绿色队列时，无需判断，直接推出绿色队列的末尾`meta`即可；
  + `sector`：初始情况下所有的`sector`都为`0xCCCCCCCC`；
    + 考虑到Pintos的磁盘容量必然小于1.6 TB，同时Pintos本身也会在释放Inode之后将其中的扇区号标记为`0xCCCCCCCC`，因此实际就可以使用这个魔数来表示处于初始状态的`meta`；

这样一来，当系统初次向Buffer Cache中添加Block时，就可以遵循惯常的执行流程：

1. 遍历两个队列，比较它们的`sector`，由于和目标扇区的下标都不一样，因此可以安全到达`sc_block`的最后一个`meta`；
2. 获取该`meta`的`meta_lock`并检查`worker_cnt`，发现其为0，因此可以直接将此Block Evict掉；
3. 由于`dirty==false`，因此无需将该Block写入磁盘；
4. 此时就可将目标磁盘中的内容读取到对应的Block中，修改各元数据字段，并进行[队列移动操作](#队列操作)；

#### Inode函数

下述所有操作队列的函数执行时必须**持有队列锁**：

- `byte_to_sector`：利用`inode`以及文件中的偏移量，计算出文件中该偏移量所在扇区的下标；
  - 首先执行[扇区查找函数](#扇区查找函数)中的逻辑，将Inode所在扇区加载到Buffer中，得到对应的`meta`；
  - 现在`meta`的`block`指向存有Inode的Cache Buffer Block，此时即可利用偏移量[找到合适的扇区下标](#Extensible Files)；
- `inode_read_at`和`inode_write_at`：在Inode的指定位置进行读写操作；
  - 调用`byte_to_sector`获取文件中指定偏移量所在扇区的下标；
  - 利用`byte_to_sector`返回的扇区下标执行[扇区查找函数](#扇区查找函数)，将与目标下标相对应的扇区加载到Buffer中；
  - 此时获取的`meta`指针应指向目标扇区位于Cache Buffer中的缓存，确保[同步](#扇区查找函数中的同步)的情况下读写Buffer即可；
- `inode_create`：创建Inode，直接将其覆写在指定扇区中；
  - 免去正向遍历，直接反向遍历，获取一个干净的`meta`；
  - 同时无需将指定扇区中的内容读取到Cache Buffer中（[只写](#读取旗标)）；
  - 注意在释放队列锁之前持有写者锁，随后再将新的Inode覆写到Cache Buffer中；
- `inode_open`：读取指定扇区中的内容，将其作为Inode解析；
  - 调用`byte_to_sector`获取文件中指定偏移量所在扇区的下标；
  - 利用`byte_to_sector`返回的扇区下标执行[扇区查找函数](#扇区查找函数)，将与目标下标相对应的扇区加载到Buffer中；
- `inode_close`：可能需要将Inode写回到磁盘中；
  - 如果`meta.dirty == true`的话，需要将其写入到磁盘中；

#### 扇区查找函数

> 函数功能：确保指定扇区（可能是Inode，也可能是文件块）被加载到Cache Buffer中，随后返回与该扇区对应的`meta`指针
>
> 函数参数：
>
> + `sector`：待查找的扇区；
> + 读取旗标：是否要将该扇区的内容读取到`meta`中；
>
> 执行结果：
>
> + 返回值`meta`指针指向的`meta`为指定扇区中的内容；
> + `meta.worker_cnt`至少为`1`；

考虑到所有磁盘写入操作都需要经过Cache Buffer，因此无论是执行读还是写操作都需要确保目标扇区对应的`meta`位于队列中，即任何读写操作都需要前置执行此算法（需确保[同步](#扇区查找函数中的同步)）：

1. **获取队列锁**；
2. [遍历队列函数](#遍历队列函数)：判断是否已经将目标扇区加载到队列中；
3. [Evict Block函数](#Evict Block函数)：判断是否需要将某个Block Evict回磁盘；
4. **释放队列锁**；
5. **按照调用时给定的参数**，确定是否需要将目标扇区读取到Cache Buffer中；
7. 返回指向`meta`的指针；

扇区查找函数执行完毕后，指针所指向的`meta`应该满足以下两个条件：

- `worker_cnt`不为0，`sector`为目标值；
- 如果`meta`被替换之前Cache Buffer的内容并非现在的`sector`，原来的内容应该被妥善写入磁盘；
- 如果调用参数为初始化Cache Buffer，那么现在其中应该含有指定内容；
- 不持有任何锁；

随后就可以安全获取`block_lock`读取Cache Buffer中的内容了

##### 遍历队列函数

> 函数功能：正向遍历队列，查找是否已经将目标扇区加载到Cache Buffer中：
>
> - 已加载：将`meta`移动到合适位置并返回指针；
> - 未加载：返回`NULL`；

正向遍历队列，寻找是否已将目标扇区加载到队列中，比较`meta.sector`是否和目标`sector`一致，期间检查每个Block时需确保[同步](#扇区查找函数中的同步)：

- 如果`sector`等于目标值，查找成功，保持`worker_cnt`递增值并返回；
- 如果`sector`等于目标值，查找失败，递减`worker_cnt`，继续检查队列中的下一个Block；

##### Evict Block函数

> 函数功能：反向遍历队列，寻找可被移出Cache Buffer的`meta`，同时将其移动到队列中合适的位置，并将其对应的Cache Buffer Block读取为合适的值（按照读取旗标的要求）：
>
> + 符合移出条件：将`meta`各字段改为目标值，但维持Dirty Bit不变，不在此执行Write-Back操作；
> + 不符合移出条件：一直循环遍历两个队列，找到为止；
>
> 字段修改完毕之后即执行队列操作，将`meta`移动到合适的位置，随后执行磁盘操作，最后返回`meta`指针

如果没有在队列中找到合适的`meta`，那么就需要执行Evict Block操作：

1. 从`sc_blocks`末尾开始倒序遍历`meta`，Evict之前需要检查`worker_cnt`的值（需确保[同步](#扇区查找函数中的同步)）：
   + 如果`worker_cnt > 1`，那么说明现在有线程正在读取Buffer中的信息，切换到队列中的下一个`meta`；
   + 如果`worker_cnt == 1`，那么说明现在没有线程正在读取Buffer中的信息，可将该[`meta`移出队列](#磁盘操作)；
2. 如果无论是`active_blocks`还是`sc_blocks`中都没有合适的`meta`，那么重复第一步；
   - 递减某个`worker_cnt`并不需要获取队列锁，同时线程读写Cache Buffer之前必然会将其`worker_cnt`递增，不用担心该`meta`被意外Evict，因此只有线程读写完毕之后将`worker_cnt`递减，那么此线程终会检测到队列中出现了一个可被Evict的`meta`，循环遍历队列自然没有什么问题；
   - 不过或许可以在遍历整个队列，发现其中没有可供Evict的Block时，调用`thread_yield`让出CPU；

3. 根据队列类型执行合适的[队列操作](#队列操作)；
4. 执行[磁盘操作](#磁盘操作)；
5. 返回`meta`指针

###### 队列操作

根据`meta`所处队列的不同，应该如何处理该`meta`也有所不同：

1. 如果`meta`位于绿色队列，无需在队列中移动`meta`；
2. 如果`meta`位于黄色队列，将其移到绿色队列头部，将绿色队列尾部`meta`移动到黄色队列头部；

###### 磁盘操作

> 功能：处理`meta`指针指向的`meta`，将磁盘扇区中的内容读取到对应的Cache Buffer Block中：
>
> + `meta.dirty`：如果该`meta`的`dirty==true`，需要先将Cache Buffer Block中的数据写入到磁盘中；
> + `sector`：需要在`meta.dirty`检查完毕后将`sector`替换为目标中的`sector`；
> + `worker_cnt`：无需修改；
> + 读取旗标：根据读取旗标的不同，决定是否需要将磁盘扇区中的内容读取到Cache Buffer中；
>
> 前置要求：
>
> + 持有`meta_lock`；

除了上面提到的操作，还有一些同步性问题需要注意：

+ 持有`meta_lock`时应该执行如下任务：
  + 检查`meta.dirty`，如果需要将旧内容写入到磁盘中，还需要保存此刻的`meta.sector`；
  + 将`meta.sector`替换为目标中的`sector`；
+ 释放`meta_lock`；
+ 根据读取旗标，获取写者锁或读者锁；
+ 确保满足[如下条件](#扇区查找函数中的同步)时，释放队列锁；
+ 在Cache Buffer中进行数据读写；

###### 读取旗标

如果是直接覆写目标扇区所有内容的话，不需要将扇区中的内容读取到Cache Buffer中：

1. `inode_write_at` 判断需要覆盖整个扇区所有内容时，无需预先读取；
2. `inode_create` 会直接将大小为一个扇区的Inode覆写到磁盘中，也无需预先读取；

另外，只写流程实际指的是读取旗标为`false`，也就是调用Evict Block函数腾出一个`meta`，但是不把磁盘中的数据读取到其中

#### Free Map

`free_map_file`在分配扇区（`free_map_allocate`）以及释放扇区（`free_map_release`）时都会被写入。文件系统被打开时（`filesys_init`）会被打开（`free_map_init, free_map_open`），文件系统被关闭时（`filesys_done`）才会被关闭（`free_map_close`）

不过Free Map文件读写实际也是靠着`inode_read_at`来的，因此只需修改这几个对应的函数即可

#### 停机

停机时，需要遍历两条队列，将其中所有的Dirty Block写入到磁盘中。此外也需要将Free Map写入到磁盘中

### Synchronization

所有可能被多线程访问的队列可归类如下：

- 两条队列：寻找队列元素或者操作队列元素之前需要获取队列共用锁，实际读取其中某个Block时无需获取；
- `meta`：读取及修改`meta`中非`elem`之外的字段需要先获取`meta_lock`；
- Cache Buffer：读取Buffer之前需要获取对应Block的读者锁，修改Buffer之前需要获取写者锁；
- Free Map：考虑到所有针对内存中的Free Map的操作都是原子的，因此不需要为Free Map添加同步措施；

还有一些关于锁的额外说明：

- 队列锁：线程不可在持有队列锁时执行磁盘IO，其他要求见下文[同步](#扇区查找函数中的同步)；

- `meta_lock`：
  - 用于确保线程对`meta`中各字段的修改始终原子：必须在持有锁时才能读取或修改`meta`中的字段；
  - 不保护线程对`meta.block`的IO：执行磁盘IO时**不可**持有此锁；

- `block_lock`：用于确保`meta.block`中的数据始终为合法值，尤其注意防止线程在其中读取到垃圾；

#### 同步检查`meta`

为确保对`meta`的检查始终线程安全，防止下列情况出现：

- 如果在释放队列锁之后再获取`meta_lock`，有可能导致另一线程读取到不完整的Block；
- 如果在执行磁盘IO时仍持有队列锁，系统可能会变得很慢；
- 如果在检查`meta`时不持有队列锁，那么有可能刚找到合适的`meta`就被抢先并替换掉这个`meta`了，恢复上下文时`meta`不再指向先前的扇区了；

为此，检查`meta`内字段时需**始终持有队列锁**

#### 扇区查找函数中的同步

首先，由于无论是检查`meta`中的字段，还是Evict某个Block，抑或是进行数据的读写操作，都需要先获取`meta_lock`. 此外，一旦释放队列锁，那么其他线程就有可能开始遍历队列，也就必须要确保其他线程遍历队列时不会对现在已经找到的`meta`造成影响。因此这么多同步原则实际是为了实现如下几点：

- 遍历队列时，按照遍历的方向以及目的的不同，对其中数据的处理也有所不同：
  - 正向遍历：比较各`meta.sector`，寻找之前是否将相关下标的扇区读取到Cache Buffer中；
    - 执行所有检查操作时，必须持有`meta_lock`；
    - 如果`sector`匹配成功，那么递增`worker_cnt`，随后即可释放`meta_lock`；
    - 如果`sector`匹配失败，那么释放`meta_lock`，继续检查队列中其他`meta`；
  - 反向遍历：比较各`meta.worker_cnt`，寻找队列中第一个可以被移除的`meta`；
    - 执行所有检查操作时，必须持有`meta_lock`；
    - 如果检查发现`worker_cnt == 0`，说明没有任何一个线程正在读取此`meta`，可安全替换；
    - 如果检查发现`worker_cnt != 0`，说明有线程正在读取此`meta`，继续检查队列中其他`meta`；
- 如果要替换掉某一个`meta`，为了防止在替换中途此`meta`被其他线程读取，需要确保如下同步措施：
  - 必须持有队列锁以及`meta_lock`，但是将所有字段（除Cache Buffer）都修改过后即可释放`meta_lock`；
  - 确保释放队列锁时，下文“队列锁被释放时...”中的条件都被满足；
  - 释放队列锁之后，将新数据读取到Cache Buffer之前，如果之前的Dirty Bit为`true`，**需要先将`meta`之所含扇区的缓存数据写入到磁盘中**，随后才可读取新数据；
- 队列锁被释放时，队列中所有的`meta`都需要处于合法状态（各字段已被替换、队列操作已执行完毕）。但具体所需执行的操作在两种不同的情况下有细微的不同：
  - 如果`meta`原先就存在于队列中，那么释放队列锁之前，需确保如下两点：
    - 目标`meta`的`worker_cnt`需被递增：防止`meta`在执行`meta.block`IO的过程中被意外移除；
    - 目标`meta`已经被移动到了合适的位置：无论原先位于哪一队列中，后来都必须位于绿色队列；
  - 如果需要从磁盘中加载`meta`，那么释放队列锁之前，需确保如下三点：
    - 目标`meta`的`worker_cnt`为1：防止`meta`在执行`meta.block`IO的过程中被意外移除；
    - 目标`meta`位于绿色队列：不过如果替换了绿色队列中的`meta`，可能不需要移动队列；
    - `sector`被更新为指定值：确保之后想要读取此扇区的线程能知悉扇区已被加载；
    - 持有`meta.block`的写者锁：防止其他线程读取到垃圾值，需要先将合适的数据写入到Buffer中（见上粗体）；
- 读取或写入`meta.block`指向的Cache Buffer时：
  - 必须在持有读者锁或写者锁的情况下操作数据；
  - 操作读取数据时不可持有任何除`block_lock`之外的锁；
- 数据读取或写入完毕之后需获取`meta_lock`并将`worker_cnt`递减；


### Rationale


----------
## Extensible Files

### Data Structures and Functions

需要修改两个数据结构：

+ `inode_disk`：尽管不能将磁盘中的Inode缓存到内存中，但是将Inode读取到Cache Buffer时，可以将`meta.block`指针强制类型转换为`inode_disk`指针，从而方便地对Cache Buffer中的数据进行读写；
+ `inode`：删掉了其中的`data`字段，现在如果要读取Inode的底层表示的话，不可以直接读取`inode.data`，而是需要使用其中的`sector`执行[扇区查找函数](#扇区查找函数)了；

需要实现两个数据结构：

- `indirect_blk_t`；

+ `dbindirect_blk_t`；

#### `indirect_blk_t`

间接块，大小为`BLOCK_SECTOR_SIZE`，保存着一个`block_sector_t[128]`数组：

~~~c
struct indirect_blk_t {
   block_sector_t direct_ptr[128];
};
~~~

#### `dbindirect_blk_t`

二级间接块，大小为`BLOCK_SECTOR_SIZE`，保存着一个`indirect_blk_t[128]`数组：

~~~c
struct dbindirect_blk_t {
	block_sector_t indirect_ptr[128];
};
~~~

#### `inode_disk`

Inode的底层表示，创建时最好使用`calloc`申请空间：

~~~c
/*  */ 
struct inode_disk {
  block_sector_t start; /* First data sector. */
  off_t length;         /* File size in bytes. */
  unsigned magic;       /* Magic number. */
  block_sector_t direct_ptr[64]; /* 直接块的下标 */
  block_sector_t indirect_prt[32]; /* 间接块的下标 */
  block_sector_t dbindirect_ptr; /* 二级间接块的下标 */
  uint32_t unused[125 - 64 - 32 -1]; /* Not used. */
};
~~~

此数据结构只可能在两个情境下位于内存中：

- 将Inode加载到Cache Buffer中；
- 延长文件时需要在内存中保留Inode的副本；

#### `inode`

删除`data`，新增`length, dir`：

~~~c
struct inode {
  struct list_elem elem;  /* Element in inode list. */
  block_sector_t sector;  /* Sector number of disk location. */
  int open_cnt;           /* Number of openers. */
  bool removed;           /* True if deleted, false otherwise. */
  int deny_write_cnt;     /* 0: writes ok, >0: deny writes. */
  off_t length;         /* File size in bytes. */
  bool dir; /* 是否为目录文件 */ 
};
~~~

### Algorithms

可能执行文件延长逻辑的函数有两处：

+ `inode_create`：可以将创建文件长度为指定值的Inode这一操作，当作延长一个长度为0的文件；
+ `inode_write_at`：需要根据参数判断是否会在文件当前EOF之后写入新数据；

#### 树型寻址结构

要求：

- 文件各Block可分散在磁盘各处，无需连续；
  - 延长文件时，以单个扇区为单位调用`free_map_allocate`；
  - 当然，先尝试一下能否直接申请到所需数量的连续扇区；
- 支持最大为8 MiB的文件：
  - 扇区大小为512 Byte，`block_sector_t`所需空间为4 Byte，因此一个Indirect Block可容纳的数据量为64 KiB，Doubly-indirect Blocks为8 MiB；
  - 实现至少一个doubly-indirect blocks（8 MiB）；

##### `byte_to_sector`

获取指向`meta`的指针之后，即可将`meta.block`强制类型转换为`inode_disk`并读取其中的内容。具体实现逻辑可以照抄Discussion中的代码，功能**仅仅是计算扇区**，不要加任何多余的东西

#### 文件延长

要求：

- 只要磁盘有空余空间且文件大小不超过8 MiB，文件即可延长；
  - Inode中需要有字段表示文件当前大小；
- 可在EOF之后的位置进行数据写入，使文件延长的同时将EOF和数据写入位置之间的空间用0填充；
  - 执行`inode_write_at`中的[只写流程](#读取旗标)即可；

##### 何时延长文件？

只有在EOF之后的位置写入数据才会延长文件，其他`seek`和`read`都不会，但是这两个系统调用的应该有点小改动

`inode_write_at`将Inode加载到Cache Buffer之后，需要比对本次的写入操作是否会延长文件：

- 具体而言就是比对`inode_disk.lenth`和`offset + size`；

+ 确认需要延长文件时，在内存中维护一个Inode副本，分配磁盘新空间的时候对其进行修改，Cache Buffer中的`inode_disk`维持不变；

##### Sparse ﬁles

分配空洞部分的扇区时令它们都指向一个特殊的扇区（Sparse ﬁles扇区），其中内容全为0

此时文件Inode中和空洞部分对应的Entry保存着该扇区的下标，读取时只能读取到0

如果`inode_write_at`利用`byte_to_sector`获知扇区为Sparse ﬁles扇区的话：

+ 在`free_map`中申请一个扇区；
+ 调用[Evict Block函数](#执行Evcit Block函数)腾出一个`meta`；
+ 将`meta`中的Sparse ﬁles扇区换成新扇区的值；
+ 将`meta.block`清空；

随后就可以安全执行数据写入了

##### 如何延长文件？

核心是将**扩大文件**和**向文件中写入数据**这两个语义完全分开来看。延长文件时，仿照Dic 8中的逻辑，计算将文件拓展为指定大小所需分配的扇区数量的同时，修改Inode内存副本中的指针：

+ 延长后大小可被直接指针所覆盖：将EOF所在扇区之后的直接指针设置为Sparse ﬁles扇区；
+ 延长后大小可被间接指针所覆盖：计算所需间接块的数目，调用[间接块分配函数](#间接块分配函数)：
  + 函数会在`free_map`中请求一个扇区作为间接块；
  + 接收一个参数，用于描述需要让间接块中的多少个指针指向Sparse ﬁles扇区，其余用0填补；
+ 延长后大小不可被间接指针所覆盖：将剩余需要的空间传递给二级间接块分配函数：
  + 函数会在`free_map`中请求一个扇区作为二级间接块；
  + 接收一个参数，用于描述需要申请多少空间，随后的逻辑和第二步类似；

此时文件旧EOF和新EOF之间的扇区都指向Sparse ﬁles扇区，向其中写入数据就会触发这里的[Zero-out逻辑](#Sparse ﬁles)

##### 间接块分配函数

> 函数功能：
>
> 1. 接收一个`off_t`，计算保存这个`off_t`需要多少直接块，记作`n`；
> 2. 调用`free_map_alloc`申请一个扇区作为间接块；
> 3. 申请一个`indirect_blk_t`；
> 4. 将`indirect_blk_t[0:n)`设置为Sparse ﬁles扇区，数组其余空间用0填补；
> 5. 将`n`递减“分配的空间”（使用的直接块 * 512 Byte）并返回；
>
> 函数说明：
>
> 如果`off_t`大于64 KiB，那么`indirect_blk_t`中的所有内容都为Sparse ﬁles扇区
>
> 函数的正确调用方式是将目标所需分配空间的大小传递给此函数，直到：
>
> + 返回值为0；
> + Inode间接块指针耗尽；
>
> 随后调用二级间接块分配函数

#### 磁盘空间耗尽

- 确保磁盘空间耗尽时能回滚到系统之前的安全状态；
- Inode的状态需要始终合法；
  - 确认文件所有的新空间都分配成功之后才将Inode副本写入到Cache Buffer中；
- 避免磁盘空间泄露：如果在延长文件的过程中磁盘被耗尽了，那么程序需要比对内存中的Inode和磁盘中的Inode，确认这段时间之内分配了多少新扇区，将其悉数释放：
  - Free Map：将Free Map中新扇区的Bit置`false`；
  - Cache Buffer：Cache Buffer中可能含有这些“不上不下”的扇区，需要将它们无害化。虽然说一般情况下不进行无害化也没有关系，修改Free Map便不会导致磁盘泄露，但是Write-Behind线程可能会将这些扇区写入到磁盘中，导致一定量无谓的IO开销，因此需要将它们的`dirty`都置`false`；

- 实现系统调用`inumber(int fd)`，根据文件描述符获取Inode Number；
  - Inode Number永久性地标识一个文件**或**目录，在文件存在期间，它的存在是唯一的；
  - 使用Inode所在的扇区号作为其Inode Number实际就足够了；
  - 获取`file`中的`inode.sector`；

### Synchronization

同步相关的问题：

- 线程在延长文件时，其他线程想要读取或写入文件内容：

  > Concurrent reads are not required.

  分情况讨论：

  - 延长线程在读写线程之前执行：Inode的写者锁在延长线程手上，其他所有想要读写文件的线程只能等延长线程执行完毕；
  - 延长线程在读写线程之后执行：
    - 如果延长线程和读写线程的的读写区域并不覆盖，两者相安无事；
    - 相反，由于延长线程和读写线程读写文件块之前需要获取对应的读写锁，因此不用担心这方面的同步问题；

- 线程在延长文件时，文件的Inode被修改了：

  > If two operations are **writing to the same sector** or **extending the same ﬁle**, then they are **not** considered independent and you may serialize those operations to maintain data consistency.

  所以系统同一时间只能有一个线程对某个文件执行延长操作，防止同一时间有两个线程在修改文件。如果`inode_write`检查发现本次的写入操作需要拓展文件，那么：

  + 递增Inode的`worker_cnt`：防止在线程延长文件时，Inode被意外Evict回磁盘之后，其他线程将该Inode读取到Cache Buffer中并对其进行拓展，使两个线程同时延长一个文件；
  + 获取Inode的的写者锁：防止在线程延长文件时，其他线程执行扇区查找算法，将该Inode读取到Cache Buffer中并对其进行拓展，使两个线程同时延长一个文件；


### Rationale


----------
## Subdirectories
### Data Structures and Functions


### Algorithms


### Synchronization


### Rationale


----------
## Concept check
### Write-Behind

间歇性地将Cache Buffer中的Dirty Block写入到磁盘中

TODO：

+ 初始化文件系统的时候就创建一个Swap线程，只有在关闭文件系统的时候才令其停止执行，或许可以利用一个全局性的Flag来控制这一点；
+ 可以利用Poj 2中的`timer_sleep`函数，让线程每60秒醒来一次，遍历两个队列并将其中的Dirty Block写入内存中；
+ 有可能Swap线程检查到某个`meta`的时候该`meta`正在被其他线程读写，这时候如果执行Dirty Block Evict操作明显不妥，因此Swap检查`meta`之前需要获取`meta_lock`并检查`worker_cnt`：
  + `worker_cnt == 0 `：**不存在**正在读取或写入其中内容的线程，**可**检查`meta.dirty`；
  + `worker_cnt != 0 `：**存在**正在读取或写入其中内容的线程，跳转到队列中的下一个`meta`；
+ 因此在Swap执行数据写入之前，还需要再检查一遍`meta.dirty`的情况。只有在确认`dirty`仍旧为`true`时才执行磁盘IO。

#### Algorithms

理想状况下的Swap线程的工作流程是在遍历队列的过程中，遇到任何一个`meta.dirty == true`的`meta`都将其中的数据写入到磁盘中，写入完毕之后转而继续去处理队列中的下一个`meta`。

但是考虑到性能等多方面因素，既不能在持有队列锁的时候执行磁盘IO；也不能在将一个`meta`中的数据写入到磁盘时释放队列锁，随后读取队列中下一个`meta`的时候再去获取队列锁。

因此，如果Swap遍历队列时遇到了一个Dirty Block，仅仅是将其`worker_cnt`递增（防止意外Evict）同时将其指针记录在某个数组中。随后在释放队列锁之后，检查指针数组并将其中记录的`meta`写入到磁盘中。总之，算法的执行流程可分为两个部分：

+ 记录：将队列中所有`dirty == true`的`meta`记录在指针数组中；
+ 清空：将记录在指针数组中的`meta`写入到磁盘中；

##### 记录

尽管有可能Swap线程记录到某个`meta`的时候该`meta`正在被其他线程读写，但记录时的`meta`情况和清空时`meta`的情况可能会出现不同，因此无需将此作为记录条件。只有当`meta.dirty == true`时才将`meta`记录到数组中

###### 指针数组

考虑到将某个`worker_cnt != 0`的实际效果就相当于将这个`meta` “Pin”在了队列的指定位置中，它同时也不会被其他扇区替代掉，因此可以安全地将指向该`meta`的指针保存在数组中，无需担心之后使用该指针访问到其他扇区：

+ 由于系统中最多只有64个`meta`，因此可以在使用大小固定、位置固定的数组保存`meta`指针：
  + 初始化文件系统的时候申请一个`64 * sizeof(uint8_t*)`的堆空间，用于保存所有的`meta`指针；
  + 为确保语义正确，需要使用`calloc`而不是`malloc`申请数组；
+ 维护一个`tail`变量，用来指示数组从0开始，第一个为`NULL`的元素：
  + 数组元素下标和`meta`下标没有必然联系；
  + 记录：只要遇到一个满足条件的`meta`，就将它的指针保存到`array[tail]`中，随后执行`tail++`；
  + 清空：从`--tail`开始逐个遍历数组中的`meta`，一直递减到`tail == -1`为止，随后将`tail`设置为`0`；
  + 由于只有一个线程会处理Swap逻辑，因此`tail`以及`array`都无需加锁；

###### 同步检查

记录时的同步规则与[这里](#扇区查找函数中的同步)提到的类似，之所以在符合条件时保留递增值，是因为要防止在[间歇](#释放间歇)中此`meta`被Evict：

1. 获取`meta_lock`；
2. 递增`worker_cnt`；
3. 检查`dirty`；
4. 如果`dirty == false`递减`worker_cnt`，反之保留递增值；
5. 释放`meta_lock`；

##### 清空

等到队列被遍历完全，释放队列锁之后，就可以利用之前保存在数组中的信息，逐个将之前记录的`meta`的Cache Buffer写入到磁盘中了（注意维护`tail`的使用逻辑）。不过考虑到[间歇](#释放间歇)中提到的情况，执行磁盘操作之前需要确保两个条件都被满足：

+ `dirty == true`：只有在间歇过后数据依旧是脏的情况下，才执行磁盘IO；
+ `worker_cnt == 1`：只有在自己是唯一进行数据读取的线程时，才执行磁盘IO；
  + 如果检查发现不是`1`，记得将`worker_cnt`递减；

随后，可执行下列操作序列，将Cache Block Buffer写入到磁盘：

1. **将`dirty`设置为`false`**；
2. 释放`meta_lock`；
3. 获取读者锁；
4. 将Cache Buffer Block写入磁盘；
5. 释放读者锁；
6. 获取`meta_lock`；
7. 将`worker_cnt`递减；
8. 释放`meta_lock`；

###### 释放间歇

注意，有可能在Swap释放队列锁，开始处理指针数组的间歇，之前记录的某些`meta`已经被写入到了磁盘中，`dirty`变为了`false`，这时候就没必要将该`meta`写入到磁盘中了

还有一种可能是在释放间歇中，其他线程开始读取`meta`中的数据，如果这时候再执行磁盘IO自然就不太妥当

另外，无需担心在这个间歇中又产生了新的脏数据，Swap算法执行的目的仅仅是为了提高系统的可靠性，无需对Dirty Block做出任何其他保证；

### Read-ahead

读取文件中Block的时候，自动将顺序的下一个Block异步读取到Cache Buffer中

TODO:

+ 可以利用Inode中的文件大小，判断当前Block是否为文件中的最后一个Block，只有在不是的情况下才执行Read-ahead；
+ 异步读取被执行时下一个Block有可能已经位于Cache Buffer中了，因此需要检查一下；
+ 可能会有多个线程在读取不同文件中的数据，各自产生不同的异步读取请求，因此需要使用请求队列。每当线程执行Block获取操作时需要向请求队列中压入一个请求；
+ 系统中只有一个请求处理线程，不应该每出现一个Read-ahead请求就创建一个线程；
+ 可以使用条件变量作为普通线程向请求处理线程通讯的方式，执行条件是请求队列非空；
+ 尽管是异步读取，但本质还是从文件中读取指定扇区，因此可以执行与`inode_read_at`类似的逻辑；

#### 生产者消费者问题

请求队列的特征可以被归纳为：

+ 多个线程提交请求，一个线程处理请求；
+ 容量为N的请求队列；
+ 如果因队列未满而提交失败，生产者无需等待队列非满；

##### C++实现

下为C++代码实现的一个环形缓存（[circular buffer](https://en.wikipedia.org/wiki/Circular_buffer)）：

~~~c++
class Bounded_buffer {
  Portion buffer[N];    // 0..N-1
  unsigned head, tail;  // 0..N-1
  unsigned count;       // 0..N
  std::condition_variable nonempty, nonfull;
  std::mutex mtx;
public:
  void append(Portion x) {
    std::unique_lock<std::mutex> lck(mtx);
    nonfull.wait(lck, [&]{ return !(N == count); });
    assert(0 <= count && count < N);
    buffer[tail++] = x;
    tail %= N;
    ++count;
    nonempty.notify_one();
  }
  Portion remove() {
    std::unique_lock<std::mutex> lck(mtx);
    nonempty.wait(lck, [&]{ return !(0 == count); });
    assert(0 < count && count <= N);
    Portion x = buffer[head++];
    head %= N; 
    --count;
    nonfull.notify_one();
    return x;
  }
  Bounded_buffer() {
    head = 0; tail = 0; count = 0;
  }
};
~~~

各变量作用如下：

+ `head, tail`：分别表示缓存中元素所在的起始位置和结束位置，是一个左闭右开区间（$[head, tail)$）；
+ `count`：表示缓存中元素个数，存在关系`head + count == tail`；
+ `nonempty, nonfull`：条件变量队列，含义是表示线程现在正在等待的条件。如果线程对`nonempty`调用`wait`，那么说明线程等待队列非空，此时调用`nonempty.signal()`即可唤醒此线程。`nonfull`亦然；
+ `mtx`：条件变量对应的锁，操作两个条件变量以及读取上述所有字段之前，必须持有该锁；

算法核心：

+ `head`指向缓存中第一个非空位置，`tail`指向`head`之后的第一个空的位置（环形视角）；
+ 操作`head, tail`之后需要对其执行`%= N`赋值，从而计算出其被修改过后在环中的正确下标；
+ 条件变量的判断条件可以利用对`count`的判断来实现；

##### 定制实现

Read-ahead中需要用到的缓存需要在上述实现基础上稍加改动，只需确保生产者线程不会等待队列非空：

+ 如果`append`函数检查条件变量不满足，那么不是执行`wait`而是释放锁之后直接`return`；
+ 不再需要`nonfull`条件变量，只留下一个`nonempty`即可；

同时处理者线程需要在`remove`函数之外包装死循环，不断将缓存中的请求移除并对其进行处理

#### 请求处理

虽然有时候线程调用的`inode_read_at`和`inode_write_at`是需要对文件中一连串的Block进行读取和写入的，但从具体的一个个Block被读取到Cache Buffer，到函数处理其中的内容，最后在读取下一个Block的这段时间之内，如果能将这些Block的下一个Block读取到Cache Buffer中，确实能提升一定的IO性能。

##### 请求时机

换而言之，线程计算出它接下来所要读取的文件块之后，就应当发出Read-ahead请求，令消费者读取目标文件块的下一个文件块。注意，线程发出Read-ahead请求之前，需要持有队列锁，以免目标文件块被延迟读取到Buffer中。此后，即可执行[扇区查找函数](#扇区查找函数)，将指定Block读取到Cache Buffer中。

##### 请求内容

Read-ahead处理请求的整体原则应当是如果线程想要将要文件中的某个文件块，同时可以预知到线程将读取该文件块的下一个文件块，那么将下一个文件块读取到Cache Buffer中，除此之外的什么事情都不要做。举例来说，就算可以预测到线程将要读取整个文件，也不要在线程读取文件中首个文件块的时候就将文件中所有的文件块都加载到Cache Buffer中，而是在线程读取一个文件块的时候，将其下一个文件块异步读取到Cache Buffer中。

##### 请求条件

具体来说就是在线程利用文件偏移量计算对应扇区的同时，将该偏移量递增512并判断这个值是否超过文件大小或偏移量 + 本次将读取的数据量。如果没有超过，那么使用递增后的偏移量计算对应扇区并调用[扇区查找函数](#扇区查找函数)
