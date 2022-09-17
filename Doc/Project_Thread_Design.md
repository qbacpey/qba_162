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
struct thread {
  int64_t wake_up; // 线程需要在什么时候醒来
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

1. 利用`timer_ticks`返回值和参数`ticks`设置TCB中的`wake_up`；
2. 禁用中断时执行以下两件事：
   1. 使用`list_insert_ordered`将TCB移入`timer_sleep_list`；
   2. 使用`thread_block`将TCB移出Ready Queue；

### timer_interrupt

`timer_interrupt`作为计时器中断处理程序，执行全程自动禁用中断

需要在原有实现上增加一些东西：检查`timer_sleep_list`，是否有需要移动到Ready Queue中的线程（仿照`timer_sleep`中的逻辑进行设计）

每次触发计时器中断的时候都需要检查`timer_sleep_list`，决定要不要唤醒线程（使用`thread_unblock`将线程移动到等待队列）

注意，由于将线程在列表中移动会修改结构体`elem`中的各种字段，因此如果需要将TCB从`timer_sleep_list`移动到Ready Queue中的话，需要先执行`list_pop_front`再执行`thread_unblock`

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
uint8_t e_pri; // 线程的实际优先级
struct thread *donee; // 线程优先级的捐献对象
struct lock *donated_for; // 线程因为哪一个锁诱发优先级捐献？
~~~

##### 修改部分

~~~c
uint8_t priority; -> uint8_t b_pri; // 线程的基本优先级
~~~

#### struct lock

##### 增加部分

~~~c
uint8_t state; // 当前锁的状态，3种状态仿照Linux2中内核锁实现
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
- 同优先级RR性质使用grater才插入进行实现

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

1. 当前正在运行的线程的优先级大于系统中其他所有线程的**实际**优先级。此时就算出现和当前系统中优先级最大的线程优先级持平的线程，也不用担心，因为系统选择下一个将要运行的线程的时候会将线程pop出来。该线程运行过程中可能有新的线程被加进去，等到下一次中断发生并出现调度事件的时候，调度器需要将该线程insert到合适的位置，而插入的策略是大于才插（而且在这个过程中无需担心被打断）；
2. 线程需要获取的锁位于高优先级线程手上。如果当前线程的优先级比该线程优先级低，自然不会发生优先级捐献；

同步原语：

- 获取锁的时候需要依次读取TCB中的e_pri，确定将这个TCB插到什么地方；
- 优先级捐献的时候需要读取（也可能修改）e_pri和donee；
- 线程释放锁的时候可能需要修改自己的e_pri和donee；

#### 防护方式

上溯线程链的全过程禁用中断

不变性：

- 系统中正在运行的线程优先级必然最高；
- 捐献链上游线程的基础优先级必然小于任何能更新它的线程

- 线程沿路更新捐献链上TCB时，必然处于抢先捐献树底部线程的状态。并且直到它成功遇见终止条件或者系统中出现了其他更高优先级的线程之前，都不会停下来；

任何针对TCB的修改操作必须事先获取TCB的读写锁，执行优先级捐献遍历线程链的全过程需要禁用中断：

> 这是之前版本的优先级捐献，有时间再改，直接看新的：
> 
>- 沿捐献链上溯的时候需按照以下顺序检查其中内容：
>  1. 解引用holder或donee，禁用中断；
>  2. 比对自身实际优先级和被捐献者的实际优先级，如果自身的优先级小于等于其**实际**优先级，释放读写锁并离开；
>  3. 更新TCB中的实际优先级并检查其donee，如果是NULL那么释放读写锁并离开；
>  4. 继续上溯捐献链；
>- 自身释放锁的时候需要检查自身的实际优先级（当然还需要将优先级列表中的第一个线程拖出来）：
>  - 如果和基本优先级持平，那么不需要yield；
>  - 如果实际优先级更高，那么需要：
>    1. 获取自身TCB读写锁；
>    2. 将自己的donee、donated_for设置为NULL；
>    3. 恢复自身实际优先级为基本优先级；
>    4. 释放自身TCB读写锁；
>    5. yield CPU；
>

考虑到线程可能会请求多个锁这些不同的锁也有可能被不同的线程以不同的顺序请求比如说线程H想要获取锁A，此时的锁A为线程M持有为获取锁A，线程H需要将自己的优先级捐献给线程M但是线程M此时正在被锁B阻塞，锁B为线程L所持有也就是说，线程H不仅需要将自己的优先级捐献给线程M，还需要将优先级捐献给线程L那么线程M获取并释放锁B之后，它不应该恢复到它本来的优先级而是需要恢复到H，只有在释放锁A之后才能恢复

优先级捐献的范围不应该只局限于通过donee维护的捐献链而是需要拓展到**被捐献线程的所有资源涉及到的所有线程**

如果将整个优先级捐献系统建模为网络的话，我们能将线程和锁的结构分为两个维度：
1. 横：每个线程持有的锁；
2. 纵：线程优先级捐献的对象；

每个线程持有的锁之间通过链表产生联系（恢复记录列表）：
1. 线程获取锁的时候需要想此链表中压入一个元素，释放锁时弹出；
2. 元素中需要含有最近一次获取的锁的指针（用于比较）以及释放锁时需要恢复的优先级；

线程优先级捐献的对象通过TCB->donee进行维护：
1. 线程需要将目标锁的当前持有者放入其中（无论相对优先级高低）其中不能简单保存TCB，而是需要保存&lock-holder，以便始终能获取到最正确的数据；
2. 线程获取到锁之后就将其设置为NULL；

此时的优先级捐献就应该从纵横两个维度来完成：
1. 如果需要获取的锁是holder的顶层锁：此线程更新holder的e_pri即可；
2. 如果需要获取的锁不是holder的顶层锁，但是holder没有捐献优先级给其他线程：此线程更新tab中，位于锁恢复记录之前的所有回复记录的优先级即可
3. 如果holder有捐献优先级给其他线程的话（需要获取的锁必然不是holder的顶层锁）：此线程更新tab中，位于锁恢复记录之前的所有回复记录的优先级然后获取holder的捐献对象holder->donee的捐献记录更新holder->donee中位于holder->donated_for之前所有捐献记录的优先级不断重复这个过程直到holder->donee==NULL为止
## Rationale

性能还是比较好的，禁用中断的地方仅出现在操作队列和state的时候，其他时候并不需要禁用中断

同时如果锁是FREE的话之间递减state即可，无需执行函数调用，检查wait queue（lock release也是类似的）


## Changes

### Data Structure

没有采用额外添加新结构体的方式实现捐献历史记录恢复，而是直接在锁中添加字段`pri`，保存线程释放锁的时候应当恢复的目标优先级（`struct lock`中的其他字段没有修改）

~~~c
struct lock {
  struct thread* holder;      /* Thread holding lock (for debugging). */
  int8_t state;               // 当前锁的状态，3种状态仿照Linux2中内核锁实现
  struct semaphore semaphore; /* Binary semaphore controlling access. */
  int8_t pri;                 // 释放锁的时候需要恢复为此优先级
  struct list_elem elem; // 如果锁被某一个线程获取，那么它位于线程的donated_record_tab中
};
~~~

TCB也与原设计文档存在部分偏差：
~~~c
  /* Strict Priority Scheduler相关 */
  struct rw_lock lock;        // 修改TCB之前需要获取此锁
  struct lock* donated_for;   // 线程最近一次接收优先级捐献由哪一个锁诱发？
  struct list lock_queue;   // 线程当前持有的锁的队列，按锁的优先级进行排列
  int8_t b_pri;               // 线程的基本优先级.
  int8_t e_pri;               // 线程的实际优先级
    
