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

struct semaphore* filesys_sema = NULL; /* 定义全局临时文件系统锁 */
// static struct semaphore temporary; /* 现在才搞清楚原来这个temporary是用来和process_wait协作的 */
static thread_func start_process NO_RETURN;
static thread_func start_pthread NO_RETURN;
static bool load(const char* file_name, void (**eip)(void), void** esp);
bool setup_thread(void (**eip)(void), void** esp);

/* 用于在父子用户进程之间传递参数的 */
struct init_pcb {
  struct process* parent;
  struct child_process* self;
  struct semaphore* editing;
  char* cmd_line;
  bool processed; /* 是否处理过 cmd_line*/
};

/* Initializes user programs in the system by ensuring the main
   thread has a minimal PCB so that it can execute and wait for
   the first user process. Any additions to the PCB should be also
   initialized here if main needs those members 
   
   简而言之，在这里执行主线程（进程）的初始化操作
   */
void userprog_init(void) {

  if (filesys_sema == NULL) {
    malloc_type(filesys_sema);
    sema_init(filesys_sema, 1);
  }

  struct thread* t = thread_current();
  bool success;

  /* Allocate process control block
     It is imoprtant that this is a call to calloc and not malloc,
     so that t->pcb->pagedir is guaranteed to be NULL (the kernel's
     page directory) when t->pcb is assigned, because a timer interrupt
     can come at any time and activate our pagedir */
  t->pcb = calloc(sizeof(struct process), 1);
  success = t->pcb != NULL;
  /* Kill the kernel if we did not succeed */
  ASSERT(success);

  /* 进程表相关的逻辑 */
  struct process* pcb = t->pcb;
  list_init(&(pcb->children));

  malloc_type(pcb->editing);
  success = pcb->editing != NULL;
  ASSERT(success);
  sema_init(pcb->editing, 1);
  lock_init(&(pcb->children_lock));

  /* 文件描述表 */
  list_init(&(pcb->files_tab));
  lock_init(&(pcb->files_lock));

  /* 要让自己有一个进程表元素对应 */
  struct child_process* fake_child_elem = NULL;
  malloc_type(fake_child_elem);
  success = fake_child_elem != NULL;
  ASSERT(success);

  pcb->self = fake_child_elem;
  sema_init(&(fake_child_elem->waiting), 0);
  fake_child_elem->editing = pcb->editing;
  fake_child_elem->pid = t->tid;
  fake_child_elem->exited = true;
  fake_child_elem->child = pcb;
}

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.
  （这个说法实际上有点迷糊，它实际上指的是parent process 执行完 thread_create 之后，
   函数返回之前新线程有可能会被立即执行，
   并不是指代新线程会在此函数执行的任意位置都有可能被执行）  
   
   Returns the new process's
   process id, or TID_ERROR if the thread cannot be created. 
   
   这是原版的函数原型：pid_t process_execute(const char* file_name)

   */
