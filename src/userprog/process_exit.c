#include "bitmap.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/string.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/filesys_lock.h"
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "userprog/tss.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NO_INLINE void process_exit_tail(struct process *, struct thread *);
inline static void set_exit_code(struct process *, int32_t);
inline static void exit_helper(struct thread *, struct list *, bool);
inline static void exit_helper_remove_from_list(struct thread *);
static void pthread_exit(void);

/**
 * @brief 执行退出检查或直接退出
 *
 * @details 任何线程退出内核时都需要执行此函数
 * 如果进程正在退出，那么需要执行退出逻辑
 * 可能调用thread_exit以DYING退出
 * 也可能调用pthread_exit以ZOMBIE退出
 *
 * @param pcb
 * @param is_pthread_exit 用于表示线程是否想要退出
 */
void exit_if_exiting(struct process *pcb, bool is_pthread_exit) {
  /* 当前线程是否需要执行`thread_exit`退出？ */
  bool thread_exit_flag = false;
  /* 当前线程是否需要执行`pthread_exit`退出？ */
  bool pthread_exit_flag = is_pthread_exit;
  /* 是否需要释放PCB */
  bool free_pcb = false;
  struct thread *t = thread_current();

  /*
   * 根据当前退出事件确定接下来的行为，执行全程禁用中断：
   * 1. `process_exit_exception`执行完毕：外部中断已经将进程所有资源（除PCB）释放，自己是遗留线程
   * 2. `process_exit_normal`阻塞中，需执行`pthread_exit`退出；
   * 3. 主线程执行`pthread_main_exit`；
   * 4. 无任何退出事件，仅仅是系统调用执行完毕，离开内核；
   */
  enum intr_level old_level = intr_disable();
  pcb->in_kernel_threads--;
  t->in_handler = false;

  if (pcb->exiting == EXITING_EXCEPTION) {
    thread_exit_flag = true;
    if (pcb->in_kernel_threads == 0)
      /* 当自己是最后一个遗留线程的情况下，需要释放PCB */
      free_pcb = true;
  } else if (pcb->exiting == EXITING_NORMAL) {
    pthread_exit_flag = true;
    /* 进程中除了自己以外只剩下正在执行`process_exit_normal`的线程，将其唤醒 */
    if (pcb->in_kernel_threads == 1)
      thread_unblock(pcb->thread_exiting);
  } else if (pcb->exiting == EXITING_MAIN) {
    /* 只有自己也执行`pthread_exit`同时主线程也已执行`pthread_exit`才唤醒主线程 */
    if (is_pthread_exit && pcb->active_threads == 2)
      thread_unblock(pcb->main_thread);
  }
  intr_set_level(old_level);

  /* 如果自己是异常退出的遗留线程，不可调用`pthread_exit`执行通用的退出流程 */
  if (thread_exit_flag) {
    t->pcb = NULL;
    /* 若自己是遗留线程，还需释放PCB */
    if (free_pcb) {
      while (!list_empty(&t->lock_queue))
        list_pop_front(&t->lock_queue);
      free(pcb);
    }
    thread_exit();
  }
  /* 当前线程自己想退出，或者其他线程执行了`exit()`，执行`pthread_exit`通用退出流程 */
  else if (pthread_exit_flag) {
    pthread_exit();
  }

  process_activate();
}

/**
 * @brief 执行通常的退出流程
 * @note Free the current thread's resources. Most resources will
 * be freed on thread_exit(), so all we have to do is deallocate the
 * thread's userspace stack. Wake any waiters on this thread.
 *
 * The main thread should not use this function. See
 * pthread_exit_main() below.
 *
 * This function will be implemented in Project 2: Multithreading. For
 * now, it does nothing. */
static void pthread_exit(void) {
  ASSERT(!intr_context());
  struct thread *t = thread_current();

  intr_disable();
  wake_up_joiner(t);
  /* 释放pos的用户栈 */
  void *stack = ((void *)PHYS_BASE - t->stack_no * STACK_SIZE) - PGSIZE;
  bitmap_set(t->pcb->stacks, t->stack_no, false);
  palloc_free_page(pagedir_get_page(t->pcb->pagedir, stack));
  pagedir_clear_page(t->pcb->pagedir, stack);
  process_activate();

  t->pcb->active_threads--;
  thread_zombie(t);
  NOT_REACHED();
}

/**
 * @brief 如果此线程有Joiner的话，将他叫醒
 *
 * @pre 调用此函数时，必须禁用外部中断
 *
 * @param t
 */
void wake_up_joiner(struct thread *t) {
  ASSERT(intr_get_level() == INTR_OFF);
  struct thread *joiner = t->joined_by;
  if (joiner != NULL) {
    thread_unblock(joiner);
  }
}

