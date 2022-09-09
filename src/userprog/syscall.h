#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

void syscall_init(void);

#endif /* userprog/syscall.h */

#ifndef USER_SYNC_TYPE
#define USER_SYNC_TYPE
/* Synchronization Types */
typedef char lock_t;
typedef char sema_t;
#endif
