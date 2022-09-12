#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/switch.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* Random value for struct thread's `magic' member.
   Used to detect stack overflow.  See the big comment at the top
   of thread.h for details. */
#define THREAD_MAGIC 0xcd6abf4b

/* List of processes in THREAD_READY state, that is, processes
   that are ready to run but not actually running. */
static struct list ready_list;

/* Strict Priority Scheduler 的 Ready Queue，列表中越靠前优先级越高 */

/* List of all processes.  Processes are added to this list
   when they are first scheduled and removed when they exit. */
static struct list all_list;

/* Idle thread. */
static struct thread* idle_thread;

/* Initial thread, the thread running init.c:main(). */
static struct thread* initial_thread;

/* Lock used by allocate_tid(). */
static struct lock tid_lock;

/* Stack frame for kernel_thread(). */
struct kernel_thread_frame {
  void* eip;             /* Return address. */
  thread_func* function; /* Function to call. */
  void* aux;             /* Auxiliary data for function. */
};

/* Statistics. */
static long long idle_ticks;   /* # of timer ticks spent idle. */
static long long kernel_ticks; /* # of timer ticks in kernel threads. */
static long long user_ticks;   /* # of timer ticks in user programs. */

/* Scheduling. */
#define TIME_SLICE 4          /* # of timer ticks to give each thread. */
static unsigned thread_ticks; /* # of timer ticks since last yield. */

static void init_thread(struct thread*, const char* name, int priority);
static bool is_thread(struct thread*) UNUSED;
static void* alloc_frame(struct thread*, size_t size);
static void schedule(void);
static void thread_enqueue(struct thread* t);
static tid_t allocate_tid(void);
void thread_switch_tail(struct thread* prev);

static void kernel_thread(thread_func*, void* aux);
static void idle(void* aux UNUSED);
static struct thread* running_thread(void);

static struct thread* next_thread_to_run(void);
static struct thread* thread_schedule_fifo(void);
static struct thread* thread_schedule_prio(void);
static struct thread* thread_schedule_fair(void);
static struct thread* thread_schedule_mlfqs(void);
static struct thread* thread_schedule_reserved(void);

/* Determines which scheduler the kernel should use.
   Controlled by the kernel command-line options
    "-sched=fifo", "-sched=prio",
    "-sched=fair". "-sched=mlfqs"
   Is equal to SCHED_FIFO by default. */
enum sched_policy active_sched_policy;

/* Selects a thread to run from the ready list according to
   some scheduling policy, and returns a pointer to it. */
typedef struct thread* scheduler_func(void);

/* Jump table for dynamically dispatching the current scheduling
   policy in use by the kernel. */
scheduler_func* scheduler_jump_table[8] = {thread_schedule_fifo,     thread_schedule_prio,
                                           thread_schedule_fair,     thread_schedule_mlfqs,
                                           thread_schedule_reserved, thread_schedule_reserved,
                                           thread_schedule_reserved, thread_schedule_reserved};

/* Initializes the threading system by transforming the code
   that's currently running into a thread.  This can't work in
   general and it is possible in this case only because loader.S
   was careful to put the bottom of the stack at a page boundary.

   Also initializes the run queue and the tid lock.

   After calling this function, be sure to initialize the page
   allocator before trying to create any threads with
   thread_create().

   It is not safe to call thread_current() until this function
   finishes. */
void thread_init(void) {
  ASSERT(intr_get_level() == INTR_OFF);

  lock_init(&tid_lock);
  list_init(&ready_list);
  list_init(&all_list);

  /* Set up a thread structure for the running thread. */
  initial_thread = running_thread();
  init_thread(initial_thread, "main", PRI_DEFAULT);
  initial_thread->status = THREAD_RUNNING;
  initial_thread->tid = allocate_tid();
}

/* Starts preemptive thread scheduling by enabling interrupts.
   Also creates the idle thread. */