/**
 * @brief 主线程执行`pthread_exit`时会进入此函数
 * @post 执行`process_exit_tail`时，确保自己是进程中唯一剩下的线程
 *
 * @note Only to be used when the main thread explicitly calls pthread_exit.
 *  The main thread should wait on all threads in the process to
 *  terminate properly, before exiting itself. When it exits itself, it
 *  must terminate the process in addition to all necessary duties in
 *  pthread_exit.
 *  This function will be implemented in Project 2: Multithreading. For
 *  now, it does nothing.
 *
 */
void pthread_exit_main(void) {
  struct thread *tcb = thread_current();
  struct process *pcb = tcb->pcb;
  ASSERT(is_main_thread(tcb, pcb));
  bool exiting = false;


  /* == 判断当前退出事件等级 == */
  enum intr_level old_level = intr_disable();
  set_exit_code(pcb, 0);
  if (pcb->exiting == EXITING_NONE) {
    /* 主线程退出时只会标记退出事件以及`thread_exit`，不会将自己从PCB线程队列中移除 */
    pcb->exiting = EXITING_MAIN;
    pcb->thread_exiting = tcb;
    wake_up_joiner(tcb);
    /* 阻塞直到自己是进程所有线程中的最后一个 */
    while (pcb->active_threads > 1)
      thread_block();
  } else {
    exiting = true;
  }
  intr_set_level(old_level);

  /* 如果现时退出态比现在的更高，直接退出 */
  if (exiting) {
    exit_if_exiting(pcb, true);
    NOT_REACHED();
  }

  /*
   * 可保证此函数再次被运行时，自己是进程中唯一剩下的线程
   */
  list_remove(&tcb->prog_elem);
  ASSERT(pcb->in_kernel_threads == 1);
  struct thread *pos = NULL;
  list_clean_each(pos, &pcb->threads, prog_elem, { palloc_free_page(pos); });

  process_exit_tail(pcb, tcb);
}

/**
 * @brief 线程直接执行系统调用`exit`、用户代码执行异常会到达此函数
 * @post 执行`process_exit_tail`时，确保自己是进程中唯一剩下的线程
 *
 * @note 仅仅是用户级代码不能继续执行而已，位于内核中的线程还是可以继续执行的：
 * When exit is called on a multithreaded program,
 * all currently active threads in the user program should
 * be immediately terminated: none of the user threads should
 * be able to execute any more user-level code.
 *
 * Each of the backing kernel threads should release
 * all its resources before terminating.
 *
 * @param exit_code
 */
void process_exit_normal(int exit_code) {
  /* If this thread does not have a PCB, don't worry */
  struct thread *tcb = thread_current();
  if (tcb->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }
  struct process *pcb = tcb->pcb;
  bool exiting = false;


  /*
   * 判断当前退出事件等级
   */
  enum intr_level old_level = intr_disable();
  wake_up_joiner(tcb);
  set_exit_code(pcb, exit_code);
  /* 如果自己是第一个执行`exit`系统调用的线程 */
  if (pcb->exiting == EXITING_NONE) {
    pcb->exiting = EXITING_NORMAL;
    /* 将自己记录为“负责执行`exit`函数的线程”，确保`process_exit_exception`能将此线程清除 */
    pcb->thread_exiting = tcb;
  }
  /* 主线程已经执行了`pthread_exit` */
  else if (pcb->exiting == EXITING_MAIN) {
    /* 主线程不可调用此函数 */
    ASSERT(!is_main_thread(tcb, pcb));
    /* 主线程正在Sleep 让它成为成为实质上的ZOMBIE */
    exit_helper(pcb->main_thread, &pcb->threads, false);
    pcb->in_kernel_threads--;
  }
  /* 其他线程执行时触发异常或已有其他线程先于当前线程执行`exit` */
  else {
    exiting = true;
  }
  intr_set_level(old_level);

  /* 如果现时退出态比现在的更高，直接退出 */
  if (exiting) {
    exit_if_exiting(pcb, true);
    NOT_REACHED();
  }

  /*
   * 第一次遍历：将所有并非位于内核态的线程标记为ZOMBIE，阻止它们继续被调度运行
   * 同时确保所有对它们调用`pthread_join`的线程都能妥善退出
   */

  /* 其实这里本来是通过禁用防止用户级代码继续执行的，但是考虑到没有能够提升用户级线程优先级的系统调用
     因此最后还是使用优先级提升实现这一点 */
  thread_set_priority(PRI_DEFAULT + 1);
  rw_lock_acquire(&pcb->threads_lock, RW_READER);

  /* 将自己的TCB移出进程的线程队列，防止自己将自己标记为僵尸 */
  list_remove(&tcb->prog_elem);

  struct thread *pos = NULL;
  list_for_each_entry(pos, &pcb->threads, prog_elem) {
    old_level = intr_disable();
    if (pos->in_handler == false) {
      pos->status = THREAD_ZOMBIE;
      exit_helper_remove_from_list(pos);
      list_remove(&pos->allelem);
      wake_up_joiner(pos);
    }
    intr_set_level(old_level);
  }
  pos = NULL;
  rw_lock_release(&pcb->threads_lock, RW_READER);

  /*
   * 第一次遍历完成，阻塞直到只剩下自己
   */
  old_level = intr_disable();
  if (pcb->in_kernel_threads > 1) {
    thread_block();
  }
  intr_set_level(old_level);

  /*
   * 第二次遍历：清理所有线程的内核栈
   */
  ASSERT(pcb->in_kernel_threads == 1);
  list_clean_each(pos, &pcb->threads, prog_elem, { palloc_free_page(pos); });
  process_exit_tail(pcb, tcb);
}

