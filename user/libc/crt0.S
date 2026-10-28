/*
 * Process entry point. The kernel leaves the stack as:
 *   (%esp) argc, 4(%esp) argv[0] ... NULL, envp[0] ... NULL
 */
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
    movl 8(%esp), %ebx
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

.section .note.GNU-stack, "", @progbits
