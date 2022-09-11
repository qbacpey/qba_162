# Project Threads Design

Group X (replace X with your group number)

| Name | Autograder Login | Email |
| ---- | ---------------- | ----- |
|      |                  |       |
|      |                  |       |
|      |                  |       |
|      |                  |       |

# Efficient Alarm Clock

TODO:

- 实现、验证优先级队列；
  - 可参考word_count实现；
- timer_sleep & thread_tick;

## Data Structures and Functions

### 增加部分

#### time.h

~~~c
struct timer_sleep_thread {
    int64_t start; // 调用timer_sleep时的tick
    int64_t ticks; // 想要等多少个tick
    struct thread *thread; // TCB
    struct list_elem elem; // timer_sleep_list的入列元素
}
~~~



#### timer.c

~~~c
/* 因执行timer_sleep而BLOCK的线程列表，是一个优先级队列，优先级是ticks */
static struct list timer_sleep_list;
~~~



## Algorithms

### timer_sleep

这个函数需要在其原有的实现上做出如下改进：

1. 利用`timer_ticks`的返回值初始化`struct timer_sleep_thread`；
2. 禁用中断时执行以下两件事：
   1. 将当前线程TCB移入`timer_sleep_list`；
   2. 将当前线程TCB移出Ready Queue；
3. 启用中断，执行`thread_yield`；

### thread_tick

作为计时器中断处理程序，此函数执行过程中中断被禁用，需要在原有实现上增加一些东西：检查`timer_sleep_list`，是否有需要移动到Ready Queue中的线程（仿照`timer_sleep`中的逻辑进行设计）

这样每次触发计时器中断的时候都需要检查`timer_sleep_list`，决定要不要唤醒线程

## Synchronization

考虑到计时器中断可能会修改`timer_sleep_list`，因此任何针对`timer_sleep_list`的访问都需要禁用中断

## Rationale

还有其他可能的实现吗？

# Strict Priority Scheduler

> **Strict Priority**
> 
> A strict priority scheduler schedules the task with the highest priority. If multiple tasks have the same priority, they can be scheduled in some RR fashion. 
> This ensures that important tasks get to run ﬁrst but doesn’t maintain fairness. Evidently, a strict priority scheduler suﬀers from starvation for lower priority threads.
> 

TODO: 

- 替换Ready Queue实现；
  - 应该有对应的测试用例；
  - 还有获取、设置优先级的那两个函数；
- 信号量队列；
  - 优先级队列实现；
- 锁和优先级捐献；
  - 需要参考这里的算法；
- 条件变量；
  - signal、broadcast；

同步原语的要求：

- 除了优先级捐献，不可修改其他线程的优先级（thread_set_priority只能改自己的优先级）；
- thread_get_priority需要返回实际优先级；
- 若线程释放锁之后实际优先级降低了，那么需要立刻thread_yield；
- 仅需为lock实现优先级捐献；
- 需要为lock、semaphores、都实现优先级调度；
- 锁被释放之后，下一个获取锁的线程必须是优先级最高的线程；

- 优先级捐献的各种情况：
  - 多来源捐献：采纳优先级最高的那一个；
  - 嵌套捐献：沿路线程的实际优先级都需要修改；
  - 恢复捐献：释放锁的时候需要恢复Effective为Base；

## Data Structures and Functions

### 数据结构

#### strict_ready_list

~~~c
static struct list strict_ready_list;
~~~

Strict Priority Scheduler 的 Ready Queue，列表中越靠前优先级越高

#### struct thread

##### 增加部分

~~~c
struct rw_lock lock; // 修改TCB之前需要获取此锁
int e_pri; // 线程的实际优先级
struct thread *donee; // 线程优先级的捐献对象
struct lock *donated_for; // 线程因为哪一个锁诱发优先级捐献？
~~~

##### 修改部分

~~~c
int priority; -> int b_pri; // 线程的基本优先级
~~~

#### struct semaphore

##### 增加部分

~~~c
struct list waiting_list; // 等待此锁的线程，优先级队列
~~~

#### struct lock

##### 增加部分

~~~c
int state: // 当前锁的状态，3种状态仿照Linux2中内核锁实现
~~~

上述三种不同状态指的是：

1. FREE：state==1；
2. BUSY：state==0；
3. QUEUING：state==-1（锁可能有等待者）；

## Algorithms

### 线程创建与退出

线程创建的时候需要初始化读写锁，如果线程想要退出，必须先获取这个读写锁。目的是为了防止线程在捐赠优先级的时候，当前想要修改的线程退出了（执行thread_exit）

### 优先级队列

- 骨架使用原生list进行实现；
- 可以参看word_count的实现；
- 首元素必然最大；
- 比较函数：接收两个参数作为比较对象、返回int作为比较结果；
- list_insert_pri，遍历列表，如果遇见等于自己的或者小于自己的就插到他前面:
  - 对函数list_insert进行包装；
  - 参数包括：外壳类型信息、list_elem、需要作为比较的元素名字、比较函数；
  - 利用list_entry解包list_elem之后，不断调用比较函数对需比较元素进行比较；

### Ready Queue