/**
 * @deprecated 此函数被`process_exit_exception替代，发生内核异常时也执行上述函数
 * @brief 线程位于内核中发生异常时会执行此函数（比如内核线程执行时出现Page Fault）
 * @post 执行`process_exit_tail`时，自己不一定是进程中唯一剩下的线程
 * @note 不同于`exit_normal`，此函数
 *
 * @param exit_code
 */
void process_exit_exception(int exit_code) {
  ASSERT(exit_code == -1);

  struct thread *tcb = thread_current();
  struct process *pcb = tcb->pcb;

  enum intr_level old_level = intr_disable();
  pcb->exiting = EXITING_EXCEPTION;
  set_exit_code(pcb, exit_code);
  // 由于之后的list_clean_each统一处理，因此不用递减

  /*
   * 根据现在的退出事件等级执行部不同的操作：
   * 1. 主线程已执行`pthread_exit`：将其标记为僵尸，统一处理；
   * 2. 其他线程已因执行`exit`而等待：将其压回线程队列，统一处理
   */
  if (pcb->exiting == EXITING_MAIN)
    exit_helper(pcb->main_thread, &pcb->threads, false);
  else if (pcb->exiting == EXITING_NORMAL)
    exit_helper(pcb->thread_exiting, &pcb->threads, true);

  /*
   * 释放所有线程的内核栈（除了自己）
   */
  list_remove(&tcb->prog_elem);
  struct thread *pos = NULL;
  list_clean_each(pos, &pcb->threads, prog_elem, {
    if (pos->in_handler == true)
      pcb->in_kernel_threads--;
    /* 将TCB从其他队列中移除，确保线程不再可能被调度运行 */
    exit_helper_remove_from_list(pos);
    /* TCB的地址为所在内存页底部，因此其地址即为内存页地址 */
    palloc_free_page(pos);
  });
  intr_set_level(old_level);

  process_exit_tail(pcb, tcb);
}

/* Free the current process's resources.
 *
 * 其实如果是用户线程退出的话，可以提升自己优先级
 * 从而防止其他派生的线程继续执行，但这种行为有依赖调度器
 * 实现的嫌疑，还是作为辅助手段吧
 *
 * 现列出需要额外释放的资源：
 * 1.进程文件描述符表
 *
 * 2.子进程表
 *
 * 3.线程派生出的子线程结构体好像也算在里边
 *
 * 4.锁（待定）
 *
 * 5.线程指针列表：除了需要释放struct thread本身之外（仿照switch_tail）
 *                还需要处理一下allelem（thread_exit）
 *                elem的问题还不好说，先不管他，毕竟thread_exit也没有退出操作
 *
 * 6.进程名称
 *
 *
 * 还有其他需要做的工作：
 * 1.将退出状态返回给父进程
 *
 * 回头想想，如果是中断处理程序调用process_exit，考虑到外部中断执行时
 * 必然处于中断环境中（intr_context()==true）因此就不可能会被抢先
 * 而且之所以不能在外部中断时让线程Sleep的原因主要是需要通知PIC
 * 要不然外部中断维持太久指不定会出现什么问题
 *
 * 那么，process_exit实际需要做的是在进入时判断当下是不是外部中断环境
 * 如果是，那么针对所有数据的访问都不需要使用同步原语进行保护，直接一把梭就是了
 * （除了进程间共享的数据需要禁用中断）
 * 如果不是，那么就还是老老实实的获取锁再修改比较好
 *
 */