pid_t process_execute(const char* file_name) {
  char* fn_copy;
  tid_t tid;
  struct process* pcb = thread_current()->pcb;
  struct list* children = &(pcb->children);
  struct lock* children_lock = &(pcb->children_lock);

  // 文件全局信号量？
  // sema_init(&temporary, 0);
  /* Make a copy of FILE_NAME.
     Otherwise there's a race between the caller and load(). */
  fn_copy = palloc_get_page(0);
  if (fn_copy == NULL)
    return TID_ERROR;

  bool success = false;
  /* 向自己的进程表中加入新进程 */
  struct child_process* child_elem = NULL;
  malloc_type(child_elem);
  success = child_elem != NULL;
  if (!success) {
    return TID_ERROR;
  }
  sema_init(&(child_elem->waiting), 0);
  child_elem->pid = -1;
  child_elem->child = NULL;
  child_elem->exited = false;
  child_elem->exited_code = -2; /* 初始退出码为-2 */
  lock_acquire(children_lock);
  list_push_front(children, &(child_elem->elem));
  lock_release(children_lock);
  malloc_type(child_elem->editing);
  success = child_elem->editing != NULL;
  if (!success) {
    free(child_elem);
    return TID_ERROR;
  }
  sema_init(child_elem->editing, 1);

  /* 初始化init_pcb（即start_process的参数） */
  struct init_pcb* init_pcb_ = NULL;
  malloc_type(init_pcb_);
  success = success && init_pcb_ != NULL;
  if (!success) {
    free(child_elem->editing);
    free(child_elem);
    return TID_ERROR;
  }
  success = init_pcb_ != NULL;
  init_pcb_->editing = child_elem->editing;
  init_pcb_->self = child_elem;
  init_pcb_->parent = pcb;
  init_pcb_->cmd_line = fn_copy;

  /* file_name的第一个空格设置为\0 拷贝系统调用参数到内核 */
  strlcpy(fn_copy, file_name, PGSIZE);
  char *token, *save_ptr;
  fn_copy = strtok_r(fn_copy, " ", &save_ptr);
  /* 与start_process协作，用于标识是否对文件名字符串进行了替换 */
  if (strlen(fn_copy) != strlen(file_name)) {
    init_pcb_->processed = true;
  } else {
    init_pcb_->processed = false;
  }

  /* Create a new thread to execute FILE_NAME. 
   *
   * 从thread_create开始，child就有可能打断parent并开始执行 
   * 为此，父进程的wait不仅需要等待waiting，
   * 还需要等待editing（当然child初始化PCB完成之后无论成功与否都需要释放editing）
   * 这主要是防止在child PCB未初始化完成时，父进程就访问其数据了
   * （父进程需要等待子进程PCB初始化执行完毕）
   */
  sema_down(init_pcb_->editing);
  tid = thread_create(fn_copy, PRI_DEFAULT, start_process, init_pcb_);
  child_elem->pid = tid;

done:
  if (tid == TID_ERROR)
    palloc_free_page(fn_copy);
  /* 返回进程ID给process_wait（注意，此时的线程是parent） */
  return tid;
}

/* A thread function that loads a user process and starts it
   running. */