- 使用优先级队列进行实现；
- 入列函数使用list_insert_pri，出列函数使用list_push（选出优先级最高的运行）；
- 操作队列的时候需要禁用中断；
- thread_yield可能也需要修改以确保优先级政策的正确实现；

### 同步原语

不变性：

- 嵌套链顶部线程释放锁，下一个获取到锁的线程必然是等待这个锁的线程中优先级最高的那个；
- 靠近捐献树顶部，线程的优先级必然越高；
- 优先级小于等于锁当前优先级的线程不入列，直接进锁的Waiting Queue；

#### 基本优先级实现

- 信号量：
  - sema_down：包装list_insert_pri；
  - sema_up：包装list_push（选出优先级最高的运行）；
- 锁：
  - lock_acquire：包装list_insert_pri；
  - lock_release：包装list_push（选出优先级最高的运行）；
- 条件变量：
  - cond_wait：修改等待逻辑；
  - cond_signal：包装list_push（仅将第一个TCB拖出来）；
  - cond_broadcast：清空wait_list（将所有的TCB都拖到Ready Queue中）；

#### 优先级捐献实现

以下几点需牢记在心：

- 对lock的state执行任何操作之前需要禁用中断；
- 同步原语入列的时候使用`thread_block`将其移出Ready Queue，出列的时候使用`thread_unblock`将其移入等待队列；
- 关于donee：
  - 线程捐献优先级的时候需要将donee设置为holder；
  - 线程起来运行的时候**必须**将donee设置为NULL（初始化的时候也是）；
  - 如果TCB的donee!=NULL，表示当前线程还在等待donee；
  - 如果TCB的donee==NULL，表示当前线程没有参与任何优先级捐献；

##### lock_acquire

使用信号量中的优先级队列维护优先级

当前线程尝试获取锁的时候，根据锁的不同状态有不同的行为：

- FREE（state==1）需禁用中断
  1. state=0；
  2. sema_down；
  3. 设置holder；
- BUSY & QUEUING（state<=0）需禁用中断
  1. state--；
  2. 执行Sync中提到的优先级捐献；
  3. sema_down（入列） ->...->获取锁；
  4. holder=thread_current()；
  5. donee=NULL；

##### lock_release

当前线程尝试释放锁的时候，根据锁的不同状态有不同的行为：

- FREE（state==1）PANIC；

- BUSY （state==0）
  1. state=1;
  2. holder=NULL;
  3. sema_up;
- QUEUING（state<0）
  1. holder=NULL；
  2. state++；
  3. sema_up；
  4. 执行Sync中提到的优先级捐献；

## Synchronization

同步资源：

- 所有线程的TCB；
- 锁本身的状态（state）；
- Ready Queue；

### Ready Queue

#### 访问方式

- 中断处理程序可能访问（调度器）：
  - 执行schedule，确定下一个需要运行的线程；
- 各种同步原语的释放和获取：
  - 将Ready Queue中的TCB移动到Wait Queue中；
- thread_create & thread_exit：
  - 线程创建和线程结束的时候修改Ready Queue；

#### 防护方式

直接禁用中断即可，schedule这个函数本身调用的时候就需要禁用中断

### Lock 本身状态

中断处理程序无需访问Lock本身的状态，只有需要访问Ready Queue的时候需要禁用中断

#### 访问方式

各种同步原语的释放和获取：

- 线程获取锁的时候需要检查并修改state、修改Wait Queue（不过是通过修改信号量的内容来实现的）；
- 线程释放锁的时候也需要检查并修改state，修改Wait Queue；

#### 防护方式

如果是同步原语的Wait List的话，实际上在原来信号量实现（禁用中断）的基础上弄就可以了。具体来说就是将这里的内容换成优先级队列实现即可（sema_up可类似处理）：