void thread_start(void) {
  /* Create the idle thread. */
  struct semaphore idle_started;
  sema_init(&idle_started, 0);
  thread_create("idle", PRI_MIN, idle, &idle_started);

  /* Start preemptive thread scheduling. */
  intr_enable();

  /* Wait for the idle thread to initialize idle_thread. */
  sema_down(&idle_started);
}

/* Called by the timer interrupt handler at each timer tick.
   Thus, this function runs in an external interrupt context. 
   
   每发生一次计时器中断这个中断处理程序就会被调用（此时中断已被禁用）
   作用是递增内核计时（ticks）供调度器做出正确判断
   如果递增过后发现是时候需要执行调度器了（thread_ticks >= TIME_SLICE）
   那么就会调用intr_yield_on_return设置一个旗标（flag）

   intr_handler退出的时候就会检查这个旗标，根据这个旗标中的内容
   确定是否需要执行调度算法
    */
void thread_tick(void) {
  struct thread* t = thread_current();

  /* Update statistics. */
  if (t == idle_thread)
    idle_ticks++;
#ifdef USERPROG
  else if (t->pcb != NULL)
    user_ticks++;
#endif
  else
    kernel_ticks++;

  /* Enforce preemption. */
  if (++thread_ticks >= TIME_SLICE)
    intr_yield_on_return();
}

/* Prints thread statistics. */
void thread_print_stats(void) {
  printf("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n", idle_ticks, kernel_ticks,
         user_ticks);
}

/* Creates a new kernel thread named NAME with the given initial
   PRIORITY, which executes FUNCTION passing AUX as the argument,
   and adds it to the ready queue.  Returns the thread identifier
   for the new thread, or TID_ERROR if creation fails.

   If thread_start() has been called, then the new thread may be
   scheduled before thread_create() returns.  It could even exit
   before thread_create() returns.  Contrariwise, the original
   thread may run for any amount of time before the new thread is
   scheduled.  Use a semaphore or some other form of
   synchronization if you need to ensure ordering.

   The code provided sets the new thread's `priority' member to
   PRIORITY, but no actual priority scheduling is implemented.
   Priority scheduling is the goal of Problem 1-3. */
tid_t thread_create(const char* name, int priority, thread_func* function, void* aux) {
  struct thread* t;
  struct kernel_thread_frame* kf;
  struct switch_entry_frame* ef;
  struct switch_threads_frame* sf;
  tid_t tid;

  ASSERT(function != NULL);

  /* 
   * Allocate thread. 
   * 
   * 向内核申请4Kib内存页
   * 用于保存线程结构体和内核栈
   * 
   */
  t = palloc_get_page(PAL_ZERO);
  if (t == NULL)
    return TID_ERROR;

  /* Initialize thread. */
  init_thread(t, name, priority);
  tid = t->tid = allocate_tid();
  rw_lock_init(&(t->lock));
  t->donee = NULL;
  t->donated_for = NULL;
  t->b_pri = priority;
  t->e_pri = priority;

  /* 
   * Stack frame for kernel_thread(). 
   * 
   * 手动初始化kernel_thread()的函数调用帧
   * 这个函数的作用即是调用thread_func* function
   * 因此可以被认为是kernel_thread()函数的桩
   * 
   * */
  kf = alloc_frame(t, sizeof *kf);
  kf->eip = NULL;
  kf->function = function;
  kf->aux = aux;

  /* Stack frame for switch_entry(). */
  ef = alloc_frame(t, sizeof *ef);
  ef->eip = (void (*)(void))kernel_thread;

  /* Stack frame for switch_threads(). */
  sf = alloc_frame(t, sizeof *sf);
  sf->eip = switch_entry;
  sf->ebp = 0;

  /* 
   * Add to run queue. 
   * 
   * 初始化内核线程初始调用栈完毕
   * 
   * 
   * */
  thread_unblock(t);
// TODO
  if(thread_get_priority() < priority) {
    thread_yield();
  }

  return tid;
}

