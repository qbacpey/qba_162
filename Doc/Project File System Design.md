# Project File System Design 

----------
## Buffer Cache

所有涉及磁盘扇区的读写操作都需要经过Cache Buffer来完成。也就是说，无论是读还是写，首先都需要将扇区读取到Cache Buffer中作为Cache Block之后，再对其进行修改或读取其中的数据，随后有需要或者Cache Buffer没有空间了，就将Block写回到磁盘中。具体来说，有如下几项要求：
+ 置换算法：Second-chance List（VAX/VMS）即可；
+ 容量：Buffer最大可容纳64个扇区，维护数据结构所需的元数据不计入其中；
+ 同步：线程对Cache Buffer的访问应该被同步；
  + 线程向某个Buffer Block中写入数据或从中读取数据时，其他线程不可将该Block Evict回磁盘；
  + 当某个Block被Evict出Buffer的时候，不可以有任何线程正在访问该Block；
  + 如果某个扇区正在被读取到Buffer中，其他线程不可同时将该B扇区读取到Buffer Cache中；
  + 扇区被完全加载到Buffer中之前，线程不可对其进行访问；
+ Write-Back Cache：需要使用Dirty Bit将多次磁盘写入操作集中在一起执行；
  + 只有当Block被Evict出Buffer或者系统停机的时候才将Block写入到磁盘；
  + 优化一：可以使用一个`timer_sleep`周期性地将数据写入到磁盘中；
  + 优化二：可以实现Read-ahead，当文件的某个扇区被读取的时候，自动将邻接的下一个扇区也读取到Buffer Cache中；

下述所有操作队列的函数执行时必须**持有队列锁**：

