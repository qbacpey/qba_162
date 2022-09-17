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


static void process_exit_tail(struct process*, struct thread*);
inline static void set_exit_code(struct process*, int32_t);

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {
  ASSERT(!intr_context());
  intr_disable();
  list_remove(&thread_current()->allelem);
  thread_current()->status = THREAD_ZOMBIE;
  wake_up_joiner(thread_current());
  thread_zombie();
  NOT_REACHED();
}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {
  struct thread* tcb = thread_current();
  struct process* pcb = tcb->pcb;
  bool exiting = false;
  wake_up_joiner(tcb);

  enum intr_level old_level = intr_disable();
  set_exit_code(pcb, 0);
  if (pcb->exiting == EXITING_NONE) {
    pcb->exiting = EXITING_MAIN;
    pcb->thread_exiting = tcb;
  } else {
    exiting = true;
  }
  intr_set_level(old_level);

  // 如果现时退出态比现在的更高，直接退出
  if(exiting){
    exit_if_exiting(pcb);
    NOT_REACHED();
  }
}

void process_exit_exception(int exit_code){
    PANIC("process_exit_exception 尚未实现");
}

/**
 * @brief 除了主线程之外的其他所有线程退出进程时
 * 都需要执行此函数
 * 
 * @param exit_code 
 */
void process_exit_normal(int exit_code) {
  /* If this thread does not have a PCB, don't worry */
  struct thread* tcb = thread_current();
  if (tcb->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }
  struct process* pcb = tcb->pcb;
  bool exiting = false;
  wake_up_joiner(tcb);

  enum intr_level old_level = intr_disable();
  set_exit_code(pcb, exit_code);
  if (NONE_OR_MAIN(pcb->exiting)) {
    pcb->exiting = EXITING_NORMAL;
    pcb->thread_exiting = tcb;
  } else {
    exiting = true;
  }
  intr_set_level(old_level);

  // 如果现时退出态比现在的更高，直接退出
  if(exiting){
    exit_if_exiting(pcb);
    NOT_REACHED();
  }

  // 第一次遍历 
  enum intr_level old_level = intr_disable();
  list_remove(&tcb->prog_elem);
  struct thread* pos = NULL;
  list_for_each_entry(pos, &pcb->threads, prog_elem) {
    if (pos->in_handler == false) {
      pos->status = THREAD_ZOMBIE;
      list_remove(&pos->elem);
      list_remove(&pos->allelem);
      wake_up_joiner(pos);
    }
  }

  pos = pcb->main_thread;
  if (pos->in_handler == false) {
    pos->status = THREAD_ZOMBIE;
    list_remove(&pos->elem);
    list_remove(&pos->allelem);
    wake_up_joiner(pos);
  }

  // 第一次遍历完成，阻塞直到只剩下自己
  thread_block();
  ASSERT(pcb->pending_thread == 1);

  // 开始第二次遍历
  list_clean_each(pos, &pcb->threads, prog_elem, {
    list_remove(&pos->elem);
    list_remove(&pos->allelem);
    palloc_free_page(pos);
  });

  pos = pcb->main_thread;
  list_remove(&pos->elem);
  list_remove(&pos->allelem);
  palloc_free_page(pos);
  intr_set_level(old_level);

  process_exit_tail(pcb, tcb);
}

/**
 * @brief 任何线程退出内核时都需要执行此函数
 * 如果进程正在退出，那么需要执行退出逻辑
 * 可能调用thread_exit以DYING退出
 * 也可能调用pthread_exit以ZOMBIE退出
 * 
 * @param pcb 
 */
void exit_if_exiting(struct process* pcb) {
  bool thread_exit_flag = false;
  bool pthread_exit_flag = false;
  bool free_pcb = false;

  enum intr_level old_level = intr_disable();
  pcb->pending_thread--;
  thread_current()->in_handler = false;

  if (pcb->exiting == EXITING_EXCEPTION) { /* 外部中断已经将进程所有资源释放 */
    thread_exit_flag = true;
    /* 可能外部中断有多个遗留线程，令`pending_thread==0`清除 */
    if (pcb->pending_thread == 0) 
        free_pcb = true;
  } else if (pcb->exiting == EXITING_NORMAL || pcb->exiting == EXITING_MAIN) { /* `process_exit`阻塞 */
    pthread_exit_flag = true;
    if (pcb->pending_thread == 1) {
      if (pcb->thread_exiting != NULL) /* 优先唤醒thread_exiting */
        thread_unblock(pcb->thread_exiting);
      else /* 唤醒pthread_main_exiting */
        thread_unblock(pcb->main_thread);
    }
  }
  intr_set_level(old_level);

  if (thread_exit_flag) {
    if (free_pcb)
      free(pcb);
    thread_exit();
  } else if (pthread_exit_flag) {
    pthread_exit();
  }
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
static void process_exit_tail(struct process* pcb, struct thread* tcb) {
  // 将自己的优先级设置为系统最大值
  thread_set_priority(PRI_MAX);

  struct thread* cur = tcb;
  struct process* pcb_to_free = pcb;
  uint32_t* pd;


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
  struct semaphore* editing = pcb_to_free->editing;
  sema_down(editing);
  free_parent_self(pcb_to_free->self, pcb_to_free->exit_code);
  sema_up(editing);

  if (pcb_to_free->filesys_sema != NULL) {
    sema_up(pcb_to_free->filesys_sema); /* 文件系统执行操作时发生page fault */
  }

  sema_down(filesys_sema);
  file_close(pcb_to_free->exec);
  sema_up(filesys_sema);

  /* 释放文件描述符表，必须使用 NULL 进行初始化 */
  struct file_desc* file_pos = NULL;
  list_clean_each(file_pos, &(pcb_to_free->files_tab), elem, {
    file_close(file_pos->file);
    free(file_pos);
  });

  // 释放进程锁列表
  struct registered_lock* lock_pos = NULL;
  list_clean_each(lock_pos, &(pcb_to_free->locks_tab), elem, { free(lock_pos); });

  // 释放进程信号量列表
  struct registered_lock* sema_pos = NULL;
  list_clean_each(sema_pos, &(pcb_to_free->semas_tab), elem, { free(sema_pos); });

  /* 释放子进程表 */
  struct child_process* child = NULL;
  struct semaphore* child_editing = NULL;
  list_clean_each(child, &(pcb_to_free->children), elem, {
    // 这里的信号量实际上相当于引用计数，false=2，true=1
    if (sema_try_down(&(child->waiting))) {
      // 子进程已经退出，直接释放即可
      free_child_self(child);
    } else {
      // 子进程未退出，需要获取editing，再进一步进行操作
      child_editing = child->editing;
      sema_down(child_editing);
      free_child_self(child);
      sema_up(child_editing);
    }
  });

  // TODO 线程资源释放
  bitmap_destroy(pcb_to_free->stacks);

  /* 资源全部释放 */
  cur->pcb = NULL;
  free(pcb_to_free);

  // sema_up(&temporary);
  thread_exit();
}

/* 子进程退出时 由子进程清除父子共同资源 同时设置返回值*/
void free_parent_self(struct child_process* self, int exit_code) {
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
void free_child_self(struct child_process* child) {
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
inline static void set_exit_code(struct process* pcb, int32_t exit_code) {
  if (pcb->exit_code == 0 || exit_code != -1)
    pcb->exit_code = exit_code;
}