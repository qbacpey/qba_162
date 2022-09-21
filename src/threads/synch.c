/* This file is derived from source code for the Nachos
   instructional operating system.  The Nachos copyright notice
   is reproduced in full below. */

/* Copyright (c) 1992-1996 The Regents of the University of California.
   All rights reserved.

   Permission to use, copy, modify, and distribute this software
   and its documentation for any purpose, without fee, and
   without written agreement is hereby granted, provided that the
   above copyright notice and the following two paragraphs appear
   in all copies of this software.

   IN NO EVENT SHALL THE UNIVERSITY OF CALIFORNIA BE LIABLE TO
   ANY PARTY FOR DIRECT, INDIRECT, SPECIAL, INCIDENTAL, OR
   CONSEQUENTIAL DAMAGES ARISING OUT OF THE USE OF THIS SOFTWARE
   AND ITS DOCUMENTATION, EVEN IF THE UNIVERSITY OF CALIFORNIA
   HAS BEEN ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   THE UNIVERSITY OF CALIFORNIA SPECIFICALLY DISCLAIMS ANY
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
   WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
   PURPOSE.  THE SOFTWARE PROVIDED HEREUNDER IS ON AN "AS IS"
   BASIS, AND THE UNIVERSITY OF CALIFORNIA HAS NO OBLIGATION TO
   PROVIDE MAINTENANCE, SUPPORT, UPDATES, ENHANCEMENTS, OR
   MODIFICATIONS.
*/

#include "threads/synch.h"
#include <stdio.h>
#include <string.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"

static void donate_pri_acquire(struct thread* self, struct lock* lock);
static bool lock_before(const struct list_elem* , const struct list_elem* , void* aux);

/* Initializes semaphore SEMA to VALUE.  A semaphore is a
   nonnegative integer along with two atomic operators for
   manipulating it:

   - down or "P": wait for the value to become positive, then
     decrement it.

   - up or "V": increment the value (and wake up one waiting
     thread, if any). */
void sema_init(struct semaphore* sema, unsigned value) {
  ASSERT(sema != NULL);

  sema->value = value;
  list_init(&sema->waiters);
}

/* Down or "P" operation on a semaphore.  Waits for SEMA's value
   to become positive and then atomically decrements it.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but if it sleeps then the next scheduled
   thread will probably turn interrupts back on. */
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

/* Down or "P" operation on a semaphore, but only if the
   semaphore is not already 0.  Returns true if the semaphore is
   decremented, false otherwise.

   This function may be called from an interrupt handler. */
bool sema_try_down(struct semaphore* sema) {
  bool success;

  ASSERT(sema != NULL);
  enum intr_level old_level = intr_disable();
  if (sema->value > 0) {
    sema->value--;
    success = true;
  } else
    success = false;
  intr_set_level(old_level);
  return success;
}

/* Up or "V" operation on a semaphore.  Increments SEMA's value
   and wakes up one thread of those waiting for SEMA, if any.

   This function may be called from an interrupt handler. */
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

static void sema_test_helper(void* sema_);

/* Self-test for semaphores that makes control "ping-pong"
   between a pair of threads.  Insert calls to printf() to see
   what's going on. */
void sema_self_test(void) {
  struct semaphore sema[2];
  int i;

  printf("Testing semaphores...");
  sema_init(&sema[0], 0);
  sema_init(&sema[1], 0);
  thread_create("sema-test", PRI_DEFAULT, sema_test_helper, &sema);
  for (i = 0; i < 10; i++) {
    sema_up(&sema[0]);
    sema_down(&sema[1]);
  }
  printf("done.\n");
}

/* Thread function used by sema_self_test(). */
static void sema_test_helper(void* sema_) {
  struct semaphore* sema = sema_;
  int i;

  for (i = 0; i < 10; i++) {
    sema_down(&sema[0]);
    sema_up(&sema[1]);
  }
}

/* Initializes LOCK.  A lock can be held by at most a single
   thread at any given time.  Our locks are not "recursive", that
   is, it is an error for the thread currently holding a lock to
   try to acquire that lock.

   A lock is a specialization of a semaphore with an initial
   value of 1.  The difference between a lock and such a
   semaphore is twofold.  First, a semaphore can have a value
   greater than 1, but a lock can only be owned by a single
   thread at a time.  Second, a semaphore does not have an owner,
   meaning that one thread can "down" the semaphore and then
   another one "up" it, but with a lock the same thread must both
   acquire and release it.  When these restrictions prove
   onerous, it's a good sign that a semaphore should be used,
   instead of a lock. */
