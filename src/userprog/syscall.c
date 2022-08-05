#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/malloc.h"
#include "userprog/process.h"

static void syscall_handler(struct intr_frame*);
static struct lock temporary; /* 临时文件系统锁 */
static struct lock* filesys_lock = NULL; /* 临时文件系统锁 */

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }
int syscall_practice(uint32_t* args);
bool syscall_create(uint32_t* args);
bool syscall_remove(uint32_t* args);
int syscall_open(uint32_t* args);
int syscall_write(uint32_t* args);
int syscall_filesize(uint32_t* args);
int syscall_read(uint32_t* args);
void syscall_seek(uint32_t* args);
void syscall_close(uint32_t* args);
unsigned syscall_tell(uint32_t* args);
static inline bool check_fd(uint32_t); /* 二参数系统调用检查 */
static inline bool check_buffer(uint32_t*, uint32_t); /* 三参数系统调用检查 */

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

  if(filesys_lock == NULL){
    lock_init(&temporary);
    filesys_lock = &temporary;
  }

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);
  bool beneath = false;

  switch (args[0]) {
    case SYS_EXIT:
      f->eax = args[1];
      printf("%s: exit(%d)\n", thread_current()->pcb->process_name, args[1]);
      process_exit(f->eax);
      break;

    case SYS_PRACTICE:
      f->eax = syscall_practice(args);
      break;

    case SYS_CREATE:
      beneath = check_buffer(args[1], strlen(args[1]));
      if(beneath){
        f->eax = syscall_create(args);
      }
      break;

    case SYS_REMOVE:
      beneath = check_buffer(args[1], strlen(args[1]));
      if (beneath) {
        f->eax = syscall_remove(args);
      }
      break;

    case SYS_OPEN:
      beneath = check_buffer(args[1], strlen(args[1]));
      if (beneath) {
        f->eax = syscall_open(args);
      }
      break;

    case SYS_FILESIZE:
      f->eax = syscall_write(args);
      break;

    case SYS_READ:
      f->eax = syscall_write(args);
      break;

    case SYS_WRITE:
      beneath = check_buffer(args[2], args[3]);
      if(beneath){
        f->eax = syscall_write(args);
      }
      break;

    case SYS_SEEK:
      f->eax = syscall_write(args);
      break;

    case SYS_TELL:
      f->eax = syscall_write(args);
      break;

    case SYS_CLOSE:
      f->eax = syscall_write(args);
      break;

    default:
      printf("Unknown system call number: %d\n", args[0]);
      process_exit(-1);
      break;
  }

  if (!beneath) {
    printf("System call %d failed for invalid address!\n", args[0]);
    process_exit(f->eax);
  }
}

int syscall_practice(uint32_t* args) { return (int)args[1] + 1; }

bool syscall_create(uint32_t* args) {
  bool result = false;
  lock_acquire(filesys_lock);
  result = filesys_create(args[1], args[2]);
  lock_release(filesys_lock);
  return result;
}

bool syscall_remove(uint32_t* args) {
  bool result = false;
  lock_acquire(filesys_lock);
  result = filesys_remove(args[1]);
  lock_release(filesys_lock);
  return result;
}

int syscall_open(uint32_t* args) {
  struct process* pcb = thread_current()->pcb;
  struct list* files_tab = &(pcb->files_tab);
  struct lock* files_tab_lock = &(pcb->files_lock);
  struct file* new_file = NULL;

  lock_acquire(filesys_lock);
  new_file = filesys_open(args[1]);
  lock_release(filesys_lock);

  if (new_file == NULL) {
    ASSERT(pcb->files_next_desc >= 3);
    struct file_desc* new_file_desc = malloc(sizeof(struct file_desc));
    new_file_desc->file = new_file;

    lock_acquire(files_tab_lock);
    new_file_desc->file_desc = pcb->files_next_desc;
    list_push_front(files_tab, &(new_file_desc->elem));
    pcb->files_next_desc++;
    lock_release(files_tab_lock);

    return new_file_desc->file_desc;
  } else {
    return -1;
  }
}

int syscall_filesize(uint32_t* args){

}

int syscall_write(uint32_t* args) {

  if (args[1] == 1) {
    putbuf((char*)args[2], (size_t)args[3]);
    return 0;
  }

  printf("%s: write(%d)\n", thread_current()->pcb->process_name, args[1]);
  return 0;
}

static inline bool check_fd(uint32_t fd) {
  struct process* pcb = thread_current()->pcb;
  if(pcb->files_next_desc >= fd){
    return -1;
  }
  list_for_each_entry(){
    
  }
  return (void*)args[1] > (void*)0xbffffffc ? false : true;
}
static inline bool check_buffer(uint32_t* buffer, uint32_t size) {
  void* addr = (void*)(buffer + size);
  return addr > (void*)0xbffffffc ? false : true;
}