/* Puts the current thread to sleep.  It will not be scheduled
   again until awoken by thread_unblock().

   同步原语将TCB归入等待队列的时候会使用这个函数

   This function must be called with interrupts turned off.  It
   is usually a better idea to use one of the synchronization
   primitives in synch.h. 
   
   之所以不会被再次调度，是由于调度器每次调度的时候都会将列表中的第一个线程
   pop出来，schedule又不会将此线程加入等待队列，因此只需要将此线程的状态设置为
   THREAD_BLOCK。而只有等到下一次使用thread_unblock将TCB加回到等待队列之后
   执行thread_block的线程才会被再次调度执行

   */
void thread_block(void) {
  ASSERT(!intr_context());
  ASSERT(intr_get_level() == INTR_OFF);

  thread_current()->status = THREAD_BLOCKED;
  schedule();
}

/* Places a thread on the ready structure appropriate for the
   current active scheduling policy.
   
   This function must be called with interrupts turned off. */
static void thread_enqueue(struct thread* t) {
  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(is_thread(t));

  if (active_sched_policy == SCHED_FIFO)
    list_push_back(&ready_list, &t->elem);
  else if (active_sched_policy == SCHED_PRIO)
    list_insert_ordered(&ready_list, &t->elem, &thread_before, &grater_pri);
  else
    PANIC("Unimplemented scheduling policy value: %d", active_sched_policy);
}

/* Transitions a blocked thread T to the ready-to-run state.
   This is an error if T is not blocked.  (Use thread_yield() to
   make the running thread ready.)

   一般是某些同步原语弹出优先级队列中TCB的时候会使用这个函数
   此函数不会调用schedule()，而只会做以下两件事情：
   1.将t归入等待队列
   2.将t的状态设置为THREAD_READY

   This function does not preempt the running thread.  This can
   be important: if the caller had disabled interrupts itself,
   it may expect that it can atomically unblock a thread and
   update other data. */
void thread_unblock(struct thread* t) {
  enum intr_level old_level;

  ASSERT(is_thread(t));

  old_level = intr_disable();
  ASSERT(t->status == THREAD_BLOCKED);
  thread_enqueue(t);
  t->status = THREAD_READY;
  intr_set_level(old_level);
}

/* Returns the name of the running thread. */
const char* thread_name(void) { return thread_current()->name; }

/* Returns the running thread.
   This is running_thread() plus a couple of sanity checks.
   See the big comment at the top of thread.h for details. 
   
   使用当前线程的esp获取线程的TCB（TCB一定位于线程内存页的底部）
   此函数仅仅是running_thread的包装而已 */
struct thread* thread_current(void) {
  struct thread* t = running_thread();

  /* Make sure T is really a thread.
     If either of these assertions fire, then your thread may
     have overflowed its stack.  Each thread has less than 4 kB
     of stack, so a few big automatic arrays or moderate
     recursion can cause stack overflow. */
  ASSERT(is_thread(t));
  ASSERT(t->status == THREAD_RUNNING);

  return t;
}

/* Returns the running thread's tid. */
tid_t thread_tid(void) { return thread_current()->tid; }

/* Deschedules the current thread and destroys it.  Never
   returns to the caller.
   
   1.将当前线程的TCB从allelem中移除
   2.将当前线程TCB标记为THREAD_DYING */
void thread_exit(void) {
  ASSERT(!intr_context());

  /* Remove thread from all threads list, set our status to dying,
     and schedule another process.  That process will destroy us
     when it calls thread_switch_tail(). */
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_DYING;
  schedule();
  NOT_REACHED();
}

/* Yields the CPU.  The current thread is not put to sleep and
   may be scheduled again immediately at the scheduler's whim. 
   1.将当前线程状态设置为THREAD_READY并入列
   2.禁用中断，执行schedule */
