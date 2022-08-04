#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "userprog/process.h"

static void syscall_handler(struct intr_frame*);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }
int syscall_write(uint32_t* args);
int syscall_practice(uint32_t* args);
static inline bool check_beneath(void*);

/* 
 * userprog/syscall.c（也就是本文件）的作用实际上就一个：分发系统调用
 * 
 * 本质上来说，这是因为下边的这个函数syscall_handler就是系统调用的分派函数
 * 因此实际上所有的任务实际上可以被概括为一句话：让syscall_handler根据args
 * 调用合适的系统调用处理程序 
 * 
 * 但是syscall.h中的修改究竟起到什么作用呢？这一点究竟还是不甚明确的
 * 
 * 另外需要注意的是，遇到内核访问错误的时候，需要终止进程并且返回错误码
 * 
 *  */

static void syscall_handler(struct intr_frame* f UNUSED) {
  uint32_t* args = ((uint32_t*)f->esp);

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);

  switch (args[0])
  {
  case SYS_EXIT:
    f->eax = args[1];
    printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
    process_exit();
    break;

  case SYS_WRITE:
    f->eax = syscall_write(args);
    break;

  case SYS_PRACTICE:
    f->eax = syscall_practice(args);
    break;
  
  default:
    printf("Unknown system call number: %d\n", args[0]);
    process_exit();
    break;
  }
}

int syscall_practice(uint32_t* args) {
  return (int)args[1] + 1;
}

int syscall_write(uint32_t* args) {

  bool is_beneath = check_beneath((void *)(args[2] + args[3]));
  if(!is_beneath){
    return -1;
  }

  if(args[1] == 1){
    putbuf((char*)args[2], (size_t)args[3]);
    return 0;
  }

  printf("%s: write(%d)\n", thread_current()->pcb->process_name, args[1]);
  return 0;
}

static inline bool check_beneath(void* addr){
  return addr > (void *)0xbffffffc ? false : true;
}