static NO_INLINE void process_exit_tail(struct process *pcb, struct thread *tcb) {
  // 将自己的优先级设置为系统最大值
  // if (!intr_context()) {
  //   ASSERT(pcb->in_kernel_threads == 1);
  // }
  printf("%s: exit(%d)\n", pcb->process_name, pcb->exit_code);

  struct thread *cur = tcb;
  struct process *pcb_to_free = pcb;
  uint32_t *pd;

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

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  /* 需要确保子进程编辑自己的PCB之前先获取父进程的锁 */
  struct semaphore *editing = pcb_to_free->editing;
  sema_down(editing);
  free_parent_self(pcb_to_free->self, pcb_to_free->exit_code);
  sema_up(editing);

  /* 文件系统执行操作时发生page fault */
  if (pcb_to_free->filesys_sema != NULL) {
    sema_up(pcb_to_free->filesys_sema);
  }

  /* 关闭进程可执行文件 */
  file_close(pcb_to_free->exec);

  /*
   * 释放进程所有资源：
   * 1. 释放文件描述符表，必须使用 NULL 进行初始化；
   * 2. 弹出列表中的所有锁回复记录，以免free错误访问
   * 3. 释放进程锁列表
   * 4. 释放进程信号量列表
   * 5. 释放子进程表
   * 6. 释放进程地址空间栈Bit map
   */

  struct file_desc *file_pos = NULL;
  list_clean_each(file_pos, &(pcb_to_free->files_tab), elem, {
    file_close(file_pos->file);
    free(file_pos);
  });

  while (!list_empty(&cur->lock_queue))
    list_pop_front(&cur->lock_queue);

  if (!list_empty(&pcb_to_free->locks_tab)) {
    struct registered_lock *lock_pos = NULL;
    list_clean_each(lock_pos, &(pcb_to_free->locks_tab), elem, { free(lock_pos); });
  }

  if (!list_empty(&pcb_to_free->semas_tab)) {
    struct registered_sema *sema_pos = NULL;
    list_clean_each(sema_pos, &(pcb_to_free->semas_tab), elem, { free(sema_pos); });
  }

  struct child_process *child = NULL;
  struct semaphore *child_editing = NULL;
  list_clean_each(child, &(pcb_to_free->children), elem, {
    /* 这里的信号量实际上相当于引用计数，false=2，true=1 */
    if (sema_try_down(&(child->waiting))) {
      /* 子进程已经退出，直接释放即可 */
      free_child_self(child);
    } else {
      /* 子进程未退出，需要获取editing，再进一步进行操作 */
      child_editing = child->editing;
      sema_down(child_editing);
      free_child_self(child);
      sema_up(child_editing);
    }
  });

  bitmap_destroy(pcb_to_free->stacks);

  cur->pcb = NULL;
  /* 就剩下自己了 */
  if (pcb_to_free->in_kernel_threads == 1) {
    while (!list_empty(&cur->lock_queue))
      list_pop_front(&cur->lock_queue);
    free(pcb_to_free);
  } else {
    pcb_to_free->in_kernel_threads--;
  }
  // sema_up(&temporary);
  thread_exit();
}

/* 子进程退出时 由子进程清除父子共同资源 同时设置返回值*/
void free_parent_self(struct child_process *self, int exit_code) {
  if (self->exited) {
    /* 父进程已经退出 释放子进程表元素  */
    free(self->editing);
    free(self);
  } else {
    /* 父进程尚未退出 需要设置引用计数、返回值、waiting */
    self->exited = true;
    self->exited_code = exit_code;
    self->child = NULL;
    sema_up(&(self->waiting));
  }
}

/* 父进程退出时 由父进程清除父子共同资源 不会将此元素从链表中剥离*/
void free_child_self(struct child_process *child) {
  if (child->exited) {
    /* 子进程已经退出 释放子进程表元素  */
    free(child->editing);
    free(child);
  } else {
    /* 子进程尚未退出 需要设置引用计数 */
    child->exited = true;
    child->child->parent = NULL;
    sema_up(&(child->waiting));
  }
}

/**
 * @brief Set the exit code
 * 需要在中断禁用时调用
 *
 * @param pcb
 * @param exit_code
 */
inline static void set_exit_code(struct process *pcb, int32_t exit_code) {
  if (pcb->exit_code == 0 || exit_code != -1)
    pcb->exit_code = exit_code;
}

/**
 * @brief 令THREAD_BLOCK线程成为THREAD_ZOMBIE
 *  需要在中断环境下调用
 *
 * @param t
 * @param list 进程的线程队列
 * @param flag 要不要将这个线程压入线程队列
 */
inline static void exit_helper(struct thread *t, struct list *list, bool flag) {
  if (flag)
    list_push_front(list, &t->prog_elem);
  t->status = THREAD_ZOMBIE;
  list_remove(&t->allelem);
}

/**
 * @brief 如果POS处于THREAD_BLOCKED或THREAD_READY
 * 也就是某个线程队列中的话，将POS从其当前所属线程队列中移除
 *
 * @param pos
 */
inline static void exit_helper_remove_from_list(struct thread *pos) {
  if (pos->queue != NULL && (pos->status == THREAD_BLOCKED || pos->status == THREAD_READY))
    list_remove(&pos->elem);
}