static void start_process(void* init_pcb_) {
  /* 注意，此时的线程是child */
  struct init_pcb* init_pcb = (struct init_pcb*)init_pcb_;
  struct child_process* self = init_pcb->self;
  struct semaphore* editing = init_pcb->editing;
  char* file_name = init_pcb->cmd_line;
  struct thread* t = thread_current();
  struct intr_frame if_;
  bool success, pcb_success;

  /* 分配PCB空间 */
  struct process* new_pcb = malloc(sizeof(struct process));
  success = new_pcb != NULL;
  if (!success) {
    goto done;
  }

  /* 初始化PCB */
  // Ensure that timer_interrupt() -> schedule() -> process_activate()
  // does not try to activate our uninitialized pagedir
  new_pcb->pagedir = NULL;
  t->pcb = new_pcb;
  t->pcb->parent = init_pcb->parent;

  // 初始化文件描述符表
  list_init(&(t->pcb->files_tab));
  lock_init(&t->pcb->files_lock);
  t->pcb->files_next_desc = 3;
  t->pcb->filesys_sema = NULL;

  // 初始化子进程表
  t->pcb->self = self;
  list_init(&(t->pcb->children));
  t->pcb->editing = editing;
  lock_init(&(t->pcb->children_lock));

  // 初始化线程表
  // list_init ( &(t->pcb->threads) );

  // Continue initializing the PCB as normal
  t->pcb->main_thread = t;
  strlcpy(t->pcb->process_name, t->name, sizeof t->name);

  /* Initialize interrupt frame and load executable. */

  memset(&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  sema_down(filesys_sema);
  /* 需要确保绝无可能出现page fault */
  success = load(file_name, &if_.eip, &if_.esp);
  sema_up(filesys_sema);

  /* Handle failure with succesful PCB malloc. Must free the PCB */
  if (!success) {
    // Avoid race where PCB is freed before t->pcb is set to NULL
    // If this happens, then an unfortuantely timed timer interrupt
    // can try to activate the pagedir, but it is now freed memory
    struct process* pcb_to_free = t->pcb;
    t->pcb = NULL;
    free(pcb_to_free);
    free_parent_self(self, -1);
    goto done;
  }

  /* 需要确保if_中的状态是FPU的初始状态 */
  asm("fninit; fsave (%0)" : : "g"(&if_.fpu_state));

  /* 参数传递
   *
   * 说到底这里的作用应该是向用户栈里边压入初始参数
   * 也就是Argc Argv那些玩意
   * 但是现在不知道这两个参数被放在了什么地方
   * 于是只能随便调节一下栈指针，应付一下对齐检查
   * 
   * x86的要求是在将所有argument压入栈中之后
   * 栈对齐16位，也就是esp的末位应该是0
   * 调用call之后压入返回地址，esp的末位就应当变为c
   * 压入ebp之后，esp的末位就应当变为8（这也是%ebp实际所指向的地址）
   * 这也是为什么在callee中获取argument需要0x8(ebp)的原因
   * 
   * 具体到这里，由于桩函数有两个参数，因此执行intr_exit之前
   * 地址应当变为 0x10 + 0x4 (没有调用call，自然不会将返回地址压栈)
   * 
   * 最后，由于这里实现的是fake intr_frame 因此末尾对其的是 c 而不是 0
   * 
   * if_.esp -= 0x14;
   * */

  /* 如果字符串被处理过 那么需要将之前替换掉的'\0'换成' ' */
  if (init_pcb->processed) {
    int null_pos = 0;
    while ('\0' != file_name[null_pos]) {
      null_pos++;
    }
    file_name[null_pos] = ' ';
  }

  /* 拷贝字符串 */
  int raw_char_count = strlen(file_name) + 1;
  if_.esp -= (raw_char_count);
  strlcpy(if_.esp, file_name, raw_char_count);
  char* token = if_.esp;
  /* 对齐4byte */
  if_.esp = (void*)((uint32_t)if_.esp & 0xfffffff0);

  /* 已经算上了其他固定数量的参数 */
  int byte_counter = 0;

  /* argc 一会再设置 */
  byte_counter += 4;
  if_.esp -= 4;
  *(uint32_t*)if_.esp = 0;

  /* argv 一会再设置 */
  byte_counter += 4;
  if_.esp -= 4;
  *(uint32_t*)if_.esp = 0;

  /* 使用strtok对fn_copy进行处理 */

  char* save_ptr;
  int argc = 0;
  for (token = strtok_r(token, " ", &save_ptr); token != NULL;
       token = strtok_r(NULL, " ", &save_ptr)) {
    byte_counter += 4;
    if_.esp -= 4;
    *(uint32_t*)if_.esp = token;
    argc++;
  }
  /* 确保最后一位是NULL */
  byte_counter += 4;
  if_.esp -= 4;
  *(uint32_t*)if_.esp = 0;

  /* 对齐 */
  if (byte_counter % 0x10 != 0) {
    byte_counter += 0x10;
    byte_counter &= 0xfffffff0;
  }
  if_.esp = (void*)((uint32_t)if_.esp & 0xfffffff0);

  /* 反转 有空再优化 注意单位 */
  uint32_t last_pos = byte_counter - 4;
  uint32_t place;
  for (int i = 0; i < byte_counter / 2; i += 4) {
    memmove(&place, if_.esp + last_pos - i, 4);
    memmove(if_.esp + last_pos - i, if_.esp + i, 4);
    memmove(if_.esp + i, &place, 4);
  }

  /* 设置 argv */
  *((uint32_t*)if_.esp + 1) = (uint32_t*)if_.esp + 2;
  /* 设置 argc */
  *(uint32_t*)if_.esp = argc;

  /* 伪装返回值 */
  if_.esp -= 0x4;

done:
  /* 异常退出 Clean up. Exit on failure or jump to userspace */
  palloc_free_page(file_name);
  free(init_pcb);
  if (!success) {
    // sema_up(&temporary);
    /* 如果是exited==false 但是exited_code=-1就说明PCB初始化错误 */
    self->exited = false;
    sema_up(editing);
    thread_exit();
    NOT_REACHED();
  }
  self->pid = t->tid;
  self->child = t->pcb;
  self->exited = false;
  sema_up(editing);

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. 
  * 
  * asm asm-qualifiers (AssemblerTemplate : OutputOperands : InputOperands : Clobbers)
  * 本质上来说，可以将这种指令当成是一系列能将输入参数转换输出参数的低级指令集合
  * 
  * AssemblerTemplate: 汇编指令的模板，其中包含需要执行的汇编指令和令牌（即输入输出参数、goto参数）
  * 
  * "%%"表示汇编器模板中的单个"%"
  * "%|""%{""%}"用于转义
  * 
  * OuputOperands: 会被汇编指令修改的C变量列表，以逗号分隔
  * 
  * 这是输出操作数的典型格式[ [asmSymbolicName] ] constraint (cvariablename)
  * asmSymbolicName是该C变量的符号化名字，可以在汇编代码中使用%[name]引用这个变量
  * 如果不使用asmSymbolicName为符号命名，那么就需要使用从零开始的位置符号对其进行引用
  * %0表示所有参数中的第一个，%1表示第二个，以此类推
  * 
  * constraint指的是对操作数施加的限制（符号），
  * 也能表示其允许分布的位置（字母）：用于声明可操作数可以被放在什么位置上（内存、立即数、寄存器...）
  * "r"表示这个C表达式只能充当：寄存器
  * "m"表示这个C表达式只能充当：内存
  * "i"表示这个C表达式只能充当：立即数
  * "g"表示这个C表达式既可充当：寄存器、内存、立即数
  * 
  * 输出操作数必须以"="或"+"开头
  * 前者表示将会覆写这个变量（只写入），后者表示可能读也可能写
  * 随后需要使用一个字母声明这个变量的可接受的位置（‘r’ for register and ‘m’ for memory）
  * 
  * 
  * InputOperands: 会被AssemblerTemplate中的汇编指令读取的C表达式列表
  * 
  * Clobbers: 会被汇编指令修改的寄存器或者值列表
  * 
  * "memory"表示指令会可能将Input或者Output当成地址并读取其中的数据
  * 
  * 
  */

  asm volatile(" movl %0, %%esp ; jmp intr_exit" : : "g"(&if_) : "memory");

  /* 关于指令执行的一点观察
   * 
   * 1. 执行jmp以及call等指令，硬件会自动保存关键寄存器的值
   *    相对的，iret 就会跳回原来的地方并恢复关键寄存器的值
   * 
   * 2. C内存中的结构体是按照定义的顺序向下长的，即如果直接使用
   *    结构体的地址访问4byte数据的话，会访问到结构体的第一个元素
   *    向上移4byte会访问到第二个元素
   * 
   * 3. 那么这里设置esp/eip这些本来应该由硬件维护的寄存器（关键寄存器）的意义在于
   *    让 intr_exit 处的 iret 指令能利用 _if 中的数据直接跳转到程序的开始处
   *    并开始执行用户程序
   * 
   */
  NOT_REACHED();
}

/* Waits for process with PID child_pid to die and returns its exit status.
   If it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If child_pid is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given PID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int process_wait(pid_t child_pid) {

  int result = -1;
  struct thread* tcb = thread_current();

  // 子线程的id和父进程的id之间没有固定的关系
  // 因此下面这些代码被注释掉了，用来引以为戒
  // if (child_pid <= tcb->tid) {
  //   return result;
  // }

  struct process* pcb = tcb->pcb;
  struct list* children = &(pcb->children);
  struct lock* children_lock = &(pcb->children_lock);
  struct child_process* child = NULL;
  lock_acquire(children_lock);
  list_for_each_entry(child, children, elem) {
    if (child->pid == child_pid) {
      sema_down(&(child->waiting));
      /* 此时child必然已经执行完毕了 */
      result = child->exited_code;
      list_remove(&(child->elem));
      free_child_self(child);
      break;
    }
  }
  lock_release(children_lock);
  // sema_down(&temporary);
  return result;
}