  /* Shared between thread.c / synch.c. / timer.c */
  struct list* queue;    /* 当前位于什么队列中（如果是timer的话值为NULL） */
~~~

### Algorithms

#### 优先级调度

优先级调度的逻辑又两个部分进行确保，第一是准备队列，第二是信号量的等待队列。两者都需要确保一个不变性：**队列位置靠前 TCB 的`e_pri`必然大于等于队列位置靠后 TCB 的`e_pri`**

以准备队列为例，如果队列中已存在和新TCB实际优先级相等的TCB，那么需要将这个新的TCB插在同等优先级TCB的最后位置，具体的逻辑使用`list_insert_order`的`grater`语义进行确保

这部分工作完全交给信号量进行处理，令优先级捐献逻辑和优先级调度逻辑解耦，信号量部分代码如下：
~~~c
void sema_down(struct semaphore* sema) {
  ASSERT(sema != NULL);
  ASSERT(!intr_context());

  enum intr_level old_level = intr_disable();
  while (sema->value == 0) {
    list_insert_ordered(&sema->waiters, &thread_current()->elem, &thread_before, &grater_thread_pri);
    thread_current()->queue = &sema->waiters;
    thread_block();
  }
  sema->value--;
  intr_set_level(old_level);
}
~~~
考虑到Race Condition，代码需要在禁用中断环境下完成如下工作：
- `list_insert_ordered` 根据线程的实际优先级，将TCB插入等待队列当中；
- `thread_block` 将当前线程状态标记为`THREAD_BLOCK`并阻塞当前线程；

提升信号量的代码如下：
~~~c
void sema_up(struct semaphore* sema) {
  ASSERT(sema != NULL);

  struct thread* t = NULL;
  DISABLE_INTR({
    if (!list_empty(&sema->waiters)) {
      t = list_entry(list_pop_front(&sema->waiters), struct thread, elem);
      thread_unblock(t);
    }
    sema->value++;
  });

  // 优先级队列实现，由信号量全权负责
  if (t != NULL && !intr_context() && t->e_pri > thread_get_priority())
    thread_yield();
}
~~~
同样在禁用中断环境下对等待队列和准备队列进行操作：
- 注意需要确认队列中有TCB的时候才执行弹出操作；
- 考虑到此队列中TCB必然按照其大小顺序进行排序，因此弹出的第一个TCB必然是队列中优先级最大的TCB；
- 退出中断禁用环境之后，需要检查新弹出的线程的优先级是不是比自己的大，如果是，那么需要yield CPU；
  - 考虑到中断处理程序可能调用这个函数，而中断处理程序不是线程，不可yield，因此有必要检查当下是不是中断环境；

#### 优先级捐献

优先级捐献部分基本和原文档中的设计相同（虽然那个设计也是后来增补的）不变的地方在于：
- 依然存在一个类似于回复记录的列表 `lock_queue`；
- “纵”与“横”依然是由TCB中的`donee`和`lock_queue`进行维护；

##### `lock_acquire`

变化的地方在于：
~~~c
...
if (--lock->state < 0) {
  if (t->e_pri > lock->pri){
    // 无论自身优先级如何，必须先设置自己的捐献对象
    t->donated_for = lock;
    donate_pri_acquire(t, lock);
  }
  sema_down(&lock->semaphore);
} else
  lock->semaphore.value = 0;

// 线程已获取锁
lock->pri = t->e_pri;
lock->holder = t;
t->donated_for = NULL;
list_insert_ordered(&t->lock_queue, &lock->elem, &lock_before, NULL);
...
~~~
优先级记录记录的是**线程在持有这把锁的时候，应当拥有的最小优先级**，而不是线程在释放这把锁的时候应该恢复的目标优先级。锁的优先级有两个不同的来源：
  - 获取锁时记录当下线程的`e_pri`；
  - 优先级捐献的时候可能修改某一把锁的优先级；

注意需要只有在获取锁之后才将锁放入自己的列表，要不然会引起混乱

为什么无论自身`e_pri`和锁的`e_pri`之间的大小关系都需要设置自己的`donated_for`为`lock`呢？

考虑线程H看中了线程L持有的锁A，但此时线程L因锁B为线程M持有而阻塞。此时如果线程L没有将自己的`donated_for`设置为线程M，那么线程H就没有办法通过优先级捐献将线程M的优先级设置为H，使得线程H的实际优先级成为了H，发生优先级反转

##### `lock_release`

变化的地方在于：
~~~c
...
lock->holder = NULL;
lock->pri = 0;
// 先将自己的优先级恢复为合适值
list_remove(&lock->elem);
if (list_empty(&t->lock_queue))
  t->e_pri = t->b_pri;
else
  t->e_pri = list_entry(list_begin(&t->lock_queue), struct lock, elem)->pri

// 再正式释放锁
if (++lock->state < 1)
  sema_up(&lock->semaphore);
else
  lock->semaphore.value = 1;
...
~~~
线程在释放锁的时候，需要做的是将列表中属于这把锁的优先级记录弹出来（使用`list_remove`，这把锁不一定是线程当前持有锁中优先级最高的那一个），然后将自己的`e_pri`同步为列表顶部优先级记录中的优先级，以此确保自己的优先级始终正确；
  - 线程获取锁的时候需要将此时的`e_pri`记录到其中，不过释放锁的时候并不会使用其中的优先级恢复自己的优先级；
  - 以上两端代码的顺序不可颠倒，必须先恢复自己的优先级为释放锁之后本线程的实际优先级（主要是考虑到其他锁也可能捐献给自己优先级），以防优先级队列不能将自己放在正确的位置；
  - 这里有一个和`sema_up`联动的过程，实际语义就是比较此线程释放锁之后的`e_pri`和锁等待队列顶部线程的`e_pri`之间的大小关系，从而确保整个系统始终是优先级最大的线程正在运行（当然不是用能释放锁之前的优先级去比较，那不成了左右互搏了吗）；

##### `donate_pri_acquire`
基本完全发生了变化：
~~~c
static void donate_pri_acquire(struct thread* self, struct lock* lock) {
  struct thread* donee = lock->holder;
  struct thread* donor = self;
  while (donor->e_pri > donee->e_pri) {
    // 只有是针对不同资源的优先级捐献才触发捐献历史保存
    struct lock* record = NULL;
    record = donor->donated_for;
    donee->e_pri = donor->e_pri;
    record->pri = donor->e_pri;

    // 重排锁在资源队列中的顺序
    list_remove(&record->elem);
    list_insert_ordered(&donee->lock_queue, &record->elem, &lock_before, NULL);

    // 重排线程在Ready Queue/Wait List队列中的顺序
    if (donee->queue != NULL) {
      list_remove(&donee->elem);
      list_insert_ordered(donee->queue, &donee->elem, &thread_before, &grater_thread_pri);
    }

    if(donee->donated_for == NULL)
      break;
    donor = donee;
    donee = donee->donated_for->holder;
  }
}
~~~
此函数的实现是整个优先级捐献的核心部分：遍历捐献链，更新链上TCB的捐献原因锁`donated_for`的优先级以及`e_pri`：
- 校验是否需要执行优先级捐献：`while (donor->e_pri > donee->e_pri)`，确保`donor`的优先级大于`donee`的优先级；
- 获取`donor`因为哪一把锁发生优先级捐献`donated_for`，更新这把锁的优先级和`donee`的优先级；
- 重排锁在`lock_queue`中的顺序和线程在`Ready Queue/Wait List`中的顺序；
- 如果`donee`还有优先级捐献对象，继续遍历捐献链直到`donated_for`为`NULL`;

相比于此前的所有尝试，此实现有一个很大的不同就是将“被捐献的优先级”和锁绑定在一起而不是线程绑定在一起，是因为线程持有这把锁而拥有指定优先级而不是因为线程本身就该有这样的优先级。

这样就算同一个线程因为不同的锁，接收到了不同线程的优先级捐助也不会引发混乱，毕竟优先级跟随锁而存在，不跟随线程而存在

因此线程执行优先级捐献逻辑的时候的实际更新对象是所有**影响自身获取目标锁的锁**而不是持有这些锁的线程，整体逻辑是：如果我想获取这把锁，前提条件是这把锁的持有者释放这把锁，但是如果想要做到这一点，必须让这条线程起来运行直到释放这把锁为止。为了让这条线程起来运行，这条线程也必须获取阻碍它继续运行的锁（这个过程一直连续下去便是捐献链的概念）。那么我就可以将自己的优先级给它们，直到线程释放它们手中的、涉及到捐献链形成的锁才恢复它们应该有的优先级

总的来看：
- “恢复记录”的语义发生了一定变化：
  - 从原来的“释放锁之后应当恢复的目标优先级”变为了“这把锁带给此线程的优先级”，即**优先级记录**；
- “恢复记录列表”的语义也发生了变化：
  - 从“线程的恢复记录列表”变为了“线程当前持有的锁的列表”；
  - 根据“锁的优先级”降序排列其中的元素，因此可以确保列表顶部锁的优先级必然是线程当前持有锁中优先级最大的那一把；
- `lock_acquire`不在获取锁的时候使用`malloc`分配捐献记录结构体，而是直接使用`struct lock`作为获锁时优先级的记录载体；
  - 直接原因是在内存分配器没有准备好的时候`lock_acquire`就会被调用，导致陷入循环调用直接爆栈；
  - 间接原因是没有必要这样做，必经一把锁只能为一个线程持有，没有必要引入一个新的结构体；

##### `thread_set_priority`

设置线程的基本优先级，详见注释
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
    
    struct lock pcb_lock;      /* PCB中非列表字段的锁 */
    struct condition pcb_cond; /* 条件变量 */
    struct list threads;       /* 元素是TCB */
    struct bitmap* stacks; /* 进程已经在虚拟内存空间中分配了多少个栈？（Bitmap） */
    bool exiting;          /* 进程是否正在执行exit函数？ */
    struct thread* thread_exiting; /* 当前是否有函数正在执行process_exit */
    uint32_t pending_thread;       /* 当前有多少个内核线程还在执行 */
    ...
}
~~~

- `uint32_t pending_thread`：条件变量
  - `exit`第一次检查线程列表的时候，如果遇见线程`in_hanlder==true`就会将这个值`++`；
  - `syscall_hanlder`如果需要执行`thread_exit`，那么就需要将这个值`--`，一旦这个值归零，那么发出`signal`；
- `bool exiting` 初始为`false`；
- `struct thread *thread_exiting` 初始为`NULL`

注意:
1. `b_pri`只能通过`thread_set_priority`设置；
2. `e_pri`则通过优先级捐献进行设置；
3. `thread_get_priority`会返回`e_pri`；

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
    struct process* pcb;        /* Process control block if this thread is a userprog */
    size_t stack_no;            /* 线程的虚拟内存栈编号 */
    struct list_elem prog_elem; /* 进程线程列表元素 */
    struct thread* joined_by;   /* 指向join当前线程的TCB */
    struct thread* joining;     /* 指向当前线程正在join的线程的TCB */
	#endif
    struct lock tcb_lock;       // TCB中非列表字段的锁
    ...
}
~~~

## Algorithms

线程系统的核心是三个变量：`in_handler`/`pending_thread`/`exiting`:
- `in_handler`可以表示当前线程是否在执行系统调用处理程序时，被中断处理程序调度出去：
  - 进入系统调用的时候将设置为`true`，离开时设置为`false`；
  - 进程退出时需检查线程此变量，决定要不要直接将其释放；
- `pending_thread`用于表示系统中尚有多少个线程位于内核；
  - 进入系统调用的时候将其递增，离开时递减；
  - `process_exit`执行时，需根据其中的值决定如何等待未执行完毕的线程，只有最后才递减自己的`pending_thread`；
- `exiting`进程用来表示现在进程是不是已经退出或正在执行`process_exit`；
  - 第一个进入`process_exit`的线程将其设置为`true`；
  - 系统调用处理程序离开时，需要判断这个值以及`pend_thread`，确定接下来的行动；

`process_exit`退出的时候需要遍历两次线程队列，进程正常退出的时候考虑的是在所有内核线程正常退出的基础上释放所有资源，进程异常退出的时候考虑的只有释放所有资源，因此`exit(0)`和`exit(-1)`的执行逻辑有所不同：
- 第一次遍历：目的是让系统中所有位于内核中的线程能正常退出；
  - 以`in_handler`作为释放TCB的指标；
  - 不清除任何线程的资源，仅仅移出准备队列以及设置线程状态；
  - 为了和Join系统协作，需要上溯线程链并妥善处理Join链顶部线程；
  - 迭代完之后就`THREAD_BLOCK`，等待`pending_thread==1`之后被唤醒；
