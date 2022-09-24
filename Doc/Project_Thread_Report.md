# Changes

## Lock

下面是本项目第二部分，优先级捐献锁的最终实现文档

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


## User Thread

下面是本项目第三部分，线程系统的最终实现文档

### Data Structure

#### process.h

相比于原来的设计，最终版本的设计变化不多，基本就是进一步改进了原有的设计：
1. `exiting_status exiting`用于表示当前退出事件等级，各退出函数需要根据这个字段来决定自己接下来的行为；
2. `exit_code`各退出函数需要使用`set_exit_code`从所有的退出码中竞选出所有退出码中最后的赢家，`process_exit_tail`执行时会将这个值同步到`struct child_process`中作为进程最后的退出码；
3. `in_kernel_threads`用于表示现在由多少个线程位于内核，即通过执行`syscall`进入内核，或通过处理器异常进入`kill`的`SEL_UCSEG`分支（实际就是将原来的`pending_thread`改了个名字）；
4. `active_threads`用于表示系统中还有多少个个线程还没有退出，`pthread_exit_main`会参考这个值来决定主线程的退出行为；

其他字段的语义基本没有发生变化

~~~c
struct process {
    ...
    // 线程系统相关
    struct rw_lock threads_lock; /* 线程队列读写锁 */
    struct list threads;         /* 元素是TCB */
    struct bitmap* stacks; /* 进程已经在虚拟内存空间中分配了多少个栈？（Bitmap） */
    enum exiting_status exiting;   /* 退出事件等级 */
    int32_t exit_code;             /* 临时进程退出码 */
    struct thread* thread_exiting; /* 当前是否有函数正在执行process_exit */
    uint32_t in_kernel_threads;    /* 有多少线程位于内核中？ */
    uint32_t active_threads;       /* 有多少线程位于尚未退出中？ *//
    ...
}

/* 进程退出事件级别 */
enum exiting_status {
  EXITING_NONE, /* 未发生任何退出事件 */
  EXITING_MAIN, /* 主线程正在执行pthread_exit_main */
  EXITING_NORMAL, /* 执行系统调用exit触发的退出事件 可能位于退出进程的后半段 */
  EXITING_EXCEPTION /* 进程已因异常退出 */
};
~~~

#### thread.h

结构体定义没有发出变化

### Algorithms

#### Join系统

Join系统的实现分为两个函数：
1. `pthread_join`: 线程join其他函数的时候执行的函数；
2. `wake_up_joiner`: 线程退出时执行此函数唤醒 joiner；

为了和进程退出函数正确协作，执行`pthread_join`的全过程（包括阻塞），都保证`thread_current()->in_handler==true`以及`in_kernel`有自己的一点

##### `pthread_join`

执行全过程：
1. 于PCB线程列表中寻找`pos->tid == tid`的线程；
2. 上溯`join`链；
3. 根据上溯结果：
   1. 返回错误；
   2. 返回`tid`；
   3. 阻塞；
4. 可能释放被join线程资源；

下面是实现中值得一提的东西

###### 第一步

搜索线程队列的时候，需要考虑到有可能执行`thread_create`之后，线程执行`start_thread`将自己添加到线程队列之前，新线程已经被Join的情况。为此，`pthread_execute`中需要添加一个信号量，确保只有新线程执行`start_pthread`之后`start_thread`才返回。

`pthread_execute`需要和`start_pthread`的执行同步:
~~~c
  ...
  struct semaphore finished;
  sema_init(&finished, 0);
  init_tcb->finished = &finished;
  tid_t tid = thread_create("sub thread", PRI_DEFAULT, &start_pthread, init_tcb);
  if (tid == TID_ERROR) {
    free(init_tcb);
    bitmap_set(pcb->stacks, stack_no, false);
  }
  sema_down(&finished);
  ...
~~~

同时，在线程队列中搜索到指定线程之后，还需要检查线程是不是主线程，后面的逻辑会用到这一条信息。

###### 上溯`join`链

基本上看注释就可以了，尽管在上溯的过程中不用获取锁，但是在循环内部需要获取TCB的`join_lock`以及禁用中断。

由于离开循环时链顶部线程必然没有在join任何其他的线程，同时需要满足这些条件才能离开循环：
- 链顶线程处于运行状态：
   - 但是该线程已经在当前线程上溯的过程中join了自己,也就是链顶线程成了自己，此时就不可join该线程；
   - 不过该线程就算想要join自己，但是由于它没有抢在自己之前join自己，因此链顶线程就是该线程自己，可以安全join该线程；
