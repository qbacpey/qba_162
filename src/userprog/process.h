#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"
#include "filesys/filesys.h"
#include <stdint.h>

// At most 8MB can be allocated to the stack
// These defines will be used in Project 2: Multithreading
#define MAX_STACK_PAGES (1 << 11)
#define MAX_THREADS 127

/* PIDs and TIDs are the same type. PID should be
   the TID of the main thread of the process */
typedef tid_t pid_t;

/* Thread functions (Project 2: Multithreading) */
typedef void (*pthread_fun)(void*);
typedef void (*stub_fun)(pthread_fun, void*);

/* 文件描述符表元素 
 * 
 * 开启即创建，关闭即释放
 * 创建的时候跳转到最后递增
 * 
 */
struct file_desc {
  uint32_t file_desc; /* 文件描述符，从3开始 */
  file* file;         /* 文件指针，务必注意释放问题 */
  list_elem elem;
};

/* 子进程表元素 
 * 
 * 状态码：
 * -1 进程尚未执行完毕（初始值，父进程exec的时候设置）
 * 0 进程成功退出（子进程执行exit的时候设置）
 * 1 进程异常退出（由内核进行设置，需要有更为详细的原因）
 * 
 * 地址：
 * 父进程执行exec设置child指向子进程PCB地址
 * 子进程exit正常退出的时候将地址设置为NULL
 * 子进程异常退出的时候将值设置为NULL
 * 
 */

struct child_process {
  pid_t pid;             /* 子进程ID，执行exec的时候设置 */
  uint32_t status;       /* 退出状态，子进程执行exit的时候设置 */
  struct process* child; /* 子进程PCB地址，子进程exit时候需要将此设置为NULL */
  list_elem elem;
};

/* 线程指针元素  struct list threads
 * 
 * 进程退出的时候需要使用这个表释放所有线程结构体的资源（list_entry）
 * 线程退出的时候需要将自身从此表中移除
 * 
 */


/* The process control block for a given process. Since
   there can be multiple threads per process, we need a separate
   PCB from the TCB. All TCBs in a process will have a pointer
   to the PCB, and the PCB will have a pointer to the main thread 
   of the process, which is `special`. 
   PCB 需要包含一个指向主线程的指针
   还需要包含一个子进程表
* 
* 资源释放问题就不必再说了，记得在进程退出的时候全部释放掉
*    
* 一个比较重要的问题是父子进程间的通信问题
* 父进程执行exec创建子进程的时候，需要设置子进程指向父进程PCB的指针
* 
* 如果子进程先于父进程退出，那么子进程就需要使用此指针设置父进程
* children中与之身相对应的元素的值，具体如何设置上方已经提到了
* 
* 如果父进程先于子进程退出，那么父进程就需要使用子进程中的地址
* 逐个逐个地通知各子进程。具体到实现中就是，将他们的parent设置为NULL
* 
*/
struct process {
  /* Owned by process.c. */
  uint32_t* pagedir;          /* Page directory. */
  struct process* parent;     /* 指向父进程 */
  struct child_process* self; /* 指向父进程中子进程的位置 */
  char process_name[16];      /* Name of the main thread */
  struct list files_tab; /* 元素是文件描述符表元素，也就是struct file_desc */
  struct list children;  /* 元素是子进程表元素，也就是struct child_process */
  struct list threads; /* 元素是TCB */

  struct thread* main_thread; /* Pointer to main thread */
};

void userprog_init(void);

pid_t process_execute(const char* file_name);
int process_wait(pid_t);
void process_exit(void);
void process_activate(void);

bool is_main_thread(struct thread*, struct process*);
pid_t get_pid(struct process*);

tid_t pthread_execute(stub_fun, pthread_fun, void*);
tid_t pthread_join(tid_t);
void pthread_exit(void);
void pthread_exit_main(void);

#endif /* userprog/process.h */
