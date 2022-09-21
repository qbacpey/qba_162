#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lib/string.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/filesys_lock.h"
#include "bitmap.h"

/* pthread_create/start_thread 之间传递参数 */
struct init_tcb {
  stub_fun sf;
  pthread_fun tf;
  void* arg;
  struct process* pcb;
  struct semaphore* finished; /* 线程创建完成时UP这个信号量 */
  size_t stack_no;            /* 由pthread_execute分配的栈编号 */
};

static thread_func start_pthread NO_RETURN;
inline static size_t stack_no(uint8_t* stack);
bool setup_thread(void (**eip)(void), void** esp, struct init_tcb* init_tcb);

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   整个用户线程创建的函数调用过程可归结如下：
   1.创建者的执行序列：
      1.sys_pthread_create：系统调用
      2.pthread_execute：作用类似于process_execute
          1.将新线程的TCB放入PCB列表中；
          2.配置PCB中的各种字段；
          3.调用thread_create，参数是start_pthread；
   2.被创建者的执行序列：
      1.start_pthread：作用类似于start_process
        1.需要调用install_page申请用户栈空间；
        2.需要仿照start_process配置用户栈空间，调用_pthread_start_stub；
      2._pthread_start_stub：用户函数桩（此时已经位于用户内核）；

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf, pthread_fun tf, void* arg) {
  struct process* pcb = thread_current()->pcb;
  struct init_tcb* init_tcb = NULL;

  lock_acquire(&pcb->pcb_lock);
  size_t stack_no = bitmap_scan_and_flip(pcb->stacks, 0, 1, false);
  if (stack_no == BITMAP_ERROR) {
    lock_release(&pcb->pcb_lock);
    return TID_ERROR;
  }
  ASSERT(stack_no > 0);
  lock_release(&pcb->pcb_lock);

  malloc_type(init_tcb);
  init_tcb->sf = sf;
  init_tcb->tf = tf;
  init_tcb->arg = arg;
  init_tcb->pcb = pcb;
  init_tcb->stack_no = stack_no;

  struct semaphore finished;
  sema_init(&finished, 0);
  init_tcb->finished = &finished;
  tid_t tid = thread_create("sub thread", PRI_DEFAULT, &start_pthread, init_tcb);
  if (tid == TID_ERROR) {
    free(init_tcb);
    bitmap_scan_and_flip(pcb->stacks, stack_no, 1, false);
  }
  sema_down(&finished);
  return tid;
}

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_) {
  struct init_tcb* init_tcb = (struct init_tcb*)exec_;
  struct thread* t = thread_current();
  struct process* pcb = init_tcb->pcb;
  struct intr_frame if_;

  lock_acquire(&t->join_lock);
  t->pcb = pcb;
  t->stack_no = init_tcb->stack_no;
  // 因为start_pthread有可能在被Join之后才被运行，因此不能在此赋值
  // t->joined_by = NULL;
  // t->joining = NULL;
  lock_release(&t->join_lock);

  ASSERT(t->pcb != NULL);

  bool success = false;
  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = setup_thread(&if_.eip, &if_.esp, init_tcb);

  if (!success)
    goto done;

  // 禁用中断主要是为了防止被中断处理程序触发的exit(-1)影响
  rw_lock_acquire(&pcb->threads_lock, RW_WRITER);
  if (NONE_OR_MAIN(pcb->exiting))
    list_push_front(&pcb->threads, &t->prog_elem);
  rw_lock_release(&pcb->threads_lock, RW_WRITER);

/* 通过判断现在进程是不是在退出，决定是跳到用户空间还是继续执行
     因为使用了install_page将用户栈注册到进程页目录中，因此无需
     额外释放用户栈，process_exit执行时候会释放此线程用户栈 */
done:
  // 通知pthread_execute进程创建函数执行完毕
  sema_up(init_tcb->finished);
  free(init_tcb);
  exit_if_exiting(pcb, false);
  asm volatile("movl %0, %%esp ; jmp intr_exit" : : "g"(&if_) : "memory");
  NOT_REACHED();
}

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   这个函数只有在一种情况下能够失败：进程执行exit之后此线程再被调度执行
   至于真正的Page Fault问题和进程虚拟内存空间用完的问题，由thread_extute处理

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void), void** esp, struct init_tcb* init_tcb) {
  bool success = false;
  /* Set up stack. */
  if (!setup_stack(esp, PHYS_BASE - init_tcb->stack_no * STACK_SIZE))
    goto done;
  *eip = init_tcb->sf;
  pagedir_activate(thread_current()->pcb->pagedir);
  *esp -= 0x10;
  *((uint32_t*)(*esp) + 1) = init_tcb->arg;
  *(pthread_fun*)(*esp) = init_tcb->tf;
  *esp -= 0x4;

  success = true;
done:
  return success;
}
