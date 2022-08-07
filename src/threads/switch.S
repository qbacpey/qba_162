#include "threads/switch.h"

#### struct thread *switch_threads (struct thread *cur, struct thread *next);
####
#### Switches from CUR, which must be the running thread, to NEXT,
#### which must also be running switch_threads(), returning CUR in
#### NEXT's context.
####
#### This function works by assuming that the thread we're switching
#### into is also running switch_threads().  Thus, all it has to do is
#### preserve a few registers on the stack, then switch stacks and
#### restore the registers.  As part of switching stacks we record the
#### current stack pointer in CUR's thread structure.

# 简而言之，如果switch_threads的next是第一次被调度运行的线程
# 那么switch_threads执行完后就会跳到switch_entry中
# 最后就执行thread_switch_tail，完成整个初始化过程

.globl switch_threads
.func switch_threads
switch_threads:
	# Save caller's register state.
	#
	# Note that the SVR4 ABI allows us to destroy %eax, %ecx, %edx,
	# but requires us to preserve %ebx, %ebp, %esi, %edi.  See
	# [SysV-ABI-386] pages 3-11 and 3-12 for details.
	#
	# This stack frame must match the one set up by thread_create()
	# in size.
	subl $FPU_SIZE, %esp 
	fwait
   	fsave (%esp)
	pushl %ebx
	pushl %ebp
	pushl %esi
	pushl %edi

	# Get offsetof (struct thread, stack).
.globl thread_stack_ofs
	mov thread_stack_ofs, %edx

	# Save current stack pointer to old thread's stack, if any.
	# 获取switch_threads参数列表的第一个参数：struct thread* cur
	movl SWITCH_CUR(%esp), %eax
	# 将当前%esp（即直到pushl %edi为止的栈指针）保存到cur->stack中
	movl %esp, (%eax,%edx,1)

	# Restore stack pointer from new thread's stack.
	# 获取switch_threads参数列表的第二个参数：struct thread* next
	movl SWITCH_NEXT(%esp), %ecx
	# 将当前栈指针修改为next->stack，切换到next的栈（注意，两个线程的栈位于同一地址空间）
	movl (%ecx,%edx,1), %esp

	# Restore caller's register state.
	popl %edi
	popl %esi
	popl %ebp
	popl %ebx
	# 恢复FPU
	frstor (%esp)
   	addl $FPU_SIZE, %esp 
        ret
.endfunc

.globl switch_entry
.func switch_entry
switch_entry:
	# Discard switch_threads() arguments.
	addl $8, %esp

	# Call thread_switch_tail(prev).
	pushl %eax
.globl thread_switch_tail
	call thread_switch_tail
	addl $4, %esp

	fninit
	# Start thread proper.
	ret
.endfunc