- `byte_to_sector`：利用`inode`以及文件中的偏移量，计算出文件中该偏移量所在扇区的下标；
  - 首先执行[扇区查找函数](#扇区查找函数)中的逻辑，将Inode所在扇区加载到Buffer中，得到对应的`meta`；
  - 现在`meta`的`block`指向存有Inode的Cache Buffer Block，此时即可利用偏移量[找到合适的扇区下标](#Extensible Files)；
- `inode_read_at/inode_write_at/inode_open`：在Inode的指定位置进行读写操作；
  - 调用`byte_to_sector`获取文件中指定偏移量所在扇区的下标；
  - 利用`byte_to_sector`返回的扇区下标执行[扇区查找函数](#扇区查找函数)，将与目标下标相对应的扇区加载到Buffer中；
  - 此时获取的`meta`指针应指向目标扇区位于Cache Buffer中的缓存，确保[同步](#扇区查找函数中的同步)的情况下读写Buffer即可；
- `inode_create`：创建Inode，直接将其覆写在指定扇区中；
  - 免去正向遍历，直接反向遍历，获取一个干净的`meta`；
  - 同时无需将指定扇区中的内容读取到Cache Buffer中（[只写](#读取旗标)）；
  - 注意在释放队列锁之前持有写者锁，随后再将新的Inode覆写到Cache Buffer中（`dirty=true`）；
- `inode_close`：可能需要将Inode写回到磁盘中；
  - 如果`meta.dirty == true`的话，需要将其写入到磁盘中；

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
~~~

其中各字段作用如下：

- `meta_lock`：修改`meta`结构体的字段之前需要获取此锁；
- `sector`：Block所保存的扇区的扇区号（修改前先获取`meta_lock`）；
- `worker_cnt`：等待读写或正在读写Block中数据的线程数，用于防止意料之外的Evict（修改前先获取`meta_lock`）；
- `dirty`：Dirty Bit（修改前先获取`meta_lock`）；
- `block_lock`：Block的锁，读取或写入对应Block中数据之前先获取此锁；
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

#### 扇区查找函数

> 函数功能：确保指定扇区被加载到Cache Buffer中，将其对应的Cache Buffer Block读取为合适的值（按照读取旗标的要求），返回与该扇区对应的`meta`指针
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
3. [Evict Meta函数](#Evict Meta函数)：判断是否需要将某个Block Evict回磁盘；
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

如果在黄色队列中找到了`sector`，需要执行队列移动操作

##### Evict Meta函数

> 函数功能：反向遍历队列，寻找可被移出Cache Buffer的`meta`，同时将其移动到队列中合适的位置：
>
> + 符合移出条件：将`meta`各字段改为目标值，但维持Dirty Bit不变，不在此执行Write-Back操作；
> + 不符合移出条件：一直循环遍历两个队列，找到为止；
>
> 字段修改完毕之后即执行队列操作，将`meta`移动到合适的位置，最后返回`meta`指针

如果没有在队列中找到合适的`meta`，那么就需要执行Evict Meta操作：

1. 从`sc_blocks`末尾开始倒序遍历`meta`，Evict之前需要检查`worker_cnt`的值（需确保[同步](#扇区查找函数中的同步)）：
   + 如果`worker_cnt > 0`，那么说明现在有线程正在读取Buffer中的信息，切换到队列中的下一个`meta`；
   + 如果`worker_cnt == 0`，那么说明现在没有线程正在读取Buffer中的信息，可将该[`meta`移出队列](#磁盘操作)；
2. 如果无论是`active_blocks`还是`sc_blocks`中都没有合适的`meta`，那么重复第一步；
   - 递减某个`worker_cnt`并不需要获取队列锁，同时线程读写Cache Buffer之前必然会将其`worker_cnt`递增，不用担心该`meta`被意外Evict，因此只有线程读写完毕之后将`worker_cnt`递减，那么此线程终会检测到队列中出现了一个可被Evict的`meta`，循环遍历队列自然没有什么问题；
   - 不过或许可以在遍历整个队列，发现其中没有可供Evict的Block时，调用`thread_yield`让出CPU；

3. 根据队列类型执行合适的[队列操作](#队列操作)；
4. 执行[磁盘操作](#磁盘操作)；
5. 返回`meta`指针

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

#### 队列操作

根据`meta`所处队列的不同，应该如何处理该`meta`也有所不同：

1. 如果`meta`位于绿色队列，无需在队列中移动`meta`；
2. 如果`meta`位于黄色队列，将其移到绿色队列头部，将绿色队列尾部`meta`移动到黄色队列头部；

#### 读取旗标

如果是直接覆写目标扇区所有内容的话，不需要将扇区中的内容读取到Cache Buffer中：

1. `inode_write_at` 判断需要覆盖整个扇区所有内容时，无需预先读取；
2. `inode_create` 会直接将大小为一个扇区的Inode覆写到磁盘中，也无需预先读取；

另外，只写流程实际指的是读取旗标为`false`，也就是调用Evict Meta函数腾出一个`meta`，但是不把磁盘中的数据读取到其中

#### Free Map

`free_map_file`在分配扇区（`free_map_allocate`）以及释放扇区（`free_map_release`）时都会被写入。文件系统被打开时（`filesys_init`）会被打开（`free_map_init, free_map_open`），文件系统被关闭时（`filesys_done`）才会被关闭（`free_map_close`）

不过Free Map文件读写实际也是靠着`inode_read_at`来的，因此只需修改这几个对应的函数即可

#### 停机

停机时，需要遍历两条队列，将其中所有的Dirty Block写入到磁盘中。此外也需要将Free Map写入到磁盘中

同时记得将所有buffer都释放掉

### Synchronization

所有可能被多线程访问的队列可归类如下：

- 两条队列：寻找队列元素或者操作队列元素之前需要获取队列共用锁，实际读取其中某个Block时无需获取；
- `meta`：读取及修改`meta`中非`elem`之外的字段需要先获取`meta_lock`；
- Cache Buffer：读写Buffer之前需要获取对应Block的`block_lock`（位于`meta`中）；
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

### 总结

支持多线程同时读写的文件系统缓存（Buffer Cache）最多可缓存64个扇区的内容：

+ 数据结构：
  + `active_blocks`与`sc_block`：用于保存缓存中各个缓存块的元数据，个中元素为`meta_t`；
    + 原身是被VAX计算机所采用的页置换算法：Second-chance List，可以被认为是一种LRU近似算法；
    + 系统会维护两个队列：绿色队列和黄色队列；
    + 系统如果想读取某个扇区的数据的话，首先会去绿色队列中查找，如果该扇区已经被加载到了某个位于绿色队列中的缓存块的话，立即返回该缓存块；
    + 接下来会去黄色队列中查找，如果在其中找到目标缓存块，那么会将该缓存块移动到绿色队列顶部，随后将绿色队列尾部的块移动到黄色队列的顶部；
    + 如果也没有在黄色队列中找到，那么就会在绿色队列顶部腾出一个缓存块，将目标扇区的数据读取到该缓存块中，随后将绿色队列尾部的块移动到黄色队列顶部，最后将黄色队列尾部的缓存块置换回磁盘中；
  + `meta_t`：64个Block的元数据，包含引用计数、扇区号，锁等等；
  + `cache_buffer`：数据的实际缓存位置，整体长度为64 * 512Byte；
    + `meta_t`中会包含一个指向此数组的指针；
+ 同步措施：
  + `queue_lock`：查找队列元素、移动队列元素时需持有此锁；
  + `meta_lock`：读写`meta_t`中字段的时候需持有此锁；
  + `block_lock`：读写`block`中缓存的数据的时候需要获取此锁；
+ 问题与相关解决方案：
  + 如何防止当线程正在读写某个缓存块中数据时，该缓存块被置换到文件系统中？
    + 与每个缓存块对应的`meta_t`中有一个叫做`worker_cnt`的字段，表示现在系统中正在读写该缓存块的线程的数目；
    + 线程读写缓存块之前，会将`worker_cnt`递增。读写完毕之后，会将该字段递减；
    + 系统只会将该字段等于零的缓存块置换到磁盘中，以此确保不会干扰其他线程的读写；
    + 该字段的原子性通过`meta_lock`确保；
  + 线程检查某个扇区是否被读取到缓存中的流程是（或者说想要读取某个扇区中的内容）？
    1. 获取队列锁，正向遍历两个缓存块队列：检查是否已经将目标扇区读取到缓存中；
       - 在绿色队列中找到目标扇区：将其`worker_cnt`递增并返回；
       - 在黄色队列中找到目标扇区：将其`worker_cnt`递增并将其移动到绿色队列的头部，随后返回；
    2. 没有在任何一个队列中找到：需要置换某个缓存块；
       + 反向遍历（从黄色队列尾部开始直到绿色队列首部）两个队列；
       + 寻找一个可被置换的缓存块（缓存块的`worker_cnt`应当为`0`，表示没有任何线程正在读写其中数据），更新该缓存块元数据的扇区号并递增`worker_cnt`；
       + 如果该缓存块是Dirty的，将其中数据写回到磁盘中；
       + 从磁盘中读取目标扇区的数据，写入到该缓存块中；
       + 根据该缓存块当前所在位置，将其移动到合适的队列中；
    3. 线程读写缓存块中的内容；
    4. 递减`worker_cnt`；
  + 为什么线程遍历缓存块`meta_t`的时候也需要持有队列锁？
    + 如果当前缓存块不是目标缓存块，那么需要检查队列中下一个缓存块，一样需要获取队列锁；
    + 如果当前缓存块是目标缓存块，有可能需要将`meta_t`移动到合适的队列中，一样需要获取队列锁；
    + 相比于重复获取、释放锁的开销，不如长期持有队列锁；
  + 线程读取扇区内容时的获锁序列是？
    + 持有队列锁的情况下，遍历队列中所有元素，检查其中字段之前需要获取`meta_lock`；
    + 如果需要将缓存块中原有内容写入磁盘，或从磁盘中读取目标扇区数据的话，需要获取`block_lock`；
    + 执行磁盘IO的同时不能持有队列锁
    + 查找到目标缓存块或将目标扇区读取到缓存块之后，线程需要在持有`meta_lock`和`block_lock`的情况下将`worker_cnt`递增，随后才可以释放上述两把锁。此时缓存块中所保存的数据即是目标扇区中的内容；
    + 随后，线程必须在获取`block_lock`情况下读写缓存块中的数据；
    + 如果线程确定不再需要读写缓存块中的数据，那么可以获取`meta_lock`将`worker_cnt`递减；


----------
## Extensible Files

需要对`inode_write_at`的实现稍加修改：

1. 读取Inode时检查是否需要拓展文件，如果需要则获取Inode的写者锁；
2. 调用[间接块Resize函数](#间接块Resize函数)将文件延长为目标大小；
3. 此时文件已被扩大到足以容纳所有待写入内容，可直接执行原有写入新数据的逻辑；

### Data Structures and Functions

需要修改两个数据结构：

+ `inode_disk`：尽管不能将磁盘中的Inode缓存到内存中，但是将Inode读取到Cache Buffer时，可以将`meta.block`指针强制类型转换为`inode_disk`指针，从而方便地对Cache Buffer中的数据进行读写；
+ `inode`：删掉了其中的`data`字段，现在如果要读取Inode的底层表示的话，不可以直接读取`inode.data`，而是需要使用其中的`sector`执行[扇区查找函数](#扇区查找函数)了；

需要实现两个数据结构：

- `idr_block`；

+ `dbi_block_t`；

#### `idr_block`

间接块，大小为`BLOCK_SECTOR_SIZE`，保存着一个`block_sector_t[128]`数组：

~~~c
struct idr_block {
   block_sector_t direct_ary[128];
} idr_block_t;
~~~

#### `dbi_block_t`

二级间接块，大小为`BLOCK_SECTOR_SIZE`，保存着一个`idr_block[128]`数组：

~~~c
typedef struct dbi_blk {
  block_sector_t arr[128];
} dbi_blk_t;
~~~

#### `inode_disk`

Inode的底层表示，创建时最好使用`calloc`申请空间：

~~~c
struct inode_disk {
  off_t length;                                 /* File size in bytes. */
  unsigned magic;                               /* Magic number. */
  enum file_type type;                          /* 文件类型 */
  block_sector_t dr_arr[INODE_DIRECT_COUNT];    /* 直接块的下标 */
  block_sector_t idr_arr[INODE_INDIRECT_COUNT]; /* 间接块的下标 */
  block_sector_t dbi_arr;                       /* 二级间接块的下标 */
  uint32_t unused[125 - INODE_DIRECT_COUNT - INODE_INDIRECT_COUNT - INODE_DB_INDIRECT_COUNT]; /* Not used. */
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

> 核心是将**扩大文件**和**向文件中写入数据**这两个语义完全分开来看

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

分配空洞部分的扇区时令它们都指向一个特殊的扇区（Sparse ﬁles扇区，扇区号为`3`），其中内容全为0

此扇区不会出现在Cache Buffer中，仅仅只是在`free_map`中将这个扇区号提前预留，任何针对此扇区的读取和写入操作都**不会**经过Cache Buffer。具体来说，如果`inode_read_at`中发现指定扇位置对应的直接块为`3`，那么直接清空传入的`buffer`即可

此时文件Inode中和空洞部分对应的Entry保存着该扇区的下标，读取时只能读取到0

如果`inode_write_at`利用`byte_to_sector`获知扇区为Sparse ﬁles扇区的话：

+ 在`free_map`中申请一个扇区；
+ 调用[Evict Meta函数](#执行Evcit Block函数)腾出一个`meta`；
+ 将`meta`中的Sparse ﬁles扇区换成新扇区的值；
+ 将`meta.block`清空；

随后就可以安全执行数据写入了

##### 如何延长文件？

需将：

+ 旧EOF与写入起点之间的扇区使用Sparse ﬁles扇区进行填补，将；
+ 写入起点与新EOF之间的扇区使用实际的Free Sector进行填补；

考虑到这两项工作的相似性，可以使用[间接块Resize函数](#间接块Resize函数)的实际分配旗标统一处理它们

延长文件时，仿照Dic 8中的逻辑，计算将文件拓展为指定大小所需分配的扇区数量的同时，修改Inode内存副本中的指针：

+ 延长后大小可被直接指针所覆盖：将EOF所在扇区之后的直接指针设置为Sparse ﬁles扇区；
+ 延长后大小可被间接指针所覆盖：计算所需间接块的数目，调用[间接块Resize函数](#间接块Resize函数)；
+ 延长后大小不可被间接指针所覆盖：将剩余需要的空间传递给二级间接块Resize函数；
  + 函数会在`free_map`中请求一个扇区作为二级间接块；
  + 接收一个参数，用于描述需要申请多少空间，随后的逻辑和第二步类似；

此时文件旧EOF和新写入起点之间的扇区都指向Sparse ﬁles扇区，向其中写入数据就会触发这里的[Zero-out逻辑](#Sparse ﬁles)

##### 间接块Resize函数

> 函数功能：接收一个`off_t`，计算保存这个`off_t`需要多少直接块，记作`n`，同时修改参数指针指向的`idr_block`。执行失败时确保不会新分配任何扇区
>
> + 根据“实际分配旗标”，使用Sparse ﬁles扇区或空余扇区填满`[0, n)`：
>   + `true`：间接块中所有扇区指针都来自`free_map`，不使用Sparse ﬁles扇区；
>   + `flase`：可使用Sparse ﬁles扇区代替实际空余扇区；
> + 确保`[n, 128)`之间的所有数组元素都为`0`，如果该位置原先存在数据：
>   + Sparse ﬁles扇区：直接赋值为`0`即可；
>   + 实际扇区：把它从`free_map`中释放掉；
> + 函数不会对底层直接块进行读写操作，只会调用`free_map`来申请新的扇区空间，不会执行`create_write_sector`清空其中内容：
>   + EOF所在扇区的“无人区”需要由`inode_write_at`确保清空；
>   + 新写入位置所在扇区之前的”无人区“也需要由该函数确保清空；
>   + 需要注意EOF和新写入位置之间是“无人区”的情况；
>
> 函数参数：
>
> + `idr_block *`：一个指向Cache Buffer的指针，内容为间接块的`meta`；
> + 实际分配旗标：使用Sparse ﬁles扇区或空余扇区填充新扇区；
>   + 执行过程中如果申请失败，需要将新申请的所有扇区都释放；
> + `off_t`：间接块的新大小，用于计算需要多少个直接块存放此值；
>   + 等于`0`说明需要释放此间接块中的全部扇区；
>   + 位于中间值时向上取整并按照上面提到的逻辑进行合理的Resize操作；
>   + 等于`64 * 1024`或以上，说明需要将间接块中的所有地方都填满；
>
> 返回值：执行成功与否

无论是分配间接块还是释放间接块中都可以调用此函数，但是此函数必须在如下场景中被调用：

+ Caller申请新扇区并调用Evict Meta函数腾出一个新的`meta`，将新的扇区下标赋值给`meta`；
+ 确保读写`meta`时同步（持有读者锁之后再调用此函数）；
+ 调用此函数，利用`meta.block`将Cache Buffer当成`idr_block`并对其进行修改（写入完毕需要递减`worker_cnt`）；
  + 如果成功，重复此过程直到完成申请目标；
  + 如果失败：
    + 首先释放该`meta`的扇区（操作Free Map即可）；
    + 随后调用此函数，释放所有新申请的间接块；
    + 重复上述两步直到回到原点；

无论是Inode本体申请间接块，还是二级间接块申请间接块，亦或者是Inode申请二级间接块，都需要遵循上述逻辑

###### 扇区数量计算

总体而言，使用**所需扇区数量**作为Resize函数的计算标准以及量纲

以拓展文件时，计算所需Sparse扇区数量以及实际空余数量扇区为例：

+ 关键位置：
  + EOF扇区位置：由于分配新扇区时，EOF所在扇区无需再次分配，EOF需使用`inode.length`向上取整512，再除以512；
  + Sparse扇区数量：由于Sparse扇区是EOF所在扇区与`offset`所在扇区的差值，因此如果想要计算Sparse扇区数量，需要使用`offset`向下取整512并除以512，再减去EOF的扇区位置；
  + 实际空余扇区数量：需要使用新EOF扇区位置（`offset + size`向上取整并除以512）减去`offset`所在扇区（`offset + size`向下取整并除以512）；

#### 磁盘空间耗尽

要求：

- 确保磁盘空间耗尽时能回滚到系统之前的安全状态；
- Inode的状态需要始终合法；
  - 确认文件所有的新空间都分配成功之后才将Inode副本写入到Cache Buffer中；
- 避免磁盘空间泄露；
  
- 实现系统调用`inumber(int fd)`，根据文件描述符获取Inode Number；
  - Inode Number永久性地标识一个文件**或**目录，在文件存在期间，它的存在是唯一的；
  - 使用Inode所在的扇区号作为其Inode Number实际就足够了；
  - 获取`file`中的`inode.sector`；

##### 事务化Inode写入

需要延长文件时，复制一份Cache Buffer中的Inode作为更改的实际应用对象。只有在磁盘分配全程没有出现错误时才使用这份副本

写入完毕之后记得释放，同时写入时需要持有写者锁

##### 避免磁盘空间泄露

如果在延长文件的过程中磁盘被耗尽了，那么程序需要比对内存中的Inode和磁盘中的Inode，确认这段时间之内分配了多少新扇区，将其悉数释放：

- Free Map：将Free Map中新扇区的Bit置`false`；
- Cache Buffer：分离了空间分配与磁盘写入的语义，因此无需担心Cache Buffer中出现无意义的`meta`；

如果在执行某个[间接块Resize函数](#间接块Resize函数)时新扇区分配失败了，那么需要参考[间接块Resize函数](#间接块Resize函数)中的逻辑释放Block

### Synchronization

线程在延长文件时，其他线程想要读取或写入文件内容：

> Concurrent reads are not required.

分情况讨论：

- 延长线程在读写线程之前执行：Inode的写者锁在延长线程手上，其他所有想要读写文件的线程只能等延长线程执行完毕；
- 延长线程在读写线程之后执行：
  - 如果延长线程和读写线程的的读写区域并不覆盖，两者相安无事；
  - 相反，由于延长线程和读写线程读写文件块之前需要获取对应的读写锁，因此不用担心这方面的同步问题；

线程在延长文件时，文件的Inode被修改了：

> If two operations are **writing to the same sector** or **extending the same ﬁle**, then they are **not** considered independent and you may serialize those operations to maintain data consistency.

所以系统同一时间只能有一个线程对某个文件执行延长操作，防止同一时间有两个线程在修改文件。如果`inode_write`检查发现本次的写入操作需要拓展文件，那么：

+ 递增Inode的`worker_cnt`：防止在线程延长文件时，Inode被意外Evict回磁盘之后，其他线程将该Inode读取到Cache Buffer中并对其进行拓展，使两个线程同时延长一个文件；
+ 获取Inode的的写者锁：防止在线程延长文件时，其他线程执行扇区查找算法，将该Inode读取到Cache Buffer中并对其进行拓展，使两个线程同时延长一个文件；

### Rationale

### 总结

要求：

+ 文件索引树；
  + 文件各Block可分散在磁盘各处，无需连续；
  + 结构类似BSD，一个扇区大小为512 Byte，需支持大小至少为8MiB的文件；
  + `inode_disk`（也就是Inode被保存在磁盘时的数据结构）中包含三种不同的指针区域（这里的指针和通常的定义有所不同，它们的值并不是内存地址，而是扇区标号），相互之间形成了某种嵌套关系：
    + 直接指针区域：直接保存着数据所在扇区的扇区号；
    + 间接指针区域：保存着间接块所在的扇区号，间接块是一个被当作直接指针区域的扇区；
    + 二级间接指针区域：保存着二级间接块所在的扇区号，二级间接块是一个被当作间接指针区域的扇区；
+ 文件可拓展：
  + 如果尝试向文件现有EOF之后的位置执行写入操作，文件将被自动拓展到对应大小；
+ 处理磁盘空间不足时的情况：
  + 如果文件拓展失败，那么文件Inode中任何内容都不应该被修改；
  + 换而言之，如果在拓展文件时剩余可用扇区数量不足，那么需要释放掉所有新分配的扇区，回滚所有修改操作；

实现方式：

+ 难点在于扩大文件时所采用的接口以及代码复用：
  + 同一个Inode中有三种不同的指针，指针各自指向的数据结构各有不同；
  + 文件原来的EOF可能位于这三个区域中的任意一个，需要根据文件新EOF的所在位置与旧EOF的所在位置进行分类讨论，总共需要处理6种不同的情况；
  + 如果在拓展文件时执行失败，就需要根据旧EOF和新EOF的相对位置，将文件索引树的结构回滚到文件未被拓展之间的状态，这同样也需要根据之间的相对位置的分类讨论结果执行不同的行为；
  + 由于三种不同的指针区域之间实际上存在着递归嵌套的关系（比如说二级间接块实际是一个和Inode中间接块指针区域相同的数据结构），因此需要设计较为通用的接口；
+ 解决方案：所有函数都采用了同一种接口设计的语义，都是”对于一个指针区域，给定期望的扇区数目，表示在此函数执行过后，该区域应当包含的扇区的数目“：
  + 用于操作直接块指针区域的函数：`resize_direct_ptr_portion`：
    + 语义是：将指定直接块指针区域中的扇区数目修改为期望值
      + 如果已分配扇区数小于期望值，分配更多扇区；
      + 如果已分配扇区数大于期望值，将多余的扇区释放；
    + 如果过程中分配失败，保证指针区域被回滚为调用时的内容；
  + 用于操作间接块指针区域的函数：`alloc_indirect_portion`：
    + 语义和直接块指针区域操作函数类似，不过是让指定区域的扇区数目增加为期望值扇区数目，不能用于减少该区域的扇区数目；
    + 函数有一个递归调用的语义，如果函数被执行时发现参数给定的间接块指针区域不能容纳期望的扇区数目，那么会分配一个二级间接块，尝试将溢出的扇区分配在其中；
    + 这里为什么说是递归语义呢？主要是由于间接块指针区域实际就是一个二级间接块，因此可以使用当前函数一并处理其中的分配事宜；
  + 回滚分配函数：`release_full_indirect_block`：
    + 用于将某个间接块区域中的所有间接块以及直接块全部释放；
    + 除了释放数据本身的扇区之外，还需要释放直接块和间接块所占据的扇区；


----------
## Subdirectories

TODO:

+ 可使用长度大于14的路径名查找文件；
  + 尽管单文件名称的限制为14，但是路径名整体可以大于14；
  + 简单来说需要在系统调用桩这一层级就将路径名解析为合理值；
+ 每个进程都有自己的工作目录：
  + 系统第一个用户进程的工作目录为根目录；
  + 如果进程是通过`exec`被其他进程所启动的，那么新进程会继承旧进程的工作目录。不过随后这两个之间就不存在关联了；
  + 进程可以删除正在被其他进程作为工作目录或打开的目录，前提是不能在已被删除的目录中创建新文件；
+ 进程现有的文件描述符打开表需要为目录提供支持；
+ 系统调用能将文件名解析为合适的绝对路径或者相对路径：
  + 相对于当前工作目录的路径是相对路径：路径名称不以`/`起头；
  + 相对于根目录的路径是绝对路径：路径名称以`/`起头；
  + 每个文件中都需要有`.`以及`..`，供相对路径可以正确发挥作用；
  + 还有关于路径解析的小问题，具体看PDF的FAQ部分；
+ 系统调用：
  + `open`：不仅可以打开普通文件，也可以打开目录文件；
  + `read/write`：不可以使用这两个系统调用读取目录文件，必须通过`readdir/mkdir`完成这一工作；
  + `remove`：不仅可以删除普通文件，还可以删除空的目录文件（除了根目录）；
  + `close`：支持关闭目录文件；
  + You must add the following system calls to allow user programs to manipulate directories: `chdir`, `mkdir`, `readdir`, `isdir`. 
  + You must also update the following system calls so that they work with directories: `open`, `close`, `exec`, `create`, `remove`, `inumber`.

如果将`tar`作为用户进程运行的话

### Data Structures and Functions

#### 进程文件打开表

考虑到如下因素：

+ 目录的底层数据结构（`struct dir`）和文件的底层数据结构（`struct file`）有所不同；
+ 如果为在PCB中单独添加一个“目录打开表”的话，需要再写一套和维护“文件打开表”相同的代码；
+ 本质上来说，无论是目录还是文件底层都是`inode`，基本可以使用相同的方式进行维护；

因此需要对PCB文件打开表的结构体做出如下修改：

~~~c
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
    struct dir *dir; /* 目录指针，务必注意释放问题 */
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
  uint32_t file_desc; /* 文件描述符，从3开始 */
  struct file_in_desc; /* 文件指针 */
  struct list_elem elem;
};

~~~

与此同时，所有针对进程文件描述符的操作都不在直接通过成员访问（`->`）实现，而是通过接口进行实现（`file_desc_*`）

#### 进件工作目录

只需在`struct process`添加一个字段即可

~~~c
~~~



### Algorithms

自下而上地，文件系统相关的接口可以分为如下五个层级：

1. Inode操作接口（`inode_*`）：直接读写Inode对应文件中的内容，不论是目录还是普通文件；
2. 普通文件与目录文件操作接口（`file.c: file_*, dic.c: dir_*`）：用于读写文件、读取目录表项、获取文件大小、读取Inode等等；
3. 文件系统接口（`filesys_*`）：用于创建、打开、关闭、删除普通文件以及目录文件；
   - 需要在这一层处理以及解析文件路径；
   - 原有的接口语义不变，可能需要改成专用于根目录的文件接口或者专用于文件的接口；
   - 需要添加几个专用于创建目录、移除目录、读写目录的接口；
4. 文件描述符表操作接口（`file_desc_*`）：用于操作进程文件描述符表中的表项：
   - 进程只能读取位于文件描述符表中的文件，因此无论是读取目录还是读取普通文件，都需要通过此类接口帮助其完成工作；
   - 关于进程工作目录：当进程进入某个目录时打开此目录作为新的工作目录，同时关闭旧有的工作目录；
5. 系统调用接口（`syscall_*`）：用户进程执行文件相关系统调用的桩程序；
   - 由于普通文件和目录文件都位于进程文件打开表中，因此所有文件操作都需要通过`file_desc_*`来完成；

#### 文件系统接口

原有文件系统的含义不变，依然局限于普通文件操作，唯一的变化在于对复杂文件路径的处理。如果需要操作目录文件的话，需要使用其他接口。具体来说，`filesys_open`需更名为`filesys_open_file`，如果给定的文件名为目录，那么返回`NULL`；如果要打开目录的话，需要使用另一个`filesys_open_dir`接口。

需要在文件系统内部实现一个用于判断指定`inode`文件类型接口：

+ 如果指定`inode`是目录文件那么返回`enum file_type DIR`，反之`enum file_type FILE`；
+ 防止暴露文件系统内部的`inode`实现；

这一层的主要任务是对文件路径进行解析处理：

+ 路径并非以`/`起头（相对路径或来自于`fsutil`），检查`tcb->pcb`：
  + 如果为`NULL`：内核线程，读取根目录中的对应名称文件；
  + 如果不为`NULL`：用户线程（也可能是`init`线程，有一个Mini PCB），读取进程工作目录中的对应名称文件；
+ 路径以`/`起头（绝对路径），从根目录开始路径处理处理；

#### 文件描述符表

操作文件描述符表：

+ 任何涉及解析文件路径的操作都交由文件系统接口；
+ 读取文件、获取文件大小这一类无需解析路径的操作可直接调用第二层接口；

由于引入了文件的类型字段，因此所有对进程文件打开表的操作都通过相关接口进行实现。不仅如此，任何通过传递文件来完成的操作都必须被此类接口封装。

此外需要注意的是，由于普通文件的读写和目录文件的读写有不同的底层实现，因此对应的自然就有两套接口。

##### `file_desc_open`

~~~c
~~~



##### `file_desc_read`



##### `file_desc_write`

#### 目录中的内容

初始化目录时，其第一个扇区的前三个Slot必然为结构体`struct dir_meta_entry`

### Synchronization

#### 目录读写同步

从普通文件的角度：

+ 删除普通文件不会关闭此文件；
+ 文件被删除之后依然可读可写；
+ 除了此前已经将该文件打开的进程，其他任何进程都不可打开此文件；
+ 文件可以继续存在直到所有指向该文件的文件描述符都被关闭为止；

从目录文件的角度：

+ 不可在已被删除的目录中创建新的目录表项；
+ 只有空目录（目录表项仅有`.`和`..`）可以被删除；
+ 不可打开已被删除的目录中的任何文件，包括`.`和`..`；

可以读取或修改原有的目录表项，也可以删除目录表项。但是由于只有空目录可以被删除，同时目录中的`.`和`..`不可被删除，因此实际可以认为，**目录已被删除的情况下，只可以读取其中的数据，不可做出任何修改**；

##### 删除目录

何时可以删除目录：

- 目录为空（只有`.`和`..`）：读取第一个Slot获取当前目录中表项数目；
- 可以在目录文件中写入内容：`deny_write == 0`，没有其他线程正在读取目录中的表项；

如何删除目录：

1. 首先确保现在可以进行目录删除操作（满足上述两个条件）；
2. 将第一个Slot中的删除旗标置为1；
3. 将`inode->remove`设置为`true`；

##### 在目录中创建文件

由于创建文件涉及磁盘写入，因此不能使用延迟写入来将其与删除目录进行同步。不过由于目录中的文件数目和删除旗标被保存在同一个插槽（第一个）中，因此真正修改目录文件的某个Slot之前，需要先向目录中文件数目递增。具体来说：

1. 读取目录的`struct dir_meta_entry`，检查文件是否已被删除；
2. 如果文件已被删除，直接返回。反之递增目录中文件的数目；
3. 遍历其余内容，直到找到一个空的Slot为止；

##### 打开目录中的文件

简单来说，打开文件实际是在文件中查找对应表项，随后对找到的Inode调用`inode_open`，全程不涉及任何**写入操作**。因此可以在检查目录中内容之前，调用`inode_deny_write`限制其他线程对Inode的写入操作，进而确保在此线程调用`inode_allow_write`之前，目录不可能被移除（移除目录的时间节点为**修改**目录的`struct dir_meta_entry`，意味着必须要对Inode执行写入操作才能删除目录），具体来说需要执行如下操作：

1. 调用`inode_deny_write`限制对Inode的写入操作；
2. 检查目录的`struct dir_meta_entry`，确认目录是否已经在延迟写入之前就被删除了；
3. 如果没有被删除，照常遍历并打开找到的文件即可；
4. 无论如何，调用`inode_allow_write`允许写入；


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
