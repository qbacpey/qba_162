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

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid) {
  tid_t result = TID_ERROR;
  struct thread* tcb = thread_current();
  struct process* pcb = tcb->pcb;
  bool found = false;
  rw_lock_acquire(&pcb->threads_lock, RW_READER);
  struct thread* pos = NULL;
  list_for_each_entry(pos, &pcb->threads, prog_elem) {
    if (pos->tid == tid) {
      lock_acquire(&pos->join_lock);
      if (pos->joined_by == NULL) 
        found = true;
      lock_release(&pos->join_lock);
      break;
    }
  }
  rw_lock_release(&pcb->threads_lock, RW_READER);

  if (!found)
    return result;

  enum intr_level old_level;
  bool joinable = false;
  // 为防止死锁，不可获取自己的锁
  // lock_acquire(&tcb->join_lock);

  // 上溯join链
  struct thread* chain = pos;
  /* 离开循环时chain只有两种可能
   * 
   * 1.没有被join过的pos：THREAD_ZOMBIE/chain->joined_by == NULL
   * 2.现在没有在join任何线程的活跃线程
   * 
   */
  for (;; chain = chain->joining) {
    lock_acquire(&chain->join_lock);
    old_level = intr_disable();
    // 只有chain往前再无joining的时候才能断定chain是可以Join的对象
    if (chain->joining == NULL) {
      // 正在运行中的线程，也有可能已退出
      if (chain->status != THREAD_ZOMBIE) {
        if (chain == tcb)
          joinable = false;
        else
          joinable = true;
        break;
      } else {
        // 可能没有人Join过它，也可能ZOMBIE的Joiner已被唤醒，需回滚
        if (chain->joined_by == NULL) {
          joinable = true;
          break;
        } else {
          // 避免不必要的死锁，先释放
          intr_set_level(old_level);
          lock_release(&chain->join_lock);
          // pthread_exit执行序列确保ZOMBIE必然在唤醒joiner之后再变ZOMBIE
          // 同时其joining必然为NULL
          ASSERT(chain->joined_by->joining != chain);
          chain = chain->joined_by;
          continue;
        }
      }
    }
    intr_set_level(old_level);
    lock_release(&chain->join_lock);
  }
  /* 禁用中断且持有链顶线程锁，可确保链顶线程没有join任何东西 */

  // 但是pos可能在遍历时被捷足先登了
  // 自身如果被join倒是没有关系，毕竟自己的链顶线程是安全的
  if (!joinable || pos->joined_by != NULL) {
    // 在if语句中跳出循环，需要手动启用中断和释放锁
    intr_set_level(old_level);
    lock_release(&chain->join_lock);
    return result;
  }

  pos->joined_by = tcb;
  tcb->joining = pos;
  lock_release(&chain->join_lock);
  thread_block();

  rw_lock_acquire(&pcb->threads_lock, RW_WRITER);
  lock_acquire(&pos->join_lock);

  result = pos->tid;
  // 获取线程锁之后该线程必然退出
  ASSERT(pos->status == THREAD_ZOMBIE);
  // 移出线程队列
  list_remove(&pos->prog_elem);
  // lock_release(&pos->join_lock);
  rw_lock_release(&pcb->threads_lock, RW_WRITER);

  // 释放pos的用户栈
  void* stack = ((void*)PHYS_BASE - pos->stack_no * STACK_SIZE) - PGSIZE;
  palloc_free_page(pagedir_get_page(pcb->pagedir, stack));
  pagedir_clear_page(pcb->pagedir, stack);
  pagedir_activate(pcb->pagedir);
  // 释放pos的内核栈
  DISABLE_INTR({ palloc_free_page(pos); });
  return result;
}
