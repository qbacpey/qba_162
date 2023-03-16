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

/**
 * @brief Waits for thread with TID to die, if that thread was spawned
 * in the same process and has not been waited on yet. Returns TID on
 * success and returns TID_ERROR on failure immediately, without
 * waiting.
 * @note 如果`tid`不是主线程，那么当此线程被唤醒之后，就会回收`tid`的内核栈
 * （用户栈由该线程自己回收）
 * 
 * @param tid 
 * @return tid_t 
 */
tid_t pthread_join(tid_t tid) {
  tid_t result = TID_ERROR;
  struct thread *tcb = thread_current();
  struct process *pcb = tcb->pcb;
  bool found = false;
  bool is_main = false;
  bool block_wait = false;
  rw_lock_acquire(&pcb->threads_lock, RW_READER);
  struct thread *pos = NULL;
  list_for_each_entry(pos, &pcb->threads, prog_elem) {
    if (pos->tid == tid) {
      DISABLE_INTR({
        if (pos->joined_by == NULL) {
          found = true;
          is_main = is_main_thread(pos, pcb);
          /* 如果目标线程处于`THREAD_ZOMBIE`或者`THREAD_DYING`状态的话，无需阻塞 */
          if(pos->status == THREAD_ZOMBIE || pos->status == THREAD_DYING)
            block_wait = false;
          else
            block_wait = true;
        }
      });
      break;
    }
  }
  rw_lock_release(&pcb->threads_lock, RW_READER);

  if (!found)
    return result;

  /* 必然出现Join死锁 */
  if (tcb == pos || tcb->joined_by == pos) {
    return result;
  }
  /* 可能出现Join死锁，但这是用户应该处理的事情了 */
  else {
    DISABLE_INTR({
      pos->joined_by = tcb;
      /* 如果退出事件为`EXITING_MAIN`，代表它已经执行了将自己的`joiner`唤醒，此时不可Join主线程 */
      if (block_wait && (pcb->exiting != EXITING_MAIN || !is_main)) {
        thread_block();
      }
    });
  }

  rw_lock_acquire(&pcb->threads_lock, RW_WRITER);
  result = pos->tid;
  /* 只有被Join线程不是主线程的时候，才可以释放该线程的资源 */
  if (!is_main) {
    ASSERT(pos->status == THREAD_ZOMBIE);
    /* 必须将此锁移除自己的锁队列，否则会导致Page Fault */
    list_remove(&pos->prog_elem);
  }
  rw_lock_release(&pcb->threads_lock, RW_WRITER);

  /* 释放pos的内核栈 */
  if (!is_main) {
    DISABLE_INTR({ palloc_free_page(pos); });
    pagedir_activate(pcb->pagedir);
  }
  return result;
}