- 第二次遍历：目的是确认本进程所有线程已正常退出；
  - 使用`pending_thread`和`thread_exiting`协作，不使用`join_by`；
  - `pend_thread==1`之后再遍历列表清除剩余线程TCB；
  - `exit(-1)`直接跳到这一步清除所有线程的资源；

同时，线程退出的时候也需要处理一些额外的逻辑：
- 线程退出的时候将自己的状态设置为`THREAD_ZOMBIE`而不是`THREAD_DYING`，同时需要`--pending_thread`；
- 让自己现在或未来的joiner来清除自己的资源，要么就是让`process_exit`代自己完成这一过程；

由于主线程不会将自己的TCB加入线程队列，因此线程退出的时候需要执行一些额外的逻辑

### 线程库函数
#### 线程创建相关函数

可能对函数执行逻辑产生影响的退出事件穿插逻辑如下：
- `pthread_execute`执行时发生退出事件：
  - 系统调用入口和出口处统一执行的`pending_thread`维护逻辑可确保无论在何处发生退出事件都不会影响线程；
- `pthread_execute`创建线程之后，`start_pthread`执行之前发生退出事件：
  - 由于此时`pending_thread>0`其值尚未被`start_pthread`递减，因此可类比作`start_pthread`的第一种情况；
- 线程在执行`start_pthread`时发生退出事件：
  - 发生在`setup_stack`禁用中断之前：函数禁用中断后检查发现`exiting==false`，返回错误，线程清理资源正常退出；
  - 发生在`setup_stack`禁用中断页分配完毕之后，压入PCB线程队列之前：由于注册用户栈操作具有原子性`process_exit`可安全释放用户栈；
  - 发生在压入PCB线程队列之后，`running_when_exiting`执行之前：`running_when_exiting`检查发现`exiting==false`，直接执行退出逻辑；
  - 发生在`running_when_exiting`执行之后：此时`in_handler==false`，被`process_exit`移除准备队列并将状态标记为`THREAD_ZOMBIE`；
