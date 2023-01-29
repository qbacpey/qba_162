#include "userprog/syscall.h"
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "lib/float.h"
#include "lib/string.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/process.h"
#include <stdio.h>
#include <syscall-nr.h>

static void syscall_handler(struct intr_frame *);

void syscall_init(void) { intr_register_int(0x30, 3, INTR_ON, syscall_handler, "syscall"); }
static int syscall_practice(uint32_t *args, struct process *pcb);
static bool syscall_create(uint32_t *args, struct process *pcb);
static bool syscall_remove(uint32_t *args, struct process *pcb);
static pid_t syscall_exec(uint32_t *args, struct process *pcb);
static int syscall_wait(uint32_t *args, struct process *pcb);
static int syscall_open(uint32_t *args, struct process *pcb);
static int syscall_write(uint32_t *args, struct process *pcb);
static int syscall_filesize(uint32_t *args, struct process *pcb);
static int syscall_read(uint32_t *args, struct process *pcb);
static int syscall_seek(uint32_t *args, struct process *pcb);
static int syscall_close(uint32_t *args, struct process *pcb);
static int syscall_tell(uint32_t *args, struct process *pcb);
static double syscall_compute_e(uint32_t *args, struct process *pcb);
static inline bool check_fd(uint32_t, struct process *); /* fd系统调用检查，仅检查是否大于下一个文件标识符 */
static inline bool check_buffer(void *, uint32_t); /* 三参数系统调用检查 */
static inline bool check_boundary(void *);         /* 单参数系统调用检查 */

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

static void syscall_handler(struct intr_frame *f UNUSED) {
  uint32_t *args = ((uint32_t *)f->esp);
  struct process *pcb = thread_current()->pcb;

  /*
   * The following print statement, if uncommented, will print out the syscall
   * number whenever a process enters a system call. You might find it useful
   * when debugging. It will cause tests to fail, however, so you should not
   * include it in your final submission.
   */

  // printf("System call number: %d\n", args[0]);
  bool beneath = true;
  f->eax = -1;

  switch (args[0]) {

  case SYS_HALT:
    shutdown_power_off();
    break;

  case SYS_EXIT:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = args[1];
      printf("%s: exit(%d)\n", pcb->process_name, args[1]);
      process_exit(f->eax);
    }
    break;

  case SYS_EXEC:
    beneath = check_boundary(args + 1) && (void *)args[1] != NULL &&
              check_buffer((void *)args[1], strlen((char *)args[1]));
    if (beneath) {
      f->eax = syscall_exec(args, pcb);
    }
    break;

  case SYS_WAIT:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = syscall_wait(args, pcb);
    }
    break;

  case SYS_CREATE:
    beneath = check_boundary(args + 2) && (void *)args[1] != NULL &&
              check_buffer((void *)args[1], strlen((char *)args[1]));
    if (beneath) {
      f->eax = syscall_create(args, pcb);
    }
    break;

  case SYS_REMOVE:
    beneath = check_boundary(args + 1) && (void *)args[1] != NULL &&
              check_buffer((void *)args[1], strlen((char *)args[1]));
    if (beneath) {
      f->eax = syscall_remove(args, pcb);
    }
    break;

  case SYS_OPEN:
    beneath = check_boundary(args + 1) && (void *)args[1] != NULL &&
              check_buffer((void *)args[1], strlen((char *)args[1]));
    if (beneath) {
      f->eax = syscall_open(args, pcb);
    }
    break;

  case SYS_FILESIZE:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = check_fd(args[1], pcb) ? syscall_filesize(args, pcb) : -1;
    }
    break;

  case SYS_READ:
    beneath = check_boundary(args + 3) && (void *)args[2] != NULL && check_buffer((void *)args[2], args[3]);
    if (beneath) {
      f->eax = check_fd(args[1], pcb) ? syscall_read(args, pcb) : -1;
    }
    break;

  case SYS_WRITE:
    beneath = check_boundary(args + 3) && (void *)args[2] != NULL && check_buffer((void *)args[2], args[3]);
    if (beneath) {
      f->eax = check_fd(args[1], pcb) ? syscall_write(args, pcb) : -1;
    }
    break;

  case SYS_SEEK:
    beneath = check_boundary(args + 2);
    if (beneath) {
      f->eax = check_fd((uint32_t)args[1], pcb) ? syscall_seek(args, pcb) : -1;
    }
    break;

  case SYS_TELL:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = check_fd((uint32_t)args[1], pcb) ? syscall_tell(args, pcb) : -1;
    }
    break;

  case SYS_CLOSE:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = check_fd((uint32_t)args[1], pcb) ? syscall_close(args, pcb) : -1;
    }
    break;

  case SYS_PRACTICE:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = beneath ? syscall_practice(args, pcb) : -1;
    }
    break;

  case SYS_COMPUTE_E:
    beneath = check_boundary(args + 1);
    if (beneath) {
      f->eax = beneath ? syscall_compute_e(args, pcb) : -1;
    }
    break;

  default:
    printf("Unknown system call number: %d\n", args[0]);
    process_exit(-1);
    break;
  }

  if (!beneath) {
    printf("%s: exit(%d)\n", pcb->process_name, f->eax);
    process_exit(f->eax);
  }
}