void lock_init(struct lock* lock) {
  ASSERT(lock != NULL);

  lock->holder = NULL;
  lock->state = 1;
  lock->pri = 0;
  sema_init(&lock->semaphore, 1);
}

/* Acquires LOCK, sleeping until it becomes available if
   necessary.  The lock must not already be held by the current
   thread.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void lock_acquire(struct lock* lock) {
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(!lock_held_by_current_thread(lock));

  struct thread* t = thread_current();
  ASSERT(t!=NULL);

  enum intr_level old_level = intr_disable();
  if (--lock->state < 0) {
    if (t->e_pri > lock->pri){
      // 无论自身优先级如何，必须先设置自己的捐献对象
      t->donated_for = lock;
      donate_pri_acquire(t, lock);
    }
    sema_down(&lock->semaphore);
  } else
    lock->semaphore.value = 0;

  // 不可在禁用中断的时候申请内存
  // malloc_type(record);
  // 成功获取锁 压入优先级恢复记录
  lock->pri = t->e_pri;
  lock->holder = t;
  t->donated_for = NULL;
  list_insert_ordered(&t->lock_queue, &lock->elem, &lock_before, NULL);
  intr_set_level(old_level);
}

/* Tries to acquires LOCK and returns true if successful or false
   on failure.  The lock must not already be held by the current
   thread.

   This function will not sleep, so it may be called within an
   interrupt handler. */
bool lock_try_acquire(struct lock* lock) {
  bool success;

  ASSERT(lock != NULL);
  ASSERT(!lock_held_by_current_thread(lock));
  struct thread* t = thread_current();

  enum intr_level old_level = intr_disable();
  success = sema_try_down(&lock->semaphore);
  if (success) {
    lock->state--;
    lock->pri = t->e_pri;
    lock->holder = t;
    list_insert_ordered(&t->lock_queue, &lock->elem, &lock_before, NULL);
  }
  intr_set_level(old_level);
  return success;
}

/* Releases LOCK, which must be owned by the current thread.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to release a lock within an interrupt
   handler. */
void lock_release(struct lock* lock) {
  ASSERT(lock != NULL);
  ASSERT(lock_held_by_current_thread(lock));

  struct thread* t = thread_current();

  DISABLE_INTR({
    lock->holder = NULL;
    lock->pri = 0;

    list_remove(&lock->elem);
    if (list_empty(&t->lock_queue))
      t->e_pri = t->b_pri;
    else
      t->e_pri = list_entry(list_begin(&t->lock_queue), struct lock, elem)->pri;

    if (++lock->state < 1)
      sema_up(&lock->semaphore);
    else
      lock->semaphore.value = 1;
  });
}

/* Returns true if the current thread holds LOCK, false
   otherwise.  (Note that testing whether some other thread holds
   a lock would be racy.) */
bool lock_held_by_current_thread(const struct lock* lock) {
  ASSERT(lock != NULL);

  return lock->holder == thread_current();
}

/* Initializes a readers-writers lock */
void rw_lock_init(struct rw_lock* rw_lock) {
  lock_init(&rw_lock->lock);
  cond_init(&rw_lock->read);
  cond_init(&rw_lock->write);
  rw_lock->AR = rw_lock->WR = rw_lock->AW = rw_lock->WW = 0;
}

/* Acquire a writer-centric readers-writers lock */
void rw_lock_acquire(struct rw_lock* rw_lock, bool reader) {
  // Must hold the guard lock the entire time
  lock_acquire(&rw_lock->lock);

  if (reader) {
    // Reader code: Block while there are waiting or active writers
    while ((rw_lock->AW + rw_lock->WW) > 0) {
      rw_lock->WR++;
      cond_wait(&rw_lock->read, &rw_lock->lock);
      rw_lock->WR--;
    }
    rw_lock->AR++;
  } else {
    // Writer code: Block while there are any active readers/writers in the system
    while ((rw_lock->AR + rw_lock->AW) > 0) {
      rw_lock->WW++;
      cond_wait(&rw_lock->write, &rw_lock->lock);
      rw_lock->WW--;
    }
    rw_lock->AW++;
  }

  // Release guard lock
  lock_release(&rw_lock->lock);
}

/* Release a writer-centric readers-writers lock */
void rw_lock_release(struct rw_lock* rw_lock, bool reader) {
  // Must hold the guard lock the entire time
  lock_acquire(&rw_lock->lock);

  if (reader) {
    // Reader code: Wake any waiting writers if we are the last reader
    rw_lock->AR--;
    if (rw_lock->AR == 0 && rw_lock->WW > 0)
      cond_signal(&rw_lock->write, &rw_lock->lock);
  } else {
    // Writer code: First try to wake a waiting writer, otherwise all waiting readers
    rw_lock->AW--;
    if (rw_lock->WW > 0)
      cond_signal(&rw_lock->write, &rw_lock->lock);
    else if (rw_lock->WR > 0)
      cond_broadcast(&rw_lock->read, &rw_lock->lock);
  }

  // Release guard lock
  lock_release(&rw_lock->lock);
}