##### `pthread_execute`

基本可类比作`process_execute`：

- 执行`bitmap_scan_and_flip`在进程页目录中占据位置，未分配实际用户栈的内存；
- 执行`thread_create`创建子线程，未将线程TCB添加到PCB线程列表中；
- 分配`init_tcb`，用于向`start_pthread`函数传递参数；

##### `start_pthread`

使用`pthread_execute`创建的子线程被第一次被调度时执行的函数：
- 释放`init_tcb`；
- 使用`setup_thread`为用户栈分配内存页并初始化中断帧；
- 将自己的TCB添加到PCB线程队列中；
- 调用`running_when_exiting`，执行`pending_thread`维护逻辑；
- 一切无误时执行汇编，跳转到用户模式；

#### 线程退出相关函数

可能对线程退出函数执行逻辑产生影响的退出事件穿插逻辑如下：
- 执行`pthread_exit`时发生退出事件：
  - 无需添加额外的处理代码；
- 执行`pthread_exit_main`时发生退出事件：
  - 阻塞前发生退出事件：`process_exit`必然在此线程再次被调度之前睡眠，因此只需在阻塞前检查`exiting`就可以将此问题划归为普通的系统调用问题；
  - 阻塞中发生退出事件：`process_exit`进入函数时会帮主线程递减`pending_thread`并将其设置为`THREAD_ZOMBIE`，第二次迭代时释放其资源；

##### `running_when_exiting`

执行`pending_thread`有关处理逻辑，仅可以被用户线程对应的内核线程调用：
- 任何线程退出内核时都需要执行此函数；
- 系统执行外部中断时退出时，可能存在多个第一次未被调度的线程，因此外部中断退出时需要将`thread_main`设置为`NULL`；
- 只有确认外部中断已经释放进程资源时才执行`thread_exit`直接退出，否则执行`pthread_exit`（可能唤醒`joined_by`）；
##### `pthread_exit`

