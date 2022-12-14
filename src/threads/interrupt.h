#ifndef THREADS_INTERRUPT_H
#define THREADS_INTERRUPT_H

#include <stdbool.h>
#include <stdint.h>

#ifndef FPU_SIZE
#define FPU_SIZE 108
#endif

/* Interrupts on or off? */
enum intr_level {
  INTR_OFF, /* Interrupts disabled. */
  INTR_ON   /* Interrupts enabled. */
};

enum intr_level intr_get_level(void);
enum intr_level intr_set_level(enum intr_level);
enum intr_level intr_enable(void);
enum intr_level intr_disable(void);

/* 禁用中断，执行action */
#define DISABLE_INTR(action)                                                                       \
  do {                                                                                             \
    enum intr_level old_level = intr_disable();                                                    \
    { action; }                                                                                    \
    intr_set_level(old_level);                                                                     \
  } while (0)

/* Interrupt stack frame. */
struct intr_frame {
  /* Pushed by intr_entry in intr-stubs.S.
       These are the interrupted task's saved registers. */
  uint32_t edi;                /* Saved EDI. */
  uint32_t esi;                /* Saved ESI. */
  uint32_t ebp;                /* Saved EBP. */
  uint32_t esp_dummy;          /* Not used. */
  uint32_t ebx;                /* Saved EBX. */
  uint32_t edx;                /* Saved EDX. */
  uint32_t ecx;                /* Saved ECX. */
  uint32_t eax;                /* Saved EAX. */
  uint8_t fpu_state[FPU_SIZE]; /* FPU的所有状态 使用fsave和frstore维护 */
  uint16_t gs, : 16;           /* Saved GS segment register. */
  uint16_t fs, : 16;           /* Saved FS segment register. */
  uint16_t es, : 16;           /* Saved ES segment register. */
  uint16_t ds, : 16;           /* Saved DS segment register. */

  /* Pushed by intrNN_stub in intr-stubs.S. */
  uint32_t vec_no; /* Interrupt vector number. */

  /* Sometimes pushed by the CPU, 
       otherwise for consistency pushed as 0 by intrNN_stub.
       The CPU puts it just under `eip', but we move it here. */
  uint32_t error_code; /* Error code. */

  /* Pushed by intrNN_stub in intr-stubs.S.
     中断帧指针？还是用户函数的帧指针？

       This frame pointer eases interpretation of backtraces. */
  void* frame_pointer; /* Saved EBP (frame pointer). */

  /* Pushed by the CPU.
     被硬件保存的寄存器：(ss:esp) (eflags) (cs:eip) 所谓重要寄存器
          These are the interrupted task's saved registers. */
  void (*eip)(void); /* Next instruction to execute. */
  uint16_t cs, : 16; /* Code segment for eip. */
  uint32_t eflags;   /* Saved CPU flags. */
  void* esp;         /* Saved stack pointer. */
  uint16_t ss, : 16; /* Data segment for esp. */
};

typedef void intr_handler_func(struct intr_frame*);

void intr_init(void);
void intr_register_ext(uint8_t vec, intr_handler_func*, const char* name);
void intr_register_int(uint8_t vec, int dpl, enum intr_level, intr_handler_func*, const char* name);
bool intr_context(void);
void intr_yield_on_return(void);

void intr_dump_frame(const struct intr_frame*);
const char* intr_name(uint8_t vec);

#endif /* threads/interrupt.h */