~~~c
void sema_down(struct semaphore* sema) {
  enum intr_level old_level;

  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  old_level = intr_disable();
  while (sema->value == 0) {
    list_push_back(&sema->waiters, &thread_current()->elem);
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}
~~~

如果是lock内部的state和holder的话：

- state
  - 实践上来说，这个变量本来应该使用原子性的读写进行维护。但是考虑到本系统中没有原子性的组件，因此只能在禁用中断的情况下修改这个变量；
  - 在读取、写入此变量之前需要**禁用中断**，完毕后重启中断；
- holder
  - 确保对其的修改工作发生在sema_down之后；
  - 为了调试，需要在合适的时候将其设置为NULL；

### Wait List

让信号量自己维护就可以了，对Wait List的任何操作都需要禁用中断

### TCB

这里指的是针对TCB中状态的修改操作，不是指的是将TCB在不同的队列中移动

考虑到中断处理程序不会访问其中的数据，因此直接使用同步原语保护其中内容即可

#### 访问方式

值得注意的是，发生优先级捐献有两个条件：

1. 当前正在运行的线程的优先级比系统中所有线程的优先级都要大。就算出现和当前系统中优先级最大的线程优先级持平的线程，也不用担心，因为系统选择下一个将要运行的线程的时候会将线程pop出来。该线程运行过程中可能有新的线程被加进去，等到下一次中断发生并出现调度事件的时候，调度器需要将该线程insert到合适的位置，而插入的策略是小于或等于就插（而且在这个过程中无需担心被打断）；
2. 线程需要获取的锁位于低优先级线程手上。如果当前线程的优先级比该线程优先级低，自然不会发生优先级捐献；

同步原语：

- 获取锁的时候需要依次读取TCB中的e_pri，确定将这个TCB插到什么地方；
- 优先级捐献的时候需要读取（也可能修改）e_pri和donee；
- 线程释放锁的时候可能需要修改自己的e_pri和donee；

#### 防护方式

不变性：

- 系统中正在运行的线程优先级必然最高；
- 捐献链上游线程的基础优先级必然小于任何能更新它的线程

- 线程沿路更新捐献链上TCB时，必然处于抢先捐献树底部线程的状态。并且直到它成功遇见终止条件或者系统中出现了其他更高优先级的线程之前，都不会停下来；
- 如果线程在更新捐献链上TCB时被高优先级线程抢先了，此线程必然在高优先级线程运行完毕之后才会被调度，考虑到线程可能处于即将更新已经yield的线程的优先级的状态（即将解引用时被打断），因此获取TCB锁之后需要检查TCB中的donated_for是否为FREE。如果是FREE就不继续上溯，同时将donee设置为NULL；

任何针对TCB的修改操作必须事先获取TCB的读写锁：

- 沿捐献链上溯的时候需按照以下顺序检查其中内容：
  1. 解引用holder或donee，获取被捐献者的TCB（读写锁）；
  2. 比对自身实际优先级和被捐献者的实际优先级，如果自身更低，释放读写锁并离开；
  3. 更新TCB中的实际优先级并检查其donee，如果是NULL那么释放读写锁并离开；
  4. 禁用中断并检查donated_for，如果是FREE，将TCB的donee设置为NULL并获取锁，随后离开；
  5. 继续上溯捐献链；
- 自身释放锁的时候需要检查自身的实际优先级（当然还需要将优先级列表中的第一个线程拖出来）：
  - 如果和基本优先级持平，那么不需要yield；
  - 如果实际优先级更高，那么需要：
    1. 获取自身TCB读写锁；
    2. 将自己的donee、donated_for设置为NULL；
    3. 恢复自身实际优先级为基本优先级；
    4. 释放自身TCB读写锁；
    5. yield CPU；

## Rationale

性能还是比较好的，禁用中断的地方仅出现在操作队列和state的时候，其他时候并不需要禁用中断

同时如果锁是FREE的话之间递减state即可，无需执行函数调用，检查wait queue（lock release也是类似的）

# User Threads

## 注意事项

用户线程之间的优先级问题：

- 所有用户线程的优先级相同（都是`PRI_DEFAULT`）；
- 不需要为此实现额外的调度器；

控制台（打印锁）：

- 直到实现用户级线程锁之前，所有的测试都会失败；
- 具体参见C.3 Additional Information；

进程间切换（需要查看`process_activate`）：

- 需要修改页表指针；
- 需要禁用所有的虚拟内存（TLB）；
- 创建新的用户线程的时候需要激活进程（调用上面的函数？）；

## Data Structures and Functions

### process.h

#### struct registered_lock

~~~c
struct registered_lock {
    lock_t lid; /* 锁的标识符 */
    struct lock lock; /* 锁本身 */
    struct list_elem elem; /* List element */
}
~~~

#### struct registered_sema

~~~c
struct registered_sema {
    sema_t sid; /* 信号量的标识符 */
    struct semaphore sema; /* 信号量本身 */
    struct list_elem elem; /* List element */
}
~~~



#### struct process

~~~c
struct process {
    ...
    struct rw_lock threas_lock; /* 进程线程读写锁 */
    struct list threads; /* 进程线程列表 */
    struct rw_lock locks_lock; /* 进程用户空间锁列表读写锁 */
    struct list locks; /* 进程用户空间锁列表 */
    struct rw_lock semas_lock; /* 进程用户空间信号量列表读写锁 */
    struct list semas; /* 进程用户空间信号量列表 */
    
    struct lock pcb_lock; /* PCB中非列表字段的锁 */
    bool exiting;/* 进程是否正在执行exit函数？ */
    struct thread *thread_exiting; /* 当前是否有函数正在执行process_exit */
    uint32_t pending_thread; /* 当前有多少个内核线程还在执行 */
}
~~~

- `uint32_t pending_thread`：条件变量
  - `exit`第一次检查线程列表的时候，如果遇见线程`in_hanlder==true`就会将这个值`++`；
  - `syscall_hanlder`如果需要执行`thread_exit`，那么就需要将这个值`--`，一旦这个值归零，那么发出`signal`；
- `bool exiting` 初始为`false`；
- `struct thread *thread_exiting` 初始为`NULL`

### thread.h

#### enum thread_status

~~~c
enum thread_status {
    ...
	THREAD_ZOMBIE /* 线程已结束，但尚未被join，需要回收TCB */
}
~~~

#### struct thread

~~~c
struct thread {
    ...
    bool in_handler; /* 表示当前线程是否位于内核环境中（禁用中断） */
	#ifdef USERPROG
  		/* Owned by process.c. */
  		struct process* pcb; /* Process control block if this thread is a userprog */
  		struct list_elem prog_elem; /* 进程线程列表元素 */
    	struct thread *joined_by; /* 指向join当前线程的TCB */
    	struct thread *joining; /* 指向当前线程正在join的线程的TCB */
	#endif
    struct rw_lock tcb_lock; /* TCB中非列表字段的锁 */
    ...
}
~~~

## Algorithms

### 线程库函数

#### `pthread_create`

作用主要是创建线程，工作可概括如下：

- 创建线程的时候需要将自己的TCB添加到进程的线程列表（`pcb->threads`）中，主线程也不可例外；
- 直接在TCB中加一个`struct list_elem prog_elem`字段，不单独为线程列表元素包装东西了；

- 初始化的时候将`joined_by`、`joining`设置为`NULL`；

#### `pthread_exit`

作用主要是线程退出，工作可概括如下：

- （非主线程）退出的时候将自己的状态设置为`THREAD_ZOMBIE`，同时需要唤醒`joined_by`：
  - 全程禁用中断；
  - 不需要将自己从线程队列中移除，但是需要将自己从Ready Queue中移除；
  - 防止`switch_thread`的时候自己的Page被FREE掉了[^退出状态]；
  - 要么就是让`pthread_join`自己的时候顺带将自己清理掉，要么就是让`exit`清理所有线程TCB的时候将自己清理掉；
  - 如果自己是主线程或者执行`exit`的线程，需要将自己的状态设置为`THREAD_DYING`，让调度器代码来回收自己的资源；

- 如果主线程执行`pthread_exit`，那么主线程需要join所有**活跃**线程之后再退出：
  - 需要使用`is_main_thread`判断当前线程是否是主线程，如果是，那么需要遍历当前进程的线程列表[^主线程最后的Join是否可能引发死锁？]；
- 如果主线程被join了，那么在主线程执行`thread_exit`之后，执行`process_exit`进程退出之前，该线程会被唤醒（使用`thread_unblock`）；
  - 由此来看，主线程退出的时候还需要检查自己TCB中的`joined_by`，看有没有其他线程Join自己；
  - 这一点对于普通线程也是成立的，它们退出的时候也需要检查自己的`joined_by`并唤醒它（将`joining`标记为`THREAD_READY`即可）[^死锁1]；

##### 遍历活跃线程

遍历的时候不要修改线程表，让`thread_join`承担TCB清理工作，见到一个状态不是`THREAD_ZOMBIE`的，join它即可：

- `thread_create`创建线程的时候需要在线程表的最后加TCB：
  - 活跃线程可能在主线程遍历线程表的过程中，创建其他线程；
  - 确保一但join完到线程列表的最后一个元素，线程列表中的所有线程都运行完毕了；
  - 同时凭借`thread_join`的清理功能，顺带完成了TCB的清除工作；
- 主线程join一个具体的活跃线程（非`THREAD_ZOMBIE/THREAD_DYING`）的时候[^活跃线程]，需要检查线程的`joined_by`，直到遇见`NULL`之后（到达join线程链的末尾）再join这个线程：
  - 如果遇见`joined_by`不是`NULL`的线程，跳过他的TCB即可；
  - 目的是为了防止出现join死锁，或者说违反项目的不变性（一个线程只能被join一次）；
  - 获取锁之后务必再次检查TCB的`joined_by`，防止在获锁间歇这个线程被其他线程join了；

- 需要一直不断地遍历线程列表，直到其中TCB数量为$0$或其中线程的状态全部是`THREAD_ZOMBIE`为止：
  - 可以使用一个flag表示这一点；
  - 对于那些不是`THREAD_READY`的线程，不能简单删掉TCB！
- 如果在线程列表中遇见一个状态是`THREAD_ZOMBIE`的线程，跳过即可；

#### `pthread_join`

阻塞直到指定线程执行完毕之后再继续执行（注意，任何线程可以join同进程的任何其他线程）

##### 执行前测试

Sanity Test：或许可以使用宏实现，`pthread_join`执行之前需要执行此宏。此测试的目的是：仅可以join位于同一进程的线程，且这个线程尚未被join过（要不就不合法）
- 检查目标线程TCB中的`pcb`字段，如果和自己的不一样，那么就返回`TID_ERROR`；
- 检查目标线程TCB中的`joined_by`字段，如果不是`NULL`，那么就返回`TID_ERROR`；

##### 线程状态与Join

根据被Join线程的状态不同，Join也有两种不同的行为：

- `THREAD_ZOMBIE`：不仅需要返回`tid`，还需要FREE它的TCB，将它从线程列表中移除；
- 其他线程状态：需要上溯`joining`，确认线程链的头部不是自己；
  - 如果是，那么就有可能出现 join 死锁，不可以执行本次join操作；
  - 直接禁用中断；

##### 执行全流程

具体来说，`thread_join`执行的顺序如下：

1. 禁用中断；
2. 检查需要Join的线程的状态：
   - 如果是`THREAD_ZOMBIE`且`joined_by==NULL`，直接返回`tid`；
   - 如果是`joined_by!=NULL`，返回错误；
   - 如果不是`THREAD_ZOMBIE`且`joining!=NULL`，继续；
3. 不断解引用遍历`joining->joining`，直到以下任一条件被满足：
   - 出现`joining==NULL`；
   - 线程状态是`THREAD_ZOMBIE`；
4. 如果第三步满足`joining==NULL`检查当前所在的TCB是不是自己的TCB：
   - 如果是，返回错误；
   - 如果不是，设置自己的`joining`，初始被join线程TCB的`joined_by`；
5. `thread_block`；
6. 禁用中断；
7. 回收被Join线程的TCB；
8. 重启中断，返回`tid`；

#### `get_tid`

获取线程TID

### 用户级同步原语

用户级信号量、锁的实现，需要借助系统调用（虽然基本就是对内核实现的包装）：

- `lock_init`：将锁注册到内核空间
  - `lock_t`是一个指向用户空间`char`变量的指针；
    - 估计需要在`lock`之外再包装一层，其中含有这个`lock_t`以及链表元素；
    - 当前进程持有的锁的列表的引用需要保存在TCB中，确保进程退出时释放其中内容；
  - 如果注册失败返回`false`；
- `lock_acquire`：获取锁
  - 有需要的时候阻塞这个函数（等待直到成功获取锁，这个系统调用才会返回）；
    - 实际上就是对这个锁执行`lock_acquire`；
  - 如果锁未被注册或者当前线程已经获取锁了，那么返回`false`；
    - 需要遍历锁的列表以确保这把锁被注册了；
- `lock_release`：释放锁
  - 如果锁被成功释放的话这个函数返回`true`；
  - 如果锁已经被释放了或者当前线程并未持有该锁返回`false`；

### 系统调用

#### `exec`

无论父进程有多少个线程，新分化出来的进程只有一个线程（主线程）；

- 看起来进程启动的时候需要将进程主线程的TCB直接安排好；
- 初始情况下线程列表什么也没有（主线程TCB不在其中）；

#### `wait`

同一进程中只有调用`wait`的线程会被暂停，其他线程照常运行；

- 好像没有什么特别的需要，将waiting线程的状态设置一下就可以了；

#### `exit`

假设一个进入内核的线程永远不会被阻塞。进程中任一线程调用`exit`的时候，其他所有的线程都立即释放所有的资源，随后用户程序直接结束

##### 整体执行流程

`exit`函数的整体执行流程基本如下（不包括原有的执行流程）：

1. 检查自己PCB的`exiting`字段：

   - `true`：比对自己的退出码和PCB中已有的退出码之后，执行`thread_exit`；
     - 代表已经有线程在执行`exit()`了，因此直接执行`thread_exit`；

   - `false`：将其设置为`true`并将自己的退出码更行到PCB中，随后继续执行；

2. 释放资源，在原基础上，还需要释放这些东西：

   - 进程持有的所有的内核资源（锁），其中属于本进程的锁直接FREE内存即可；
     - 采取的策略是让`exit`线程`wait`所有拥有内核资源的线程，这些线程系统调用退出的时候需要检查PCB的`exiting`字段。如果是`true`那么需要执行`thread_exit`，这种情况也算是`thread_yield`，因此不需要设置为`THREAD_DYING`；

   - 内核锁引用计数为0的所有线程的TCB，检索进程的线程列表即可；
     - 考虑到`exit()`线程会join所有线程，而`thread_join`会清除僵尸线程的TCB，因此处于`THREAD_ZOMBIE`的线程不需要担心资源释放问题；

3. 将自己从线程列表中拖出来（`list_remove`）之后遍历进程的线程列表，检查它们的`in_handler`：

   - `true`：设置此线程TCB的`process_exited`，跳到下一个TCB中；
   - `false`：直接将其从Ready Queue/Waiting List中拉出来删掉，跳到下一个TCB以后将这个TCB从线程表中移除（注意使用`list_remove`）同时FREE；
     - 对于那些`THREAD_BLOCKED/THREAD_READY`线程，由于它们TCB中的`list_elem`是两用的，因此直接使用`list_elem`将线程从Wait list 中移除即可；

4. 检查线程表是否为空[^exit()中的线程表]：

   - `true`：继续执行；
   - `false`：`wait`条件变量`pending_thread==0`；

##### 进程退出码

进程退出码：以下是进程可能的退出码，越往后的优先级越高，也就会覆盖前面的退出码：

1. 主线程调用`pthread_exit`，退出码为`0`；
2. 任何线程调用`exit(n)`，退出码为`n`；
3. 进程因异常退出，退出码为`-1`；

##### `exit`重入问题

如果内核线程在释放资源的过程中触发了Page Fault那怎么办呢

>  这种情况下代表系统中已经发生了错误（`exit_code==-1`）那么直接禁用中断并执行如下代码：
>
> - FREE线程列表中的所有TCB；
> - 释放文件系统锁；
> - 设置退出码；
> - 将`pending_thread`清零，`signal()`；
> - `thread_exit`；
>
> 等到之前执行`exit`被再次调度运行的时候，线程列表中已经不剩下TCB了，此时继续其工作即可

如果是发生外部中断导致系统需要执行`exit(-1)`可以断定的是，在`process_exit`的整个执行过程中，都不会发生中断。同时如果发生这种级别的异常，同一进程中的其他线程就别想正常执行完毕了。因此这种时候就不需要再获取线程列表的锁再对其进行操作了，直接将其中所有线程的TCB全部销毁就行了。

而且，就算是在其他`exit(0)`执行过程中（需要确保这一点的可能性只有在等到`in_hanlder`的时候才有可能）触发了中断也不要紧，`exit(-1)`照样将其中的所有TCB全部清理掉。唯一需要注意的是第一个进`process_exit`的线程除了将`exiting`设置为`true`，还需要将自己的TCB登记一下，便于`exit(-1)`线程将其一并销毁

## Synchronization

### struct process

#### 线程队列

~~~c
struct rw_lock threas_lock; /* 进程线程读写锁 */
struct list threads; /* 进程线程列表 */
~~~

进程中所有线程的TCB都被放在这里边，但是主线程不会在这个列表中

##### 访问方式

> 注意，这里的写入读取不包括对TCB本身的写入和读取

写入访问：

- `thread_create`会将新的线程加到列表的末尾；
- `thread_join`的joiner被唤醒的线程需要为join对象收尸，此时需要将其中的TCB移除；
- 第一次遍历时`exit`需要将此中`in_hanlder`是`false`的所有TCB从准备队列和此列表中移除；
- 普通线程执行`thread_exit`的时候需要将自己的TCB从线程队列中移除；

读取访问：

- 主线程调用`thread_exit`的时候需要遍历此列表，检查其中`joined_by`和`joining`并对这些字段进行设置；
- 第二次遍历时`exit`需要wait直到`in_kernel==0`为止；

##### 防护方式

考虑到只有在外部中断触发`exit(-1)`的时候此队列才会被中断处理程序所访问，而如果发生这种级别的事件，进程所有余下的线程就没有继续执行的必要了。那时所有线程TCB都会被直接释放，它们也就不再有被调度执行的机会

因此线程队列的防护方式依然可以使用`threas_lock`这把读写锁，例外只有两个：

- 外部中断调用`exit(-1)`的时候，无需获取锁（已经是中断上下文了），直接释放所有TCB；
- 内部中断执行`exit(0)`的时候，为了防止进程中其他并非位于内核中的线程被调度运行，也需要在整个操作过程中禁用中断；

总的来说，除了`exit`可以例外，其他所有针对线程列表的同步访问都通过锁进行确保

#### 进程锁队列

```c
struct rw_lock locks_lock; /* 进程用户空间锁列表读写锁 */
struct list locks; /* 进程用户空间锁列表 */
struct rw_lock semas_lock; /* 进程用户空间信号量列表读写锁 */
struct list semas; /* 进程用户空间信号量列表 */
```

保存着所有用户程序申请的锁和信号量

##### 访问方式

> 注意，这里的写入读取不包括对TCB本身的写入和读取

写入访问：

- `lock_init/semg_init`会遍历这个列表，可能需要在其中添加链表元素；
- 执行`exit`的时候需要将这里边的锁全部FREE掉（注意是在清除所有线程之后）；

读取访问：

- `lock_acquire/lock_realse`等需要遍历这个列表，比对其中的`lock_t`，必要的时候获取、释放锁；

##### 防护方式

同样使用读写锁进行防护，但是`exit`释放锁需要在释放所有线程TCB之后再执行

#### `exiting`

~~~c
bool exiting;/* 进程是否正在执行exit函数？ */
~~~

主要是和线程退出相关的东西，中断处理程序可能访问

##### 访问方式

> 尽管是对PCB中字段的访问，但是由于这其中的字段可能被中断处理程序引发的`exit(-1)`检查和使用，因此访问的时候需要禁用中断

写入访问：

- 第一个执行`exit`的线程需要将`exiting`设置为`true`；

读取访问：

- 所有进入`exit`的线程都需要检查`exiting`以确定接下来的行为；
- 系统调用处理程序需要检查`exiting`以确定是返回用户空间还是执行`thread_exit`；

##### 防护方式

围绕`exiting`可能的冲突点主要是系统调用与`exit`，`syscall_handler`实现的尾部中需要插入这些代码：

~~~c
...
old_level = intr_disable();
if(pcb->exiting){
    pcb->pending_thread--;
    if(pending_thread == 0){
        signal(pcb->pcb_lock);
    }
    thread_exit();
    NOT_REACHED();
}
intr_set_level(old_level);
~~~

`process_exit()`执行时需要加点下面的东西：

~~~c
// 进入函数就禁用中断
enum intr_level old_level = intr_disable();
if(intr_context()){ /* 触发外部中断 */
   	// 遍历清除线程列表中的所有元素 
    // 可能需要释放全局文件系统锁
    // 可能需要检查和释放pcb->thread_exiting
    
    // 有可能此进程第一次执行exit就是外部中断
    pcb->exiting = true;
	pcb->thread_exiting = thread_current();
} else if(pcb->exiting) { /* 非外部中断，但是正在退出 */
    set_exit_code(pcb, exit_code);
    goto done;
} else { /* 第一次执行exit */
    pcb->exiting = true;
	pcb->thread_exiting = thread_current();
    // 按部就班的两次遍历
    /* 第一次遍历 */
    intr_set_level(old_level);
    /* 第二次遍历 */
    
}


/* 下面的部分就是清除锁列表了，还有更新父子进程共享数据的时候也需要禁用中断 */
~~~

根据退出码和中断性质的不同`exit`的执行情况可以被分为三类：

- 内部中断执行`exit(0)`：进程执行系统调用`exit`，正常退出
  - 需要遍历并等待线程列表：
    - 第一次遍历用于清除非内核中线程，全程禁用中断；
    - 第二次遍历用于等待内核中线程执行完毕，使用条件变量（`pending_thread`）进行维护；
  - 第一次遍历:
    - 由于中断被禁用，因此外部中断不可能干扰这个过程；
    - 如果有其他线程执行`exit(-1)`的时候被这个`exit(0)`抢先了，由于禁用中断的缘故，这两个线程也不可能纠缠在一起。由于`exit(-1)`并非由外部中断所引发，因此可以简单设置退出码就离开，无需强制清空线程表，也就可以将这些工作交给`exit(0)`线程来完成；
  - 第二次遍历：
    - 外部中断可能在内核线程执行的过程中发生并调用`exit(-1)`，此时`exit(0)`线程处于条件变量的等待队列当中，因此外部中断可以安全地清空线程列表（包括`exit(0)`的TCB）之后接替它的工作；
    - 如果是其他途径触发的`exit(-1)`，设置一下`exit_code`之后`thread_exit`即可。由于进入和设置`exit_code`的时候中断都是被禁用的，因此无需关心Race Condition的问题；
    - 另外，为了防止意外苏醒，需要在`wait`的外部加上`while`；

#### `pending_thread`

~~~c
uint32_t pending_thread; /* 当前有多少个内核线程还在执行 */
~~~

`exit`清除内核中线程的时候会使用这个字段，其他时候都是废弃状态

##### 访问方式

写入访问：

- 非外部中断引发的`exit`需要根据第一次遍历线程列表时统计的`in_hanlder`数量设置这个值；
- 内部中断处理程序可能需要递减这个值；
- 外部中断处理程序也可能需要递减这个值（在`if(extern)`中）；

读取访问：

- `exit`的第二次遍历的`while`循环需要访问这个值；

##### 防护方式

这里最重要的问题就是使用`in_hanler`字段结合`pending_thread`能否实现TCB全部清空，考虑以下几种非硬件中断线程穿插情况：

- 设置`in_hanlder`之前`exit`就开始运行：
  - 第一次遍历直接清除它的TCB，无影响；
- 设置`in_hanlder`之后，`system_hanlder`尾部检查`exiting`之前，`exit`被调度执行：
  - 由于`exit`执行全过程禁用中断，`exit`可以确保一旦它被调度执行，那么下一次此线程被调度执行的时候系统具有如下不变性：`exiting==true`/`penging_thread>0`
  - 第一次遍历不会清除它的TCB，等到此线程再次被调度执行时，检查`exiting`发现正在执行`exit()`，因此递减`pending_thread`并退出（全程禁用中断）；
- `system_hanlder`尾部检查`exiting`之后，`exit`被调度执行：
  - 由于此线程系统调用对应区域执行全过程禁用中断，`exit`执行时可确保如下不变性：`in_handler==false`，因此`exit`第一次遍历线程列表的时候就能将TCB移除；

如果是外部中断，由于外部中断处理程序执行全过程禁用中断，而`intr_handler`尾部的`thread_yield`就算被抢先了也没有什么影响，因此外部中断不需要设置`in_handler`这个旗标

#### `thread_exiting`

~~~c
struct thread *thread_exiting;
~~~

当前正在执行`process_exit`的线程，用于确保外部中断不会造成内存泄漏

##### 访问方式

写入访问：

- 初次进入`exit`的线程需要设置这个值（如果是通过外部中断调用的话则不需要）；

读取访问：

- 通过外部中断进入`exit`的外部中断处理程序需要将`thread_exiting`释放掉；

##### 防护方式

对这个字段的所有读写都在禁用中断环境下，不需要担心同步问题

### struct thread

#### `status`

~~~c
enum thread_status status; /* Thread state */
~~~

线程状态

##### 访问方式

写入访问：

- `thread_join`的时候需要将自己的状态设置为`THERAD_BLOCK`；
- `thread_exit`的时候需要将自己的状态设置为`THREAD_ZOMBIE`；
- `lock_aquire/lock_realase`这些同步原语也需要修改这个值；

读取访问：

- `thread_join`如果判断一个线程是`THREAD_ZOMBIE`，需要获取它的`tid`，然后返回；

##### 防护方式

禁用中断

#### `in_handler`

~~~c
bool in_handler; /* 表示当前线程是否位于内核环境中 */
~~~

只有在软件中断的时候需要处理这个值

##### 访问方式

写入访问：

- 进入`syscall_handler`开始位置将其设置为`true`；
- 离开`syscall_handler`时可能将其设置为`false`（也可能直接离开）；

读取访问：

- `exit`第一次遍历线程列表的时候需要读取所有TCB中的`in_handler`；
  - 不过此时是中断禁用环境；

##### 防护方式

针对此字段的所有访问操作需要禁用中断

#### `joined_by`

```c
struct thread *joined_by; /* 指向join当前线程的TCB */
```

用于表示当前线程有没有被其他线程join

##### 访问方式

写入访问：

- `thread_join`执行的时候，可能会设置指定线程的此字段；

读取访问：

- `thread_join`时需要检查这个字段，用于判断是否可以join此线程；
- 线程退出的时候需要唤醒`joined_by`中的线程（无需将此字段设置为`NULL`）；
- 主线程执行`thread_exit`的时候需要遍历线程列表（不是线程链！），找到`thread_join`为`NULL`的TCB然后join这个线程；

##### 防护方式

修改此变量需要禁用中断

这样就算`thread_exit`在相对于`thread_join`执行的任何时刻开始执行也不会对系统产生影响

#### `joining`

~~~c
struct thread *joining; /* 指向当前线程正在join的线程的TCB */
~~~

用于表示当前线程有没有join其他线程

##### 访问方式

写入访问：

- `thread_join`可能需要设置TCB的`joining`；

读取访问：

- `thread_join`需要遍历线程链上的所有TCB的`joining`，直到确保链子的顶部不是自己；

##### 防护方式

见上

## Rationale

# Concept check

1. `thread_switch_tail`. 最本质的原因是`switch_thread`的实现需要访问当前线程的TCB，如果TCB在此之前被FREE了，那么就会造成空悬指针错误；

2. 内核栈（中断栈）；

3.  `functionA:lock_acquire(&lockA);` => `functionB():lock_acquire(&lockB);` => `functionB():lock_acquire(&lockA);`；

4.  B可能在持有锁的情况下被之间释放，导致这个锁永远处于BUSY状态；

5.  ~~~c
    struct lock lockA; // Global lock
    struct thread threadA;
    struct thread threadB;
    
    void functionA() {
    	lock_acquire(&lockA);
        printf("Thread A grasped the lock.\n");
    	thread_set_priority(0);
        thread_create("HighPriThread", 20, functionB, NULL);
        thread_yield();
        printf("Thread A is going to release the lock.\n");
        lock_release(&lockA);
        printf("Thread A released the lock.\n");
        printf("Thread A done.\n")
        
    }
    
    void functionB() {
        printf("Thread B is going to grasp the lock.\n");
        lock_acquire(&lockA);
        printf("Thread B grasped the lock.\n");
        lock_release(&lockA);
        printf("Thread B released the lock.\n");
        printf("Thread B done.\n")
    }
    ~~~

    - 正确的执行顺序应该是：

      ~~~c
      Thread A grasped the lock.
      Thread B is going to grasp the lock.
      Thread A is going to release the lock.
      Thread B grasped the lock.
      Thread B released the lock.
      Thread B done.
      Thread A released the lock.
      Thread A done.
      ~~~

    - 如果没有实现优先级捐献的话，执行顺序应该是：

      ~~~c
      Thread A grasped the lock.
      Thread B is going to grasp the lock.
      （死循环开始）
      ~~~

***

[^主线程最后的Join是否可能引发死锁？]: 只要切实确保了`thread_join`的两个要求：(1) 每个线程只能被join一次和 (2) 主线程只能join活跃线程，主线程最后收尾时join所有活跃线程时就不会触发死锁。具体来说，由于主线程只能join活跃的线程，因此主线程就不可能join某一线程链的最后一个线程，因此就算主线程被环的头部join了，主线程也不可能join它的尾部进而成环
[^exit()]: 由于是在系统调用环境下执行这个函数，因此当前线程是不是主线程没啥所谓
[^为什么要在TCB中？]: 尽管要只有等到所有内核资源引用计数非零的线程执行完毕之后，`exit`才能继续执行（也就是PCB不会被删掉），但是设置这个值的时候需要遍历所有线程的TCB，线程如果需要自己执行`thread_exit`退出的话也需要看自己的TCB，没有什么必要掉过头去找PCB
[^退出状态]: 普通线程退出的时候，无论自己的`joined_by`是否被设置了，都将自己的状态设置为`THREAD_ZOMBIE`。只有在以下两种条件下会将自己的状态设置为`THREAD_DYING`：(1) 当前线程执行`exit()`，(2) 当前线程是主线程（其实可以直接归类为第一种）
[^活跃线程]: 这主要考虑的是项目中假设所有的拥有用户线程映射的内核线程不可能在系统调用执行过程中被阻塞，因此就算主线程在遍历活跃线程列表的时候join了一个状态是`THREAD_BLOCK`的线程，有朝一日这个线程必然会被唤起以继续运行
[^死锁1]: 为避免这里发生死锁，线程退出的时候将自己TCB中状态修改完毕之后就释放锁（让调度器来唤起它的`joined_by`）
[^exit()中的线程表]: 考虑到如果没有被第一次遍历移除的线程必然处于中断处理程序执行过程中，而这些系统调用的执行又不可能新建线程或Join其他线程，因此不用迭代扫描