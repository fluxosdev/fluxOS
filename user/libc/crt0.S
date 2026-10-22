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

.section .note.GNU-stack, "", @progbits
