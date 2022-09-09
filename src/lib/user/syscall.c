#include <syscall.h>
#include "../syscall-nr.h"
#include <pthread.h>

/* Invokes syscall NUMBER, passing no arguments, and returns the
   return value as an `int'. */
#define syscall0(NUMBER)                                                                           \
  ({                                                                                               \
    int retval;                                                                                    \
    asm volatile("pushl %[number]; int $0x30; addl $4, %%esp"                                      \
                 : "=a"(retval)                                                                    \
                 : [number] "i"(NUMBER)                                                            \
                 : "memory");                                                                      \
    retval;                                                                                        \
  })

/* Invokes syscall NUMBER, passing argument ARG0, and returns the
   return value as an `int'. */
#define syscall1(NUMBER, ARG0)                                                                     \
  ({                                                                                               \
    int retval;                                                                                    \
    asm volatile("pushl %[arg0]; pushl %[number]; int $0x30; addl $8, %%esp"                       \
                 : "=a"(retval)                                                                    \
                 : [number] "i"(NUMBER), [arg0] "g"(ARG0)                                          \
                 : "memory");                                                                      \
    retval;                                                                                        \
  })

/* Invokes syscall NUMBER, passing argument ARG0, and returns the
   return value as a `double'. */
#define syscall1f(NUMBER, ARG0)                                                                    \
  ({                                                                                               \
    float retval;                                                                                  \
    asm volatile("pushl %[arg0]; pushl %[number]; int $0x30; addl $8, %%esp"                       \
                 : "=a"(retval)                                                                    \
                 : [number] "i"(NUMBER), [arg0] "g"(ARG0)                                          \
                 : "memory");                                                                      \
    retval;                                                                                        \
  })

/* Invokes syscall NUMBER, passing arguments ARG0 and ARG1, and
   returns the return value as an `int'. */
#define syscall2(NUMBER, ARG0, ARG1)                                                               \
  ({                                                                                               \
    int retval;                                                                                    \
    asm volatile("pushl %[arg1]; pushl %[arg0]; "                                                  \
                 "pushl %[number]; int $0x30; addl $12, %%esp"                                     \
                 : "=a"(retval)                                                                    \
                 : [number] "i"(NUMBER), [arg0] "r"(ARG0), [arg1] "r"(ARG1)                        \
                 : "memory");                                                                      \
    retval;                                                                                        \
  })

/* Invokes syscall NUMBER, passing arguments ARG0, ARG1, and
   ARG2, and returns the return value as an `int'. */
#define syscall3(NUMBER, ARG0, ARG1, ARG2)                                                         \
  ({                                                                                               \
    int retval;                                                                                    \
    asm volatile("pushl %[arg2]; pushl %[arg1]; pushl %[arg0]; "                                   \
                 "pushl %[number]; int $0x30; addl $16, %%esp"                                     \
                 : "=a"(retval)                                                                    \
                 : [number] "i"(NUMBER), [arg0] "r"(ARG0), [arg1] "r"(ARG1), [arg2] "r"(ARG2)      \
                 : "memory");                                                                      \
    retval;                                                                                        \
  })

int practice(int i) { return syscall1(SYS_PRACTICE, i); }

void halt(void) {
  syscall0(SYS_HALT);
  NOT_REACHED();
}

void exit(int status) {
  syscall1(SYS_EXIT, status);
  NOT_REACHED();
}

/* 无论父进程有多少个线程，子线程必然只有一个主线程 */
pid_t exec(const char* file) { return (pid_t)syscall1(SYS_EXEC, file); }

/* 如果子线程对指定进程执行wait，只有该线程会被暂停 */
int wait(pid_t pid) { return syscall1(SYS_WAIT, pid); }

bool create(const char* file, unsigned initial_size) {
  return syscall2(SYS_CREATE, file, initial_size);
}

bool remove(const char* file) { return syscall1(SYS_REMOVE, file); }