普通线程的退出函数，不可调用`process_exit`退出进程：

作用主要是线程退出，工作如下：
- 退出状态为`THREAD_ZOMBIE`；
- 无需将自己移出PCB线程队列；
- 唤醒`joined_by`：用户栈、虚拟内存空间、TCB都由它释放[^退出状态][^死锁1]；

##### `pthread_exit_main`

由于join主线程的线程不会释放主线程的资源，因此在任何时候`PCB->main_thread`都是合法的

主线程退出函数，基本可视为`process_exit(0)`的包装，但是允许用户代码继续执行[^主线程最后的Join是否可能引发死锁？]：
- 唤醒自己的`joined_by`；
- 禁用中断并校验`exiting`：
  - `true`：执行`running_when_exiting`；
  - `false`：`thread_block`直到`pending_thread==1`被唤醒；
- 调用`process_exit(0)`；

进入`process_exit`之后需要将自己的TCB归入`thread_exiting`，因此外部中断清理的时候需要额外注意一下`thread_exiting`是不是和`main_thread`相同，防止重复释放

##### `process_exit`

进程退出函数，详情见后`exit`系统调用部分
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

另外，由于进程退出有三套不同的处理逻辑，因此最好使用不同的函数将不同的逻辑分隔开：
- `process_exit`改成`process_exit_tail`：
  - 将原来的遍历线程队列部分拆出来，独立为三个不同的部分；
  - 仅存放释放进程资源的代码，如释放锁和父子进程共同部分；
- `pthread_exit_main`：
  - 遍历线程完毕之后设置`thread_exit`并调用`process_exit_tail`；
  - 主线程与`thread_exiting`重合的逻辑在这里处理；
- 新增`process_exit_exception`为异常处理程序调用的进程退出函数:
  - 直接执行线程队列清空逻辑；
  - 执行异常处理程序相关逻辑之后调用`process_exit_tail`；
- 新增`process_exit_normal`为非主线程调用的进程退出函数；
  - 执行完普通线程的退出逻辑之后调用`process_exit_tail`;

##### 整体执行流程

`exit`函数的整体执行流程基本如下（不包括原有的执行流程）：

1. 检查`exiting`和`thread_exiting`字段：

   - `exiting==false`：自己是第一个执行`process_exit`的线程；
     - 设置`exiting=true`；
     - 设置`thread_exiting=thread_current()`；
     - 将`exit_code`同步到PCB中；
   - `exiting==true, thread_exiting==NULL`：代表主线程正在执行`pthread_exit_main`后陷入睡眠；
     - 设置`thread_exiting=thread_current()`；
     - 需要为主线程递减`pending_thread`并将其设置为`THREAD_ZOMBIE`；
     - 将`exit_code`同步到PCB中；
   - `exiting==true, thread_exiting!=NULL`：代表已经有其他线程在自己之前一步执行`process_exit`；
     - 比对并设置`exit_code`；
     - 执行`running_when_exiting`；
   - 进入此函数的上下文是中断处理程序：无需做任何设置操作，直接跳过这一步；    

2. 根据退出码和中断性质的不同`exit`的执行情况可以被分为两类：

- 内部中断执行`exit`：进程执行系统调用`exit`，正常退出：
  - 遍历并等待线程列表：
    - 第一次遍历不清除任何资源，仅将TCB移出准备队列以及设置线程状态，全程禁用中断；
    - 第二次遍历用于等待内核中线程执行完毕，使用`pending_thread`维护；
    - 在遍历之前如果检查到`exiting==true`但是`thread_exiting==NULL`的话，就可以断定在主线程执行`main_thread_exit`的时候被当前线程抢先了，因此这时候同时也需要将主线程移出等待队列并将其状态标记为`THREAD_ZOMBIE`；
  - 第一次遍历:
    - 禁用中断且提升优先级可确保外部中断不可能干扰这个过程；
    - 先将自己的TCB从队列中移除（`list_remove`），然后唤醒自己的`join_by`；
    - 需将所有遇到的`in_handler==false`的线程标记为`THREAD_ZOMBIE`并将其TCB移出准备队列。如果它有`joined_by`，唤醒之（不要漏掉主线程）；
    - 对于`in_handler==true`的线程来说，设置`exiting`的这个操作就足够令其退出了；
  - 第二次遍历：
    - 直接`thread_block`，被唤醒之后确保不变性：`ASSERT(pending_thread==0)`；
    - 随后释放线程列表中的所有线程资源即可（如果当前线程不是主线程，还需要释放`main_thread`的资源）；