/* Free the current process's resources. 
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
void process_exit(int exit_code) {
  struct thread* cur = thread_current();
  uint32_t* pd;

  /* If this thread does not have a PCB, don't worry
   * 非用户进程 直接退出
   */
  if (cur->pcb == NULL) {
    thread_exit();
    NOT_REACHED();
  }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pcb->pagedir;
  if (pd != NULL) {
    /* Correct ordering here is crucial.  We must set
         cur->pcb->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
    cur->pcb->pagedir = NULL;
    pagedir_activate(NULL);
    pagedir_destroy(pd);
  }

  /* Free the PCB of this process and kill this thread
     Avoid race where PCB is freed before t->pcb is set to NULL
     If this happens, then an unfortuantely timed timer interrupt
     can try to activate the pagedir, but it is now freed memory */
  /* 需要确保子进程编辑自己的PCB之前先获取父进程的锁 */
  struct process* pcb_to_free = cur->pcb;
  struct semaphore* editing = pcb_to_free->editing;
  sema_down(editing);
  free_parent_self(pcb_to_free->self, exit_code);
  sema_up(editing);

  if (pcb_to_free->filesys_sema != NULL) {
    sema_up(pcb_to_free->filesys_sema); /* 文件系统执行操作时发生page fault */
  }

  sema_down(filesys_sema);
  file_close(pcb_to_free->exec);
  sema_up(filesys_sema);

  /* 释放文件描述符表，必须使用 NULL 进行初始化 */
  struct file_desc* file_pos = NULL;
  list_clean_each(file_pos, &(pcb_to_free->files_tab), elem,
                  {
                    file_close(file_pos->file);
                    free(file_pos);
                  });

      /* 释放子进程表 */
      struct child_process* child = NULL;
  struct semaphore* child_editing = NULL;
  list_clean_each(child, &(pcb_to_free->children), elem,
                  {
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

      /* 资源全部释放 */
      cur->pcb = NULL;
  free(pcb_to_free);

  // sema_up(&temporary);
  thread_exit();
}

/* Sets up the CPU for running user code in the current
   thread. This function is called on every context switch. */
void process_activate(void) {
  struct thread* t = thread_current();

  /* Activate thread's page tables. */
  if (t->pcb != NULL && t->pcb->pagedir != NULL)
    pagedir_activate(t->pcb->pagedir);
  else
    pagedir_activate(NULL);

  /* Set thread's kernel stack for use in processing interrupts.
     This does nothing if this is not a user process. */
  tss_update();
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

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32 /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32 /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32 /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16 /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr {
  unsigned char e_ident[16];
  Elf32_Half e_type;
  Elf32_Half e_machine;
  Elf32_Word e_version;
  Elf32_Addr e_entry;
  Elf32_Off e_phoff;
  Elf32_Off e_shoff;
  Elf32_Word e_flags;
  Elf32_Half e_ehsize;
  Elf32_Half e_phentsize;
  Elf32_Half e_phnum;
  Elf32_Half e_shentsize;
  Elf32_Half e_shnum;
  Elf32_Half e_shstrndx;
};

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr {
  Elf32_Word p_type;
  Elf32_Off p_offset;
  Elf32_Addr p_vaddr;
  Elf32_Addr p_paddr;
  Elf32_Word p_filesz;
  Elf32_Word p_memsz;
  Elf32_Word p_flags;
  Elf32_Word p_align;
};

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL 0           /* Ignore. */
#define PT_LOAD 1           /* Loadable segment. */
#define PT_DYNAMIC 2        /* Dynamic linking info. */
#define PT_INTERP 3         /* Name of dynamic loader. */
#define PT_NOTE 4           /* Auxiliary info. */
#define PT_SHLIB 5          /* Reserved. */
#define PT_PHDR 6           /* Program header table. */
#define PT_STACK 0x6474e551 /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1 /* Executable. */
#define PF_W 2 /* Writable. */
#define PF_R 4 /* Readable. */

static bool setup_stack(void** esp);
static bool validate_segment(const struct Elf32_Phdr*, struct file*);
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool load(const char* file_name, void (**eip)(void), void** esp) {
  struct thread* t = thread_current();
  struct Elf32_Ehdr ehdr;
  struct file* file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;

  /* Allocate and activate page directory. */
  t->pcb->pagedir = pagedir_create();
  if (t->pcb->pagedir == NULL)
    goto done;
  process_activate();

  /* Open executable file. */
  file = filesys_open(file_name);
  if (file == NULL) {
    printf("load: %s: open failed\n", file_name);
    goto done;
  }

  /* Read and verify executable header. */
  if (file_read(file, &ehdr, sizeof ehdr) != sizeof ehdr ||
      memcmp(ehdr.e_ident, "\177ELF\1\1\1", 7) || ehdr.e_type != 2 || ehdr.e_machine != 3 ||
      ehdr.e_version != 1 || ehdr.e_phentsize != sizeof(struct Elf32_Phdr) || ehdr.e_phnum > 1024) {
    printf("load: %s: error loading executable\n", file_name);
    goto done;
  }

  /* TODO 应该在下面的goto也加上file_allow_write的 防止修改可执行文件 */
  file_deny_write(file);
  t->pcb->exec = file;

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) {
    struct Elf32_Phdr phdr;

    if (file_ofs < 0 || file_ofs > file_length(file))
      goto done;
    file_seek(file, file_ofs);

    if (file_read(file, &phdr, sizeof phdr) != sizeof phdr)
      goto done;
    file_ofs += sizeof phdr;
    switch (phdr.p_type) {
      case PT_NULL:
      case PT_NOTE:
      case PT_PHDR:
      case PT_STACK:
      default:
        /* Ignore this segment. */
        break;
      case PT_DYNAMIC:
      case PT_INTERP:
      case PT_SHLIB:
        goto done;
      case PT_LOAD:
        if (validate_segment(&phdr, file)) {
          bool writable = (phdr.p_flags & PF_W) != 0;
          uint32_t file_page = phdr.p_offset & ~PGMASK;
          uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
          uint32_t page_offset = phdr.p_vaddr & PGMASK;
          uint32_t read_bytes, zero_bytes;
          if (phdr.p_filesz > 0) {
            /* Normal segment.
                     Read initial part from disk and zero the rest. */
            read_bytes = page_offset + phdr.p_filesz;
            zero_bytes = (ROUND_UP(page_offset + phdr.p_memsz, PGSIZE) - read_bytes);
          } else {
            /* Entirely zero.
                     Don't read anything from disk. */
            read_bytes = 0;
            zero_bytes = ROUND_UP(page_offset + phdr.p_memsz, PGSIZE);
          }
          if (!load_segment(file, file_page, (void*)mem_page, read_bytes, zero_bytes, writable))
            goto done;
        } else
          goto done;
        break;
    }
  }

  /* Set up stack. */
  if (!setup_stack(esp))
    goto done;

  /* Start address. */
  *eip = (void (*)(void))ehdr.e_entry;

  // 这里是不是应该调用 setup_thread ?

  success = true;

done:
  /* We arrive here whether the load is successful or not. */
  if (!success) {
    file_close(file);
  }
  return success;
}

/* load() helpers. */

static bool install_page(void* upage, void* kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool validate_segment(const struct Elf32_Phdr* phdr, struct file* file) {
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
    return false;

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off)file_length(file))
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz)
    return false;

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;

  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr((void*)phdr->p_vaddr))
    return false;
  if (!is_user_vaddr((void*)(phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool load_segment(struct file* file, off_t ofs, uint8_t* upage, uint32_t read_bytes,
                         uint32_t zero_bytes, bool writable) {
  ASSERT((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT(pg_ofs(upage) == 0);
  ASSERT(ofs % PGSIZE == 0);

  file_seek(file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) {
    /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
    size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
    size_t page_zero_bytes = PGSIZE - page_read_bytes;

    /* Get a page of memory. */
    uint8_t* kpage = palloc_get_page(PAL_USER);
    if (kpage == NULL)
      return false;

    /* Load this page. */
    if (file_read(file, kpage, page_read_bytes) != (int)page_read_bytes) {
      palloc_free_page(kpage);
      return false;
    }
    memset(kpage + page_read_bytes, 0, page_zero_bytes);

    /* Add the page to the process's address space. */
    if (!install_page(upage, kpage, writable)) {
      palloc_free_page(kpage);
      return false;
    }

    /* Advance. */
    read_bytes -= page_read_bytes;
    zero_bytes -= page_zero_bytes;
    upage += PGSIZE;
  }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool setup_stack(void** esp) {
  uint8_t* kpage;
  bool success = false;

  kpage = palloc_get_page(PAL_USER | PAL_ZERO);
  if (kpage != NULL) {
    success = install_page(((uint8_t*)PHYS_BASE) - PGSIZE, kpage, true);
    if (success)
      *esp = PHYS_BASE;
    else
      palloc_free_page(kpage);
  }
  return success;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool install_page(void* upage, void* kpage, bool writable) {
  struct thread* t = thread_current();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page(t->pcb->pagedir, upage) == NULL &&
          pagedir_set_page(t->pcb->pagedir, upage, kpage, writable));
}

/* Returns true if t is the main thread of the process p */
bool is_main_thread(struct thread* t, struct process* p) { return p->main_thread == t; }

/* Gets the PID of a process */
pid_t get_pid(struct process* p) { return (pid_t)p->main_thread->tid; }

/* Creates a new stack for the thread and sets up its arguments.
   Stores the thread's entry point into *EIP and its initial stack
   pointer into *ESP. Handles all cleanup if unsuccessful. Returns
   true if successful, false otherwise.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. You may find it necessary to change the
   function signature. */
bool setup_thread(void (**eip)(void) UNUSED, void** esp UNUSED) { return false; }

/* Starts a new thread with a new user stack running SF, which takes
   TF and ARG as arguments on its user stack. This new thread may be
   scheduled (and may even exit) before pthread_execute () returns.
   Returns the new thread's TID or TID_ERROR if the thread cannot
   be created properly.

   This function will be implemented in Project 2: Multithreading and
   should be similar to process_execute (). For now, it does nothing.
   */
tid_t pthread_execute(stub_fun sf UNUSED, pthread_fun tf UNUSED, void* arg UNUSED) { return -1; }

/* A thread function that creates a new user thread and starts it
   running. Responsible for adding itself to the list of threads in
   the PCB.

   This function will be implemented in Project 2: Multithreading and
   should be similar to start_process (). For now, it does nothing. */
static void start_pthread(void* exec_ UNUSED) {}

/* Waits for thread with TID to die, if that thread was spawned
   in the same process and has not been waited on yet. Returns TID on
   success and returns TID_ERROR on failure immediately, without
   waiting.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
tid_t pthread_join(tid_t tid UNUSED) { return -1; }

/* Free the current thread's resources. Most resources will
   be freed on thread_exit(), so all we have to do is deallocate the
   thread's userspace stack. Wake any waiters on this thread.

   The main thread should not use this function. See
   pthread_exit_main() below.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit(void) {}

/* Only to be used when the main thread explicitly calls pthread_exit.
   The main thread should wait on all threads in the process to
   terminate properly, before exiting itself. When it exits itself, it
   must terminate the process in addition to all necessary duties in
   pthread_exit.

   This function will be implemented in Project 2: Multithreading. For
   now, it does nothing. */
void pthread_exit_main(void) {}

/* Reads a byte at user virtual address UADDR. UADDR must be below PHYS_BASE.
Returns the byte value if successful, -1 if a segfault occurred. */
static int get_user(const uint8_t* uaddr) {
  int result;
  asm("movl $1f, %0; movzbl %1, %0; 1:" : "=&a"(result) : "m"(*uaddr));
  return result;
}

/* Writes BYTE to user address UDST. UDST must be below PHYS_BASE. Returns
true if successful, false if a segfault occurred. */
static bool put_user(uint8_t* udst, uint8_t byte) {
  int error_code;
  asm("movl $1f, %0; movb %b2, %1; 1:" : "=&a"(error_code), "=m"(*udst) : "q"(byte));
  return error_code != -1;
}