int open(const char* file) { return syscall1(SYS_OPEN, file); }

int filesize(int fd) { return syscall1(SYS_FILESIZE, fd); }

int read(int fd, void* buffer, unsigned size) { return syscall3(SYS_READ, fd, buffer, size); }

int write(int fd, const void* buffer, unsigned size) {
  return syscall3(SYS_WRITE, fd, buffer, size);
}

void seek(int fd, unsigned position) { syscall2(SYS_SEEK, fd, position); }

unsigned tell(int fd) { return syscall1(SYS_TELL, fd); }

void close(int fd) { syscall1(SYS_CLOSE, fd); }

mapid_t mmap(int fd, void* addr) { return syscall2(SYS_MMAP, fd, addr); }

void munmap(mapid_t mapid) { syscall1(SYS_MUNMAP, mapid); }

bool chdir(const char* dir) { return syscall1(SYS_CHDIR, dir); }

bool mkdir(const char* dir) { return syscall1(SYS_MKDIR, dir); }

bool readdir(int fd, char name[READDIR_MAX_LEN + 1]) { return syscall2(SYS_READDIR, fd, name); }

bool isdir(int fd) { return syscall1(SYS_ISDIR, fd); }

int inumber(int fd) { return syscall1(SYS_INUMBER, fd); }

double compute_e(int n) { return (double)syscall1f(SYS_COMPUTE_E, n); }

tid_t sys_pthread_create(stub_fun sfun, pthread_fun tfun, const void* arg) {
  return syscall3(SYS_PT_CREATE, sfun, tfun, arg);
}

/* 如果主线程执行此函数，那么主线程需要join所有活跃线程之后再退出 */
void sys_pthread_exit() {
  syscall0(SYS_PT_EXIT);
  NOT_REACHED();
}

/** 阻塞直到指定线程执行完毕之后再继续执行，同步相关的要求如下：
 * 1.仅可以join位于同一进程的线程，且这个线程尚未被join过
 * 2.可以join一个已经结束的线程
 * 3.任何线程可以join同进程的任何其他线程，如果主线程被join了
 *   那么在主线程执行thread_exit之后，在进程退出之前
 *   该线程会被唤醒（thread_unblock）
 * */ 
tid_t sys_pthread_join(tid_t tid) { return syscall1(SYS_PT_JOIN, tid); }

/**
 * @brief 将锁注册到内核空间
 * 
 * @param lock 指向用户空间的lock_t的指针
 * @return true 
 * @return false 
 */
bool lock_init(lock_t* lock) { return syscall1(SYS_LOCK_INIT, lock); }

/**
 * @brief 获取锁，如果需要的话阻塞
 * 
 * 获取锁成功时返回true
 * 如果锁尚未被注册到内核中或者当前线程已经获取该锁时返回false
 * 
 * @param lock 指向用户空间的lock_t的指针
 */
void lock_acquire(lock_t* lock) {
  bool success = syscall1(SYS_LOCK_ACQUIRE, lock);
  if (!success)
    exit(1);
}

/**
 * @brief 释放锁
 * 
 * 释放锁成功时返回true
 * 如果锁尚未被注册到内核中或者当前线程未持有该锁时返回false
 * 
 * @param lock 指向用户空间的lock_t的指针
 */
void lock_release(lock_t* lock) {
  bool success = syscall1(SYS_LOCK_RELEASE, lock);
  if (!success)
    exit(1);
}

bool sema_init(sema_t* sema, int val) { return syscall2(SYS_SEMA_INIT, sema, val); }

void sema_down(sema_t* sema) {
  bool success = syscall1(SYS_SEMA_DOWN, sema);
  if (!success)
    exit(1);
}

void sema_up(sema_t* sema) {
  bool success = syscall1(SYS_SEMA_UP, sema);
  if (!success)
    exit(1);
}

tid_t get_tid(void) { return syscall0(SYS_GET_TID); }
