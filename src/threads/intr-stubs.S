#include "threads/loader.h"

        .text

/* Main interrupt entry point.

   整个中断的函数调用过程可归纳如下（现在假设是由硬件计时器引发的外部中断）：

   1.硬件计时器：触发外部中断，保存`struct intr_frame'中的三个关键值到中断栈中
     随后跳转到intr_stub

   2.intr_stub继续将`struct intr_frame'中的
     frame_pointer, error_code, and vec_no保存到中断栈中
     随后跳转到intr_entry（中断处理程序下半部分）

   3.intr_entry将`struct intr_frame'中的所有寄存器保存到栈中
     也就是为调用intr_handler函数配置参数
     同时需要设置各种寄存器，如%eax/%ebp等
     随后调用intr_handler（中断处理程序下半部分）

   4.intr_handler的作用实际上就是调用合适的中断处理器上半部分
     如果是外部中断，那么它会确保中断处理器上半部分执行过程中中断始终被禁用
     随后中断处理程序就会根据intr_frame中的error_code调用合适的处理程序
     （之所以能回来说不定是因为leal 164(%esp), %ebp）
     待中断处理程序执行完毕之后此函数返回的时候就会"滑"到intr_exit中
   
   5.intr_exit：退出，作用基本是将中断栈里的除了三个关键值之外的寄存器弹出
     

   An internal or external interrupt starts in one of the
   intrNN_stub routines, which push the `struct intr_frame'
   frame_pointer, error_code, and vec_no members on the stack,
   then jump here.（这部分工作由硬件来完成）

   We save the rest of the `struct intr_frame' members to the
   stack, set up some registers as needed by the kernel, and then
   call intr_handler(), which actually handles the interrupt.

   We "fall through" to intr_exit to return from the interrupt.

   x87 FPU state包括：FPU环境（x87 FPU environment）、处于FPU寄存器栈中的数据寄存器（data registers）
   FPU环境包括：FPU控制寄存器（x87 FPU’s control registers）、FPU状态寄存器（x87 FPU’s status registers）

   FSAVE会在保存完FPU状态（x87 FPU state）之后初始化FPU（效果如FINIT）
   FRSTOR 会恢复所有东西
   
   FSAVE instruction checks for and handles pending unmasked 
   floating-point exceptions before storing the FPU state; 
   the FNSAVE instruction does not.

*/
.func intr_entry
intr_entry:
	/* Save caller's registers.
   
   接续intr_stub的工作（frame_pointer, error_code, and vec_no已被保存）
   继续保存interrupt.h中的struct intr_frame */
	pushl %ds # 尽管长度是2byte但是push和pop的时候还是按4byte计算
	pushl %es
	pushl %fs
	pushl %gs
   subl $108, %esp 
   fwait
   fsave (%esp)
	pushal

	/* Set up kernel environment. */
	cld			/* String instructions go upward. */
	mov $SEL_KDSEG, %eax	/* Initialize segment registers. */
	mov %eax, %ds
	mov %eax, %es

   /* 164 = 56 + 108

      164(%esp) 指向的地址中恰好保存着指向struct intr_frame的frame_pointer
      其中保存的地址似乎是struct intr_frame在内核栈中的的起始地址 */ 
	leal 164(%esp), %ebp	/* Set up frame pointer. */

	/* Call interrupt handler. */
	pushl %esp
.globl intr_handler
	call intr_handler
	addl $4, %esp
.endfunc

/* Interrupt exit.

   Restores the caller's registers, discards extra data on the
   stack, and returns to the caller.

   This is a separate function because it is called directly when
   we launch a new user process (see start_process() in
   userprog/process.c). */
.globl intr_exit
.func intr_exit
intr_exit:
	/* Restore caller's registers. */
	popal
   frstor (%esp)
   addl $108, %esp 
	popl %gs
	popl %fs
	popl %es
	popl %ds

        /* Discard `struct intr_frame' vec_no, error_code,
           frame_pointer members. */
	addl $12, %esp

   # 确保栈中只剩下5个关键寄存器

        /* Return to caller. */
	iret
.endfunc

/* Interrupt stubs.

   This defines 256 fragments of code, named `intr00_stub'
   through `intrff_stub', each of which is used as the entry
   point for the corresponding interrupt vector.  It also puts
   the address of each of these functions in the correct spot in
   `intr_stubs', an array of function pointers.

   第一，任何地方触发中断都会跳到这个地方随后根据中断号调用合适的中断处理函数
         向栈中压入部分数据之后（%ebp、错误码、中断号）
         随后才会跳到intr_entry
   第二，这里的栈是向下长的
   第三，push和pop的单位是4byte，使用%esp作为栈指针
   另外，pushl (%esp)的意思是将%esp所指向的地址中的内容压到esp中
         随后下移4byte


   Most of the stubs do this:

        1. Push %ebp on the stack (frame_pointer in `struct intr_frame').

        2. Push 0 on the stack (error_code).

        3. Push the interrupt number on the stack (vec_no).

   The CPU pushes an extra "error code" on the stack for a few
   interrupts.  Because we want %ebp to be where the error code
   is, we follow a different path:（注意，错误码有时候也会被CPU压入）、

   此时的原本的ebp位置成了错误码，因此需要修改一下栈的布局

        1. Push a duplicate copy of the error code on the stack.

        2. Replace the original copy of the error code by %ebp.

        3. Push the interrupt number on the stack. */

	.data
.globl intr_stubs
intr_stubs:

/* This implements steps 1 and 2, described above, in the common
   case where we just push a 0 error code. */
#define zero                                    \
	pushl %ebp;                             \
	pushl $0

/* This implements steps 1 and 2, described above, in the case
   where the CPU already pushed an error code. */
#define REAL                                    \
        pushl (%esp);                           \
        movl %ebp, 4(%esp)

/* Emits a stub for interrupt vector NUMBER.
   TYPE is `zero', for the case where we push a 0 error code,
   or `REAL', if the CPU pushes an error code for us. */
#define STUB(NUMBER, TYPE)                      \
	.text;                                  \
.func intr##NUMBER##_stub;			\
intr##NUMBER##_stub:                            \
	TYPE;                                   \
	push $0x##NUMBER;                       \
        jmp intr_entry;                         \
.endfunc;					\
                                                \
	.data;                                  \
	.long intr##NUMBER##_stub;

/* All the stubs. */
STUB(00, zero) STUB(01, zero) STUB(02, zero) STUB(03, zero)
STUB(04, zero) STUB(05, zero) STUB(06, zero) STUB(07, zero)
STUB(08, REAL) STUB(09, zero) STUB(0a, REAL) STUB(0b, REAL)
STUB(0c, zero) STUB(0d, REAL) STUB(0e, REAL) STUB(0f, zero)

STUB(10, zero) STUB(11, REAL) STUB(12, zero) STUB(13, zero)
STUB(14, zero) STUB(15, zero) STUB(16, zero) STUB(17, zero)
STUB(18, REAL) STUB(19, zero) STUB(1a, REAL) STUB(1b, REAL)
STUB(1c, zero) STUB(1d, REAL) STUB(1e, REAL) STUB(1f, zero)

STUB(20, zero) STUB(21, zero) STUB(22, zero) STUB(23, zero)
STUB(24, zero) STUB(25, zero) STUB(26, zero) STUB(27, zero)
STUB(28, zero) STUB(29, zero) STUB(2a, zero) STUB(2b, zero)
STUB(2c, zero) STUB(2d, zero) STUB(2e, zero) STUB(2f, zero)

STUB(30, zero) STUB(31, zero) STUB(32, zero) STUB(33, zero)
STUB(34, zero) STUB(35, zero) STUB(36, zero) STUB(37, zero)
STUB(38, zero) STUB(39, zero) STUB(3a, zero) STUB(3b, zero)
STUB(3c, zero) STUB(3d, zero) STUB(3e, zero) STUB(3f, zero)

STUB(40, zero) STUB(41, zero) STUB(42, zero) STUB(43, zero)
STUB(44, zero) STUB(45, zero) STUB(46, zero) STUB(47, zero)
STUB(48, zero) STUB(49, zero) STUB(4a, zero) STUB(4b, zero)
STUB(4c, zero) STUB(4d, zero) STUB(4e, zero) STUB(4f, zero)

STUB(50, zero) STUB(51, zero) STUB(52, zero) STUB(53, zero)
STUB(54, zero) STUB(55, zero) STUB(56, zero) STUB(57, zero)
STUB(58, zero) STUB(59, zero) STUB(5a, zero) STUB(5b, zero)
STUB(5c, zero) STUB(5d, zero) STUB(5e, zero) STUB(5f, zero)

STUB(60, zero) STUB(61, zero) STUB(62, zero) STUB(63, zero)
STUB(64, zero) STUB(65, zero) STUB(66, zero) STUB(67, zero)
STUB(68, zero) STUB(69, zero) STUB(6a, zero) STUB(6b, zero)
STUB(6c, zero) STUB(6d, zero) STUB(6e, zero) STUB(6f, zero)

STUB(70, zero) STUB(71, zero) STUB(72, zero) STUB(73, zero)
STUB(74, zero) STUB(75, zero) STUB(76, zero) STUB(77, zero)
STUB(78, zero) STUB(79, zero) STUB(7a, zero) STUB(7b, zero)
STUB(7c, zero) STUB(7d, zero) STUB(7e, zero) STUB(7f, zero)

STUB(80, zero) STUB(81, zero) STUB(82, zero) STUB(83, zero)
STUB(84, zero) STUB(85, zero) STUB(86, zero) STUB(87, zero)
STUB(88, zero) STUB(89, zero) STUB(8a, zero) STUB(8b, zero)
STUB(8c, zero) STUB(8d, zero) STUB(8e, zero) STUB(8f, zero)

STUB(90, zero) STUB(91, zero) STUB(92, zero) STUB(93, zero)
STUB(94, zero) STUB(95, zero) STUB(96, zero) STUB(97, zero)
STUB(98, zero) STUB(99, zero) STUB(9a, zero) STUB(9b, zero)
STUB(9c, zero) STUB(9d, zero) STUB(9e, zero) STUB(9f, zero)

STUB(a0, zero) STUB(a1, zero) STUB(a2, zero) STUB(a3, zero)
STUB(a4, zero) STUB(a5, zero) STUB(a6, zero) STUB(a7, zero)
STUB(a8, zero) STUB(a9, zero) STUB(aa, zero) STUB(ab, zero)
STUB(ac, zero) STUB(ad, zero) STUB(ae, zero) STUB(af, zero)

STUB(b0, zero) STUB(b1, zero) STUB(b2, zero) STUB(b3, zero)
STUB(b4, zero) STUB(b5, zero) STUB(b6, zero) STUB(b7, zero)
STUB(b8, zero) STUB(b9, zero) STUB(ba, zero) STUB(bb, zero)
STUB(bc, zero) STUB(bd, zero) STUB(be, zero) STUB(bf, zero)

STUB(c0, zero) STUB(c1, zero) STUB(c2, zero) STUB(c3, zero)
STUB(c4, zero) STUB(c5, zero) STUB(c6, zero) STUB(c7, zero)
STUB(c8, zero) STUB(c9, zero) STUB(ca, zero) STUB(cb, zero)
STUB(cc, zero) STUB(cd, zero) STUB(ce, zero) STUB(cf, zero)

STUB(d0, zero) STUB(d1, zero) STUB(d2, zero) STUB(d3, zero)
STUB(d4, zero) STUB(d5, zero) STUB(d6, zero) STUB(d7, zero)
STUB(d8, zero) STUB(d9, zero) STUB(da, zero) STUB(db, zero)
STUB(dc, zero) STUB(dd, zero) STUB(de, zero) STUB(df, zero)

STUB(e0, zero) STUB(e1, zero) STUB(e2, zero) STUB(e3, zero)
STUB(e4, zero) STUB(e5, zero) STUB(e6, zero) STUB(e7, zero)
STUB(e8, zero) STUB(e9, zero) STUB(ea, zero) STUB(eb, zero)
STUB(ec, zero) STUB(ed, zero) STUB(ee, zero) STUB(ef, zero)

STUB(f0, zero) STUB(f1, zero) STUB(f2, zero) STUB(f3, zero)
STUB(f4, zero) STUB(f5, zero) STUB(f6, zero) STUB(f7, zero)
STUB(f8, zero) STUB(f9, zero) STUB(fa, zero) STUB(fb, zero)
STUB(fc, zero) STUB(fd, zero) STUB(fe, zero) STUB(ff, zero)
