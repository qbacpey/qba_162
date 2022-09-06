#ifndef THREADS_SWITCH_H
#define THREADS_SWITCH_H

#ifndef FPU_SIZE
#define FPU_SIZE 108
#endif

#ifndef __ASSEMBLER__
/* switch_thread()'s stack frame. */
struct switch_threads_frame {
  uint32_t edi;        /*  0: Saved %edi. */
  uint32_t esi;        /*  4: Saved %esi. */
  uint32_t ebp;        /*  8: Saved %ebp. */
  uint32_t ebx;        /* 12: Saved %ebx. */
  uint8_t fpu_state[FPU_SIZE]; /* FPU的所有状态 使用fsave和frstore维护 */
  void (*eip)(void);   /* 16: Return address. */
  struct thread* cur;  /* 20: switch_threads()'s CUR argument. */
  struct thread* next; /* 24: switch_threads()'s NEXT argument. */
};

/* Switches from CUR, which must be the running thread, to NEXT,
   which must also be running switch_threads(), returning CUR in
   NEXT's context. 
   
   函数实现位于switch.S中，通过操作寄存器和中断栈实现从cur到next的线程切换*/
struct thread* switch_threads(struct thread* cur, struct thread* next);

/* Stack frame for switch_entry(). */
struct switch_entry_frame {
  void (*eip)(void);
};

void switch_entry(void);

/* Pops the CUR and NEXT arguments off the stack, for use in
   initializing threads. */
void switch_thunk(void);
#endif

/* Offsets used by switch.S. */
#define SWITCH_CUR 128
#define SWITCH_NEXT 132

#endif /* threads/switch.h */