- 外部中断执行`exit`：跳过第一次遍历，直接释放线程列表中所有线程的资源：
  - 执行时行为：
    - 首先将自己的TCB从线程队列中移出；
    - 释放`thread_exiting`的资源；
    - 确认自己是不是主线程，如果不是，那么需要释放主线程资源；
    - 遍历线程列表，清除其中所有线程的资源。但是要注意每清除一个`in_handler`线程的资源都需要递减`pending_thread`；
  - 两种触发外部中断的可能：
    - 第一种可能是其他系统调用引发Page Fault之类的异常，触发`exit(-1)`；
    - 第二种可能是在之前内部中断触发的`exit`第一次遍历线程列表完毕，阻塞之后，类似第一种情况引发的异常触发外部中断：
  - 其他注意事项：
    - 考虑到外部中断环境线程必不可能被中断，因此获取锁之类的操作都不需要，直接释放所有线程的资源即可；
    - 考虑到新创建的线程可能在没有执行`start_process`之前就被外部中断退出事件抢先，因此外部中断在退出的最后，如果在递减自己的`pending_thread`之后发现它仍然不为$0$，那么就不能`free(PCB)`，需要让那些未被调度过的新线程来帮助自己完成这一过程；

3. 释放资源，在原基础上，还需要释放这些东西[^exit()中的线程表]：

   - 进程持有的所有的内核资源（锁），其中属于本进程的锁直接FREE内存即可；
     - 采取的策略是让`exit`线程`wait`所有拥有内核资源的线程，这些线程系统调用退出的时候需要检查PCB的`exiting`字段。如果是`true`那么需要执行`thread_exit`，这种情况也算是`thread_yield`，因此不需要设置为`THREAD_DYING`；

   - 内核锁引用计数为0的所有线程的TCB，检索进程的线程列表即可；
     - 考虑到`exit()`线程会join所有线程，而`thread_join`会清除僵尸线程的TCB，因此处于`THREAD_ZOMBIE`的线程不需要担心资源释放问题；
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

总体来看，实现的关键无非这么几点：
1. 访问可能被外部中断处理程序及相关调用函数访问的变量时，必须禁用中断以阻止Race Condition；
2. `pending_thread`、`exiting`、`in_handler`协作控制进程退出时尚位于内核中线程的行为：

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
#### `pending_thread`

~~~c
uint32_t pending_thread; /* 当前有多少个内核线程还在执行 */
~~~

1. 任何对`pending_thread`以及`in_handler`的操作都必须禁用中断;
2. 系统在进入系统调用处理程序的时候需要`++pending_thread`、`in_handler=true`，离开时需要`--pending_thread`、`in_handler=false`;
3. 下面是一些特殊情况：
   - `thread_create`: 进入之前需要再次`++pending_thread`，应对此函数执行过程中发生进程退出的情形。初始化新线程的时候需要将新线程的`in_handler=true`；
   - `start_thread`: 将自己添加到PCB线程队列之后再`--pending_thread, in_handler=false`;
   - `thread_exit`: 由于执行此系统调用不会返回，因此也需要`--`;
   - `process_exit`: 如果到最后`pending_thread`依然不是$1$（可能在`thread_execute`和`start_thread`将TCB入列的间歇执行了`process_exit`），那么`--pending_thread`但是不释放PCB；
4. 上述所有操作`pending_thread`的线程都需要实现这一个逻辑：
   - 如果本线程递减`pending_thread`发现其值为1且`exiting==true`，那么需要通知`thread_exiting`或`main_thread`；
   - 由内部中断引发的`process_exit`的线程如果在第一次遍历队列之后发现`pending_thread!=1`那么就`thread_block`，直到被叫起来为止； 
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