static int syscall_practice(uint32_t *args, struct process *pcb) { return (int)args[1] + 1; }

static double syscall_compute_e(uint32_t *args, struct process *pcb) {

  if (args[1] < 0) {
    return -1.0;
  }

  return sys_sum_to_e(args[1]);
}

static pid_t syscall_exec(uint32_t *args, struct process *pcb) {

  pid_t result = process_execute((const char *)args[1]);
  if (result == -1) {
    /* 线程初始化失败 */
    return result;
  }

  struct list *children = &(pcb->children);
  struct lock *children_lock = &(pcb->children_lock);
  struct child_process *child = NULL;
  lock_acquire(children_lock);
  list_for_each_entry(child, children, elem) {
    if (child->pid == result) {
      sema_down(child->editing);
      if (!child->exited && child->exited_code == -1) {
        /* 要么就是PCB初始化失败，要么就是在等待的时候已经搞完了，但是不应该由exec释放表中元素，到底还是得exit
         */
        result = -1;
        list_remove(&(child->elem));
        free(child);
      } else {
        /* PCB初始化成功  */
        sema_up(child->editing);
      }
      break;
    }
  }
  lock_release(children_lock);
  // printf("children empty: %s\n", list_empty(&(pcb->children)) == 1 ? "true" : "false");

  return result;
}

static int syscall_wait(uint32_t *args, struct process *pcb) { return process_wait(args[1]); }

static bool syscall_create(uint32_t *args, struct process *pcb) {
  bool result = false;

  result = filesys_create((char *)args[1], args[2]);

  return result;
}

static bool syscall_remove(uint32_t *args, struct process *pcb) {
  bool result = false;

  result = filesys_remove((char *)args[1]);

  return result;
}

static int syscall_open(uint32_t *args, struct process *pcb) {
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file *new_file = NULL;

  // printf("%s: open file%s\n", pcb->process_name, (char*)args[1]);

  new_file = filesys_open((char *)args[1]);

  if (new_file != NULL) {
    ASSERT(pcb->files_next_desc >= 3);
    struct file_desc *new_file_desc = malloc(sizeof(struct file_desc));
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

static int syscall_close(uint32_t *args, struct process *pcb) {
  uint32_t fd = args[1];
  int result = -1;
  if (fd < 3) {
    return result;
  }
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;
  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {

      file_close(pos->file);

      result = 0;
      list_remove(&(pos->elem));
      free(pos);
      break;
    }
  }
  lock_release(files_tab_lock);
  return result;
}

static int syscall_filesize(uint32_t *args, struct process *pcb) {
  uint32_t fd = args[1];
  int size = -1;
  if (fd < 3) {
    return size;
  }
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      size = (int)file_length(pos->file);
      break;
    }
  }
  lock_release(files_tab_lock);

  return size;
}

static int syscall_tell(uint32_t *args, struct process *pcb) {
  uint32_t fd = args[1];
  int size = -1;
  if (fd < 3) {
    return size;
  }
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      size = (int)file_tell(pos->file);
      break;
    }
  }
  lock_release(files_tab_lock);

  return size;
}

static int syscall_seek(uint32_t *args, struct process *pcb) {
  uint32_t fd = args[1];
  int result = -1;
  if (fd < 3) {
    return result;
  }
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {
      file_seek(pos->file, args[2]);
      result = 0;
      break;
    }
  }
  lock_release(files_tab_lock);

  return result;
}

static int syscall_read(uint32_t *args, struct process *pcb) {
  if (args[1] == 0) {
    for (uint32_t i = 0; i < (uint32_t)args[3]; i++) {
      *(uint8_t *)(args[2] + i) = input_getc();
    }
    return args[3];
  } else if (args[1] == 1) {
    return -1;
  }

  uint32_t fd = args[1];
  uint32_t off = -1;
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {

      off = file_read(pos->file, (void *)args[2], args[3]);

      break;
    }
  }
  lock_release(files_tab_lock);
  return off;
}

static int syscall_write(uint32_t *args, struct process *pcb) {
  if (args[1] == 1) {
    putbuf((char *)args[2], (size_t)args[3]);
    return 0;
  } else if (args[1] == 0) {
    return -1;
  }

  uint32_t fd = args[1];
  uint32_t off = -1;
  struct list *files_tab = &(pcb->files_tab);
  struct lock *files_tab_lock = &(pcb->files_lock);
  struct file_desc *pos = NULL;

  lock_acquire(files_tab_lock);
  list_for_each_entry(pos, files_tab, elem) {
    if (pos->file_desc == fd) {

      off = file_write(pos->file, (void *)args[2], args[3]);

      break;
    }
  }
  lock_release(files_tab_lock);
  return off;
}

static inline bool check_fd(uint32_t fd, struct process *pcb) { return pcb->files_next_desc >= fd; }
static inline bool check_buffer(void *buffer, uint32_t size) {
  return (void *)(buffer + size) <= (void *)0xbffffffe;
}

static inline bool check_boundary(void *buffer) { return (void *)(buffer) < (void *)0xbffffffe; }