void thread_yield(void) {
  struct thread* cur = thread_current();
  enum intr_level old_level;

  ASSERT(!intr_context());

  old_level = intr_disable();
  if (cur != idle_thread)
    thread_enqueue(cur);
  cur->status = THREAD_READY;
  schedule();
  intr_set_level(old_level);
}

/* Invoke function 'func' on all threads, passing along 'aux'.
   This function must be called with interrupts off. */
void thread_foreach(thread_action_func* func, void* aux) {
  struct list_elem* e;

  ASSERT(intr_get_level() == INTR_OFF);

  for (e = list_begin(&all_list); e != list_end(&all_list); e = list_next(e)) {
    struct thread* t = list_entry(e, struct thread, allelem);
    func(t, aux);
  }
}

/* Sets the current thread's priority to NEW_PRIORITY. */
void thread_set_priority(int new_priority) { thread_current()->e_pri = new_priority; }

/* Returns the current thread's priority. */
int thread_get_priority(void) { return thread_current()->e_pri; }

/* Sets the current thread's nice value to NICE. */
void thread_set_nice(int nice UNUSED) { /* Not yet implemented. */
}

/* Returns the current thread's nice value. */
int thread_get_nice(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the system load average. */
int thread_get_load_avg(void) {
  /* Not yet implemented. */
  return 0;
}

/* Returns 100 times the current thread's recent_cpu value. */
int thread_get_recent_cpu(void) {
  /* Not yet implemented. */
  return 0;
}

/* Idle thread.  Executes when no other thread is ready to run.

   The idle thread is initially put on the ready list by
   thread_start().  It will be scheduled once initially, at which
   point it initializes idle_thread, "up"s the semaphore passed
   to it to enable thread_start() to continue, and immediately
   blocks.  After that, the idle thread never appears in the
   ready list.  It is returned by next_thread_to_run() as a
   special case when the ready list is empty. */
static void idle(void* idle_started_ UNUSED) {
  struct semaphore* idle_started = idle_started_;
  idle_thread = thread_current();
  sema_up(idle_started);

  for (;;) {
    /* Let someone else run. */
    intr_disable();
    thread_block();

    /* Re-enable interrupts and wait for the next one.

         The `sti' instruction disables interrupts until the
         completion of the next instruction, so these two
         instructions are executed atomically.  This atomicity is
         important; otherwise, an interrupt could be handled
         between re-enabling interrupts and waiting for the next
         one to occur, wasting as much as one clock tick worth of
         time.

         See [IA32-v2a] "HLT", [IA32-v2b] "STI", and [IA32-v3a]
         7.11.1 "HLT Instruction". */
    asm volatile("sti; hlt" : : : "memory");
  }
}

/** 
 * Function used as the basis for a kernel thread. 
 * 
 * 作为function的桩，这个函数有三个作用：
 * 1.启用中断
 * 2.运行function（线程初始调度运行的时候就会执行此函数）
 * 3.function返回之后调用thread_exit清除资源
 * */
static void kernel_thread(thread_func* function, void* aux) {
  ASSERT(function != NULL);

  intr_enable(); /* The scheduler runs with interrupts off. */
  function(aux); /* Execute the thread function. */
  thread_exit(); /* If function() returns, kill the thread. */
}

/* Returns the running thread. 

   利用TCB必然分布在内存页底部的特性获取线程的TCB */
struct thread* running_thread(void) {
  uint32_t* esp;

  /* Copy the CPU's stack pointer into `esp', and then round that
     down to the start of a page.  Because `struct thread' is
     always at the beginning of a page and the stack pointer is
     somewhere in the middle, this locates the curent thread. */
  asm("mov %%esp, %0" : "=g"(esp));
  return pg_round_down(esp);
}

/* Returns true if T appears to point to a valid thread. */
static bool is_thread(struct thread* t) { return t != NULL && t->magic == THREAD_MAGIC; }

/* Does basic initialization of T as a blocked thread named
   NAME. 

   初始化线程的4KB内存页
   */
static void init_thread(struct thread* t, const char* name, int priority) {
  enum intr_level old_level;

  ASSERT(t != NULL);
  ASSERT(PRI_MIN <= priority && priority <= PRI_MAX);
  ASSERT(name != NULL);

  memset(t, 0, sizeof *t);
  t->status = THREAD_BLOCKED;
  strlcpy(t->name, name, sizeof t->name);
  t->stack = (uint8_t*)t + PGSIZE; /* 将栈指针移动到页的顶部（Top of Pages） */
  t->e_pri = priority;
  t->pcb = NULL;
  t->magic = THREAD_MAGIC;

  /* 
   * 对all_list的操作需要 禁用中断
   * 而且all_list中的线程是尚未结束的线程
   * 无论是RUNNING还是WAITING都在里边
   */
  old_level = intr_disable();
  list_push_back(&all_list, &t->allelem);
  intr_set_level(old_level);
}

/* 
 * Allocates a SIZE-byte frame at the top of thread T's stack and
 * returns a pointer to the frame's base.
 *  
 * 这个函数的作用十分简单
 * 给定线程TCB，将其向下移动size个byte而已
 * 仅仅是移动内核线程的栈指针而已
 * 然后返回新的栈指针
 * 
 * */
static void* alloc_frame(struct thread* t, size_t size) {
  /* Stack data is always allocated in word-size units. */
  ASSERT(is_thread(t));
  ASSERT(size % sizeof(uint32_t) == 0);

  t->stack -= size;
  return t->stack;
}

/** 
 * First-in first-out scheduler 
 * 
 * 这里简单说明一下FIFO调度器的行为
 * 每此执行此函数，Ready Queue中的第一个线程就会被弹出
 * 随后schedule就会将此线程调度执行
 * 问题来了，之前的线程如果没有执行完毕，那么如果不将它压回到Ready Queue
 * 那么它岂不是永远不会被再次调度运行？
 * 实际上，对于硬件中断来说，计时器中断调用的对象是thread_yield
 * 而thread_yield会在执行schedule之前将TCB放回到等待队列的末尾
 * */
static struct thread* thread_schedule_fifo(void) {
  if (!list_empty(&ready_list))
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* TODO Strict priority scheduler */
static struct thread* thread_schedule_prio(void) {
  if (!list_empty(&ready_list))
    return list_entry(list_pop_front(&ready_list), struct thread, elem);
  else
    return idle_thread;
}

/* Fair priority scheduler */
static struct thread* thread_schedule_fair(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=fair\"");
}

/* Multi-level feedback queue scheduler */
static struct thread* thread_schedule_mlfqs(void) {
  PANIC("Unimplemented scheduler policy: \"-sched=mlfqs\"");
}

/* Not an actual scheduling policy — placeholder for empty
 * slots in the scheduler jump table. */
static struct thread* thread_schedule_reserved(void) {
  PANIC("Invalid scheduler policy value: %d", active_sched_policy);
}

/* Chooses and returns the next thread to be scheduled.  Should
   return a thread from the run queue, unless the run queue is
   empty.  (If the running thread can continue running, then it
   will be in the run queue.)  If the run queue is empty, return
   idle_thread. */
static struct thread* next_thread_to_run(void) {
  return (scheduler_jump_table[active_sched_policy])();
}

/* Completes a thread switch by activating the new thread's page
   tables, and, if the previous thread is dying, destroying it.

   1.重置计时器
   2.将当前线程标记为THREAD_RUNNING
   3.如果当前线程是用户线程，查看是否需要切换内存空间

   At this function's invocation, we just switched from thread
   PREV, the new thread is already running, and interrupts are
   still disabled.  This function is normally invoked by
   thread_switch() as its final action before returning, but
   the first time a thread is scheduled it is called by
   switch_entry() (see switch.S).

   It's not safe to call printf() until the thread switch is
   complete.  In practice that means that printf()s should be
   added at the end of the function.

   After this function and its caller returns, the thread switch
   is complete. */
void thread_switch_tail(struct thread* prev) {
  struct thread* cur = running_thread();

  ASSERT(intr_get_level() == INTR_OFF);

  /* Mark us as running. */
  cur->status = THREAD_RUNNING;

  /* Start new time slice. */
  thread_ticks = 0;

#ifdef USERPROG
  /* Activate the new address space. */
  process_activate();
#endif

  /* If the thread we switched from is dying, destroy its struct
     thread.  This must happen late so that thread_exit() doesn't
     pull out the rug under itself.  (We don't free
     initial_thread because its memory was not obtained via
     palloc().) */
  if (prev != NULL && prev->status == THREAD_DYING && prev != initial_thread) {
    ASSERT(prev != cur);
    palloc_free_page(prev);
  }
}

/* Schedules a new thread.  At entry, interrupts must be off and
   the running process's state must have been changed from
   running to some other state.  This function finds another
   thread to run and switches to it.

   It's not safe to call printf() until thread_switch_tail()
   has completed. 

   线程初次调度时的执行序列：
   thread_create: 作用是手动初始化各函数的调用帧，将这些函数的调用栈压栈
   switch_threads: 保存旧线程状态，加载新线程状态
   switch_entry: 维护一下当前的栈，方便调用thread_switch_tail
   thread_switch_tail: 清除旧线程
   kernel_thread: 调用线程启动函数

   线程非初次调度时的执行序列（计时器中断）：
   intr_stub: 硬件调用，保存状态
   intr_handler: 检测到是外部中断，禁用中断
      timer_interrupt: 递增 Number of timer ticks since OS booted
      thread_tick: 递增此线程tick
      intr_yield_on_return: 设置flag
   intr_handler: 检测到flag==true，退出外部中断上下文，执行thread_yield
      thread_yield: 禁用中断
          schedule: 调度器，执行调度算法，需要时执行switch_threads
              switch_threads: 保存旧线程状态，加载新线程状态（切换线程）
              thread_switch_tail: 清除旧线程，完成线程切换
      thread_yield: 启用中断
   
   */
static void schedule(void) {
  /* 返回当前线程的TCB */
  struct thread* cur = running_thread();
  /* 执行调度算法，返回下一时间片需要运行的线程的TCB */
  struct thread* next = next_thread_to_run();
  struct thread* prev = NULL;

  ASSERT(intr_get_level() == INTR_OFF);
  ASSERT(cur->status != THREAD_RUNNING);
  ASSERT(is_thread(next));

  /* 如果调度器运行结果（next）表示需要执行线程切换
     那么就调用switch_threads执行线程切换
     注意，switch_threads执行前后线程的身份是不一样的 */
  if (cur != next)
    prev = switch_threads(cur, next);
  thread_switch_tail(prev);
}

/* Returns a tid to use for a new thread. */
static tid_t allocate_tid(void) {
  static tid_t next_tid = 1;
  tid_t tid;

  lock_acquire(&tid_lock);
  tid = next_tid++;
  lock_release(&tid_lock);

  return tid;
}

bool thread_before(const struct list_elem* elem_a, const struct list_elem* elem_b,
                         void* aux) {
  bool (*grater_pri)(struct thread*, struct thread*) = aux;
  struct thread* thread_a = list_entry(elem_a, struct thread, elem);
  struct thread* thread_b = list_entry(elem_b, struct thread, elem);
  return grater_pri(thread_a, thread_b);
}

/* 比较实际优先级，如果a更大返回true */
bool grater_pri(struct thread* thread_a, struct thread* thread_b) {
  return thread_a->e_pri > thread_b->e_pri;
}

/* Offset of `stack' member within `struct thread'.
   Used by switch.S, which can't figure it out on its own. 

   这东西估计是用来算struct thread中stack这个变量离
   结构体地址的偏移量的
*/
uint32_t thread_stack_ofs = offsetof(struct thread, stack);
