#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/synch.h"
#include "threads/fixed-point.h"

#ifndef USER_SYNC_TYPE
#define USER_SYNC_TYPE
/* Synchronization Types */
typedef char lock_t;
typedef char sema_t;
#endif

/* States in a thread's life cycle. */
enum thread_status {
  THREAD_RUNNING, /* Running thread. */
  THREAD_READY,   /* Not running but ready to run. */
  THREAD_BLOCKED, /* Waiting for an event to trigger. */
  THREAD_DYING,   /* About to be destroyed. */
  THREAD_ZOMBIE   /* 线程已结束，但尚未被join，需要回收TCB */
};

#define NONE_OR_MAIN(exiting) (exiting == EXITING_NONE || exiting == EXITING_MAIN)

/* Thread identifier type.
   You can redefine this to whatever type you like. */
typedef int tid_t;
#define TID_ERROR ((tid_t)-1) /* Error value for tid_t. */

/**
 * @brief PTR是一个指针
 * 使用malloc为PTR指针分配typeof(PRY)大小的一块内存
 * 
 */
#define malloc_type(PTR) PTR = (typeof (*PTR)(*))malloc(sizeof(typeof(*PTR)))

/* Thread priorities. */
#define PRI_MIN 0      /* Lowest priority. */
#define PRI_DEFAULT 31 /* Default priority. */
#define PRI_MAX 63     /* Highest priority. */

/* A kernel thread or user process.

   Each thread structure is stored in its own 4 kB page.  The
   thread structure itself sits at the very bottom of the page
   (at offset 0).  The rest of the page is reserved for the
   thread's kernel stack, which grows downward from the top of
   the page (at offset 4 kB).  Here's an illustration:

        4 kB +---------------------------------+
             |          kernel stack           |
             |                |                |
             |                |                |
             |                V                |
             |         grows downward          |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             |                                 |
             +---------------------------------+
             |              magic              |
             |                :                |
             |                :                |
             |               name              |
             |              status             |
        0 kB +---------------------------------+

   The upshot of this is twofold:

      1. First, `struct thread' must not be allowed to grow too
         big.  If it does, then there will not be enough room for
         the kernel stack.  Our base `struct thread' is only a
         few bytes in size.  It probably should stay well under 1
         kB.

      2. Second, kernel stacks must not be allowed to grow too
         large.  If a stack overflows, it will corrupt the thread
         state.  Thus, kernel functions should not allocate large
         structures or arrays as non-static local variables.  Use
         dynamic allocation with malloc() or palloc_get_page()
         instead.
         总而言之就是，不要在内核栈中分配过大的数据结构，
         要么使用 malloc，要么使用 palloc_get_page

   The first symptom of either of these problems will probably be
   an assertion failure in thread_current(), which checks that
   the `magic' member of the running thread's `struct thread' is
   set to THREAD_MAGIC.  Stack overflow will normally change this
   value, triggering the assertion. 
   上面提到的 magic 其实就是一个哨兵值，thread_current 会检查这个哨兵值
   如果它被修改了（也就是不等于 THREAD_MAGIC）那么就会触发断言
   */

/* The `elem' member has a dual purpose.  It can be an element in
   the run queue (thread.c), or it can be an element in a
   semaphore wait list (synch.c).  It can be used these two ways
   only because they are mutually exclusive: only a thread in the
   ready state is on the run queue, whereas only a thread in the
   blocked state is on a semaphore wait list. */
struct thread {
  /* Owned by thread.c. */
  // 到时候在这里加一个指针算了

  tid_t tid;                 /* Thread identifier. */
  enum thread_status status; /* Thread state. */
  char name[16];             /* Name (for debugging purposes). */
  uint8_t* stack;            /* Saved stack pointer. */
  struct list_elem allelem;  /* List element for all threads list. */

  int64_t wake_up; // 需要在什么时候醒来

  /* Strict Priority Scheduler 相关 */
  struct lock* donated_for; // 线程最近一次接收优先级捐献由哪一个锁诱发？
  struct list lock_queue;   // 线程当前持有的锁的队列，按锁的优先级进行排列
  int8_t b_pri;             // 线程的基本优先级.
  int8_t e_pri;             // 线程的实际优先级

  /* Shared between thread.c / synch.c. / timer.c */
  struct list* queue;    /* 当前位于什么队列中（如果是timer的话值为NULL） */
  struct list_elem elem; /* List element. */

  bool in_handler;     /* 现在是否位于内核中？ */
#ifdef USERPROG
  /* Owned by process.c. */
#endif
  struct process* pcb;        /* Process control block if this thread is a userprog */
  size_t stack_no;            /* 线程的虚拟内存栈编号 */
  struct list_elem prog_elem; /* 进程线程列表元素 */
  struct thread* joined_by;   /* 指向join当前线程的TCB（通过禁用中断确保读写原子性） */

  /* Owned by thread.c. */
  unsigned magic; /* Detects stack overflow. */
  // 看起来 C 中结构体的定义方式是，最先定义的变量位于最低地址，所以哨兵值才放在最下面
};

/* Types of scheduler that the user can request the kernel
 * use to schedule threads at runtime. */
enum sched_policy {
  SCHED_FIFO,  // First-in, first-out scheduler
  SCHED_PRIO,  // Strict-priority scheduler with round-robin tiebreaking
  SCHED_FAIR,  // Implementation-defined fair scheduler
  SCHED_MLFQS, // Multi-level Feedback Queue Scheduler
};
#define SCHED_DEFAULT SCHED_FIFO

/* Determines which scheduling policy the kernel should use.
 * Controller by the kernel command-line options 内核命令行选择调度策略
 *  "-sched-default", "-sched-fair", "-sched-mlfqs", "-sched-fifo"
 * Is equal to SCHED_FIFO by default. */
extern enum sched_policy active_sched_policy;

void thread_init(void);
void thread_start(void);

/* 计时器中断处理程序 */
void thread_tick(void);
void thread_print_stats(void);

typedef void thread_func(void* aux);
tid_t thread_create(const char* name, int priority, thread_func*, void*);

/* 同步原语将线程归入等待队列时调用，当前线程sleep，调用schedule() */
void thread_block(void);
void thread_zombie(struct thread*);
/* 同步原语将线程移出等待队列时调用，当前线程不会sleep */
void thread_unblock(struct thread*);

struct thread* thread_current(void);
tid_t thread_tid(void);
const char* thread_name(void);

void thread_exit(void) NO_RETURN;
/* 线程自动让出CPU，当前线程READY，调用schedule() */
void thread_yield(void);

/* Performs some operation on thread t, given auxiliary data AUX. */
typedef void thread_action_func(struct thread* t, void* aux);
void thread_foreach(thread_action_func*, void*);

int thread_get_priority(void);
void thread_set_priority(int);

int thread_get_nice(void);
void thread_set_nice(int);
int thread_get_recent_cpu(void);
int thread_get_load_avg(void);

bool thread_before(const struct list_elem*, const struct list_elem*, void* aux);
bool grater_thread_pri(struct thread*, struct thread*);
bool grater_equal_thread_pri(struct thread*, struct thread*);

#endif /* threads/thread.h */