- 链顶线程处于僵尸状态：
  - 线程没有被join过，这时候就能安全地join它；
  - 线程被join过，有可能本线程执行上溯循环的间歇，循环下一次的目标线程恰好被调度运行同时退出为僵尸。由于线程退出以及唤醒joiner的过程是原子的，因此这时候它的joiner已经被唤醒。因此它的joiner就有可能在它执行的过程中join自己，循环就需要倒退到这个joiner上继续（尽管`chain->joining == NULL`）；

为了能在接下来的逻辑中，确保链子顶部的线程不会直接或间接地join自己，因此在离开循环的时候，中断仍然处于禁用状态（相当于join链的状态就被固定在这一刻），链子顶部TCB的`join_lock`依然确保能被当前线程持有。由此确保线程链状态在离开循环时的状态，与线程设置自己的`joining`这一操作时的状态处于同一个原子内。

###### 执行判断逻辑

> 执行判断逻辑是全程禁用中断，因此无需担心Race Condition

这里基本没有什么额外的工作需要做，唯一需要注意在`unjoinable!=true`的情况下，还需要检查进程是不是处于`EXITING_MAIN`的同时本线程的join对象是主线程。如果是，那么这时候就需要直接释放锁然后返回`tid`


###### 释放线程资源

这里需要注意的是，如果join对象是主线程的话，那么不能释放主线程任何资源。这主要是考虑到主线程可能在执行`pthread_exit_main`等待进程线程执行完毕的时候，被用户线程join，那么这时候如果将它的资源清空，并且同时系统中也没有其他线程执行`exit`，那么就会造成进程资源没得被释放。

最后，在释放线程内核栈之前，必须将自己持有的，该线程的锁释放掉，确保自己的获锁队列没有被标记释放的内存（与Commit记录中提到的`free`的执行问题一样）

##### `wake_up_joiner`

> 执行此函数之前，需要持有本线程的`join_lock`。同时，这个函数只可能被各种退出函数调用，用于唤醒各种线程的joiner

`wake_up_joiner`执行时会检查线程的`joiner`，如果不是`NULL`的话就会获取它的`join_lock`，随后就唤醒它

由于整个系统中只有这个函数会获取多把锁，因此只注意控制这个函数的获锁顺序就能控制死锁。至于为什么这个函数不可能触发死锁呢？核心还是确保获锁的单向性，毕竟只要能确保join的单向性就能确保获取锁的单向的

#### 进程退出系统

> 考虑到异常处理程序可能会调用退出函数，因此所有和退出系统字段相关的访问都需要禁用中断

为了协同join系统，用户线程有两种不同的退出态：
- `THREAD_ZOMBIE`
  - 除内核栈和TCB以外，所有的资源都被释放；
  - 不位于任何准备队列和等待队列中，再不会被调度；
  - 需要其他线程来释放其资源；
- `THREAD_DYING`
  - 调度器执行上下文切换是会将其清除；

根据被调用的上下文不同，有三个不同的进程退出系统函数：
- `pthread_exit_main`
  - 主线程执行`pthread_exit`时调用的函数；
  - 退出事件等级为`EXITING_MAIN`；
  - 等待进程中所有线程正常退出之后再清理进程资源；
  - 如果系统中现时存在`EXITING_NORMAL`或`EXITING_EXCEPTION`退出事件，直接退出；
- `process_exit_normal`
  - 用户代码触发处理器异常、进程任一线程调用`exit`都时执行函数；
  - 退出事件等级为`EXITING_NORMAL`；
  - 等待进程所有位于内核中的线程执行完成，直接清理位于用户态的线程；
  - 如果系统现时存在`EXITING_EXCEPTION`退出事件，直接退出；
- `process_exit_exception`
  - 仅内核代码触发处理器异常时会调用此函数；
  - 退出事件等级为`EXITING_EXCEPTION`；
  - 直接清理进程所有线程的资源；

###### 退出事件等级

退出事件等级使用PCB中的`enum exiting_status exiting`表示，用于表示进程是否正在发生任何形式的退出事件：
~~~c
/* 进程退出事件级别 */
enum exiting_status {
  EXITING_NONE, /* 未发生任何退出事件 */
  EXITING_MAIN, /* 主线程正在执行pthread_exit_main */
  EXITING_NORMAL, /* 执行系统调用exit触发的退出事件 可能位于退出进程的后半段 */
  EXITING_EXCEPTION /* 进程已因异常退出 */
};
~~~

特点是任何退出函数执行时都会检查现在的退出事件等级，如果现在的退出事件等级比自己的退出事件等级低，那么会将自己的退出事件等级覆盖上去并执行自己的退出逻辑，反之直接退出

###### `exit_if_exiting`

