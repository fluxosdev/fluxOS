/*
 * Process entry point, and what else of the C library is assembly: the way
 * back from a signal handler, and setjmp. Twice: for a 64-bit program, and
 * for a 32-bit one (the Windows program loader is built so), for which the
 * kernel leaves the stack as:
 *   (%esp) argc, 4(%esp) argv[0] ... NULL, envp[0] ... NULL
 */
#ifdef __x86_64__

/*
 * A 64-bit program: the stack's words are 8 bytes, the stack pointer is
 * 16-byte aligned, and arguments go in registers.
 *   (%rsp) argc, 8(%rsp) argv[0] ... NULL, envp[0] ... NULL
 */
.section .text.start, "ax"
.global _start
.type _start, @function
_start:
    xorl %ebp, %ebp
    movq (%rsp), %rdi               /* argc */
    leaq 8(%rsp), %rsi              /* argv */
    leaq 16(%rsp,%rdi,8), %rdx      /* envp = &argv[argc + 1] */
    andq $-16, %rsp
    call __libc_start
1:  jmp 1b
.size _start, . - _start

.section .text, "ax"

/* Where a signal handler returns to: the kernel left the context's
 * address above the handler's return address. */
.global __sigrestorer
.type __sigrestorer, @function
__sigrestorer:
    movq (%rsp), %rbx
    movl $74, %eax                  /* SYS_sigreturn */
    int $0x80
    ud2
.size __sigrestorer, . - __sigrestorer

/* A new thread starts here (pthread.c), on its own stack: its block is
 * what FS is based at, and the block's first word its address. */
.global __thread_start
.type __thread_start, @function
__thread_start:
    xorl %ebp, %ebp
    movq %fs:0, %rdi
    call __thread_entry
    ud2
.size __thread_start, . - __thread_start

/* jmp_buf: rbx, rbp, r12, r13, r14, r15, rsp, rip, mask_saved, sigmask */
.global setjmp
.type setjmp, @function
setjmp:
    movq $1, 64(%rdi)
    jmp .Lsetjmp_common
.size setjmp, . - setjmp

.global _setjmp
.type _setjmp, @function
_setjmp:
    movq $0, 64(%rdi)
    jmp .Lsetjmp_common
.size _setjmp, . - _setjmp

.global sigsetjmp
.type sigsetjmp, @function
sigsetjmp:
    movslq %esi, %rsi
    movq %rsi, 64(%rdi)
.Lsetjmp_common:
    movq %rbx, 0(%rdi)
    movq %rbp, 8(%rdi)
    movq %r12, 16(%rdi)
    movq %r13, 24(%rdi)
    movq %r14, 32(%rdi)
    movq %r15, 40(%rdi)
    leaq 8(%rsp), %rax
    movq %rax, 48(%rdi)
    movq (%rsp), %rax
    movq %rax, 56(%rdi)
    cmpq $0, 64(%rdi)
    je 1f
    pushq %rdi                      /* (and the stack is aligned for the call) */
    call __sigjmp_save_mask
    popq %rdi
1:
    xorl %eax, %eax
    ret
.size sigsetjmp, . - sigsetjmp

.global longjmp
.global _longjmp
.global siglongjmp
.type longjmp, @function
.type _longjmp, @function
.type siglongjmp, @function
longjmp:
_longjmp:
siglongjmp:
    cmpq $0, 64(%rdi)
    je 1f
    pushq %rdi
    pushq %rsi
    subq $8, %rsp
    call __sigjmp_restore_mask
    addq $8, %rsp
    popq %rsi
    popq %rdi
1:
    movl %esi, %eax
    testl %eax, %eax
    jnz 2f
    movl $1, %eax
2:
    movq 0(%rdi), %rbx
    movq 8(%rdi), %rbp
    movq 16(%rdi), %r12
    movq 24(%rdi), %r13
    movq 32(%rdi), %r14
    movq 40(%rdi), %r15
    movq 48(%rdi), %rsp
    jmp *56(%rdi)
.size longjmp, . - longjmp

#else /* a 32-bit program */

.section .text.start, "ax"
.global _start
.type _start, @function
_start:
    xorl %ebp, %ebp
    movl (%esp), %eax               /* argc */
    leal 4(%esp), %ebx              /* argv */
    leal 8(%esp,%eax,4), %ecx       /* envp = &argv[argc + 1] */
    andl $-16, %esp
    subl $4, %esp
    pushl %ecx
    pushl %ebx
    pushl %eax
    call __libc_start
1:  jmp 1b
.size _start, . - _start

.section .text, "ax"

.global __sigrestorer
.type __sigrestorer, @function
__sigrestorer:
    movl 12(%esp), %ebx
    movl $74, %eax                  /* SYS_sigreturn */
    int $0x80
    ud2
.size __sigrestorer, . - __sigrestorer

.global setjmp
.type setjmp, @function
setjmp:
    movl 4(%esp), %eax
    movl $1, 24(%eax)
    jmp .Lsetjmp_common
.size setjmp, . - setjmp

.global _setjmp
.type _setjmp, @function
_setjmp:
    movl 4(%esp), %eax
    movl $0, 24(%eax)
    jmp .Lsetjmp_common
.size _setjmp, . - _setjmp

.global sigsetjmp
.type sigsetjmp, @function
sigsetjmp:
    movl 4(%esp), %eax
    movl 8(%esp), %ecx
    movl %ecx, 24(%eax)
.Lsetjmp_common:
    movl %ebx, 0(%eax)
    movl %esi, 4(%eax)
    movl %edi, 8(%eax)
    movl %ebp, 12(%eax)
    leal 4(%esp), %ecx
    movl %ecx, 16(%eax)
    movl 0(%esp), %ecx
    movl %ecx, 20(%eax)
    cmpl $0, 24(%eax)
    je 1f
    pushl %eax
    call __sigjmp_save_mask
    addl $4, %esp
1:
    xorl %eax, %eax
    ret
.size sigsetjmp, . - sigsetjmp

.global longjmp
.global _longjmp
.global siglongjmp
.type longjmp, @function
.type _longjmp, @function
.type siglongjmp, @function
longjmp:
_longjmp:
siglongjmp:
    movl 4(%esp), %edx
    cmpl $0, 24(%edx)
    je 1f
    pushl %edx
    call __sigjmp_restore_mask
    popl %edx
1:
    movl 8(%esp), %eax
    testl %eax, %eax
    jnz 2f
    movl $1, %eax
2:
    movl 0(%edx), %ebx
    movl 4(%edx), %esi
    movl 8(%edx), %edi
    movl 12(%edx), %ebp
    movl 16(%edx), %esp
    jmp *20(%edx)
.size longjmp, . - longjmp

#endif

.section .note.GNU-stack, "", @progbits