/* One semaphore in a list. */
// struct semaphore_elem {
//   struct list_elem elem;      /* List element. */
//   struct semaphore semaphore; /* This semaphore. */
// };

/* Initializes condition variable COND.  A condition variable
   allows one piece of code to signal a condition and cooperating
   code to receive the signal and act upon it. */
void cond_init(struct condition* cond) {
  ASSERT(cond != NULL);

  list_init(&cond->waiters);
}

/* Atomically releases LOCK and waits for COND to be signaled by
   some other piece of code.  After COND is signaled, LOCK is
   reacquired before returning.  LOCK must be held before calling
   this function.

   The monitor implemented by this function is "Mesa" style, not
   "Hoare" style, that is, sending and receiving a signal are not
   an atomic operation.  Thus, typically the caller must recheck
   the condition after the wait completes and, if necessary, wait
   again.

   A given condition variable is associated with only a single
   lock, but one lock may be associated with any number of
   condition variables.  That is, there is a one-to-many mapping
   from locks to condition variables.

   This function may sleep, so it must not be called within an
   interrupt handler.  This function may be called with
   interrupts disabled, but interrupts will be turned back on if
   we need to sleep. */
void cond_wait(struct condition* cond, struct lock* lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));

  list_insert_ordered(&cond->waiters, &thread_current()->elem, &thread_before, &grater_thread_pri);
  lock_release(lock);
  DISABLE_INTR({
    thread_current()->queue = &cond->waiters;
    thread_block();
  });

  lock_acquire(lock);
}

/* If any threads are waiting on COND (protected by LOCK), then
   this function signals one of them to wake up from its wait.
   LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_signal(struct condition* cond, struct lock* lock UNUSED) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  ASSERT(!intr_context());
  ASSERT(lock_held_by_current_thread(lock));
  struct thread* t = NULL;
  if (!list_empty(&cond->waiters)) {
    list_sort(&cond->waiters, &thread_before, &grater_thread_pri);
    t = list_entry(list_pop_front(&cond->waiters), struct thread, elem);
    thread_unblock(t);
  }
  if (t != NULL && !intr_context() && t->e_pri > thread_get_priority())
    thread_yield();
}

/* Wakes up all threads, if any, waiting on COND (protected by
   LOCK).  LOCK must be held before calling this function.

   An interrupt handler cannot acquire a lock, so it does not
   make sense to try to signal a condition variable within an
   interrupt handler. */
void cond_broadcast(struct condition* cond, struct lock* lock) {
  ASSERT(cond != NULL);
  ASSERT(lock != NULL);
  struct thread* t = NULL;
  bool flag = false;
  while (!list_empty(&cond->waiters)) {
    t = list_entry(list_pop_front(&cond->waiters), struct thread, elem);
    thread_unblock(t);
    if (t->e_pri > thread_get_priority()) {
      flag = true;
    }
  }
  if (t != NULL && flag)
    thread_yield();
}

/* 执行优先级捐献逻辑，执行时需要禁用中断
    值得一提的是，此实现有一个缺陷
    如果某低优先级线程先后获取了两个高优先级线程所需资源
    并且靠后线程的优先级更高的话，
    此线程不能恢复到第一个高优先级捐给他的实际优先级
    而是会在释放第二个线程资源的时候让实际优先级恢复到b_pri

    解决方法就是让执行优先级捐献的线程捐献优先级的时候
    记住捐献链末尾线程的实际优先级
    获取锁的时候用这个优先级将他恢复

    如果发生了优先级捐献，需要返回true

 */
static void donate_pri_acquire(struct thread* self, struct lock* lock) {
  struct thread* donee = lock->holder;
  struct thread* donor = self;
  ASSERT(donee != NULL);
  ASSERT(donor != NULL);
  struct lock* record = NULL;
  while (donor->e_pri > donee->e_pri) {
    // 只有是针对不同资源的优先级捐献才触发捐献历史保存
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

static bool lock_before(const struct list_elem* elem_a, const struct list_elem* elem_b, void* aux) {
  struct lock* lock_a = list_entry(elem_a, struct lock, elem);
  struct lock* lock_b = list_entry(elem_b, struct lock, elem);
  return lock_a->pri >= lock_b->pri;
}