`exit_if_exiting`是整个退出系统的核心，它最主要的作用是在线程离开内核时对其执行退出检查，根据当下退出事件等级确定要不要令线程直接退出（而不是回到用户态）以及以何种方式退出（调用`pthread_exit`或者`thread_exit`），但无论是何种情况，递减`in_kernel_threads`和设置`in_handler`是一定会做的：
- `EXITING_NONE`：直接回到用户态；
- `EXITING_MAIN`且线程通过`pthread_exit`系统调用进入：检查系统活跃线程数量，有必要就唤醒主线程，随后调用`pthread_exit`退出；
- `EXITING_NORMAL`：检查系统位于内核中的线程数量，有必要的话唤醒退出线程，随后执行`pthread_exit`退出；
- `EXITING_EXCEPTION`：如果进程位于内核中的线程只有自己了（存在外部中断退出无法清除的线程）那么除了需要调用`thread_exit`退出的同时，还需要释放PCB；

此函数会在两个位置被调用：
- 线程执行系统调用完毕，离开内核时；
- 非主线程执行`pthread_exit`；

###### `pthread_exit`

`pthread_exit`是一个静态函数，仅会被`exit_if_exiting`调用，同时所有非主线程用户线程退出时基本都需要调用此函数退出。它有如下几项任务：
- 唤醒线程Joiner；
- 递减`active_threads`；
- 释放自身用户栈；
- 将自己从队列中移除；
- 将自己标记为僵尸之后退出；

###### `pthread_exit_main`

以PCB中的`active_threads`为指标，只有现时系统中仅有自己时才执行进程退出逻辑，否则持续睡眠

得益于所有线程自然推出的缘故，清理进程资源的时候只需要将线程队列中所有线程的内核栈清理掉即可，无需关注用户栈

随后就是调用`process_exit_tail`执行线程统一的清理逻辑

###### `process_exit_normal`

`process_exit_normal`的执行可分为如下几个阶段：
- 检查退出事件等级；
  - 如果当前退出事件等级大于等于`EXITING_NORMAL`执行`exit_if_exiting`退出（可能多个用户线程同时执行系统调用`exit`）；
  - 如果现时退出事件是`EXITING_MAIN`，那么由于主线程已经执行完初步的退出逻辑，正在等待所有用户线程退出，它也就不该再被继续执行，因此需要手动让他成为僵尸；
- 第一次遍历线程队列；
  - 本次遍历不会弹出线程队列中的任意一个线程，因此获取读者锁即可；
  - 如果遇到那些不是位于内核中的函数，不能将他们的TCB直接清除，而是需要将它们变为`THREAD_ZOMBIE`。这主要是考虑到其他位于内核中的用户线程可能需要Join这些位于用户态的线程（也有可能已经Join了这些用户态线程，需要手动唤醒它们），因此为了不干扰它们的正常逻辑，只是将它们变为`ZOMBIE`；
- 第二次遍历线程队列；
  - 第一次遍历完毕之后，阻塞直到`in_kernel_threads == 1`才开始第二次遍历；
  - 第二次遍历只需要清理内核栈，用户栈会被之后调用的`process_exit_tail`同一清理；

注意，代码中设置`struct thread* thread_exiting`这一步是非常有必要的，如果线程在`process_exit_normal`的第一次遍历和第二次遍历的间歇，内核态线程触发`process_exit_exception`，那么异常处理程序就需要使用这个字段将`process_exit_normal`线程清除

同时，由于此函数有可能会在用户代码触发异常时被调用。因此在`exception.c`处添加了和`syscall.c`相同的推出代码

###### `process_exit_exception`

`process_exit_normal`的执行就粗暴许多：
- 根据现时退出等级，决定是否需要做一些额外的工作；
  - 比如说将`struct thread* thread_exiting`放回到线程队列中统一处理；
- 考虑到有可能会遗留一些没有被放到线程队列中的线程（`start_pthread`未执行就触发异常），因此处理线程队列的时候需要维护`in_kernel_threads`字段；

###### `process_exit_tail`

执行统一的进程资源清理逻辑，前身是`process_exit`，现在成了仅会被上面三个退出函数调用的静态函数

为什么三个进程退出函数无需关注用户栈清理呢，根本原因是只要登记到进程页目录的虚拟地址（及其物理内存页）都能被如下代码块清除：
~~~c
  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = pcb_to_free->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    pcb_to_free->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }
~~~

同时要注意，为了避免由`free`本身结构导致的Page Fault，需要在弹出线程当前持有的所有锁之后再释放信号量和锁队列

其他基本和原来`process_exit`的设计相同
