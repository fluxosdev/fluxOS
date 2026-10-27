/*
 * Block moves on the SSE registers, for the kernel: clearing and copying
 * pages, and moving framebuffer lines 16 bytes at a time.
 *
 * The XMM registers belong to whichever process used them last (fpu.c
 * switches them lazily, so that need not be the current one). Each routine
 * therefore runs with interrupts off, clears CR0.TS while it works, keeps
 * xmm0-xmm3 on its stack and puts them and CR0 back before returning. The
 * moves and XORPS used here neither read nor change MXCSR or the x87.
 *
 * Only for use once fpu_init() has set CR4.OSFXSR (kernel_sse).
 */

#define CR0_TS 8

/* Arguments at 8(%ebp), 12(%ebp), 16(%ebp); %esi, %edi and %ebx are free. */
.macro SSE_BEGIN
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    pushfl
    cli
    movl %cr0, %eax
    pushl %eax
    clts
    andl $-16, %esp
    subl $64, %esp
    movaps %xmm0, (%esp)
    movaps %xmm1, 16(%esp)
    movaps %xmm2, 32(%esp)
    movaps %xmm3, 48(%esp)
.endm

.macro SSE_END
    movaps (%esp), %xmm0
    movaps 16(%esp), %xmm1
    movaps 32(%esp), %xmm2
    movaps 48(%esp), %xmm3
    leal -20(%ebp), %esp
    popl %eax
    testl $CR0_TS, %eax
    jz 9f
    movl %eax, %cr0                 /* TS was set: set it again */
9:  popfl
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret
.endm

.text

/* void sse_zero_page(void *page): ordinary stores, as the page is about to be used. */
.globl sse_zero_page
.type sse_zero_page, @function
sse_zero_page:
    SSE_BEGIN
    movl 8(%ebp), %edi
    xorps %xmm0, %xmm0
    movl $4096 / 64, %ecx
1:  movaps %xmm0, (%edi)
    movaps %xmm0, 16(%edi)
    movaps %xmm0, 32(%edi)
    movaps %xmm0, 48(%edi)
    addl $64, %edi
    decl %ecx
    jnz 1b
    SSE_END
.size sse_zero_page, . - sse_zero_page

/*
 * void sse_copy_page(void *dst, const void *src): fork's copies. The
 * stores stream past the caches, which the child, usually about to exec,
 * would only fill with pages it never reads.
 */
.globl sse_copy_page
.type sse_copy_page, @function
sse_copy_page:
    SSE_BEGIN
    movl 8(%ebp), %edi
    movl 12(%ebp), %esi
    movl $4096 / 64, %ecx
1:  prefetchnta 256(%esi)
    movaps (%esi), %xmm0
    movaps 16(%esi), %xmm1
    movaps 32(%esi), %xmm2
    movaps 48(%esi), %xmm3
    movntps %xmm0, (%edi)
    movntps %xmm1, 16(%edi)
    movntps %xmm2, 32(%edi)
    movntps %xmm3, 48(%edi)
    addl $64, %esi
    addl $64, %edi
    decl %ecx
    jnz 1b
    sfence
    SSE_END
.size sse_copy_page, . - sse_copy_page

/*
 * void sse_copy(void *dst, const void *src, size_t n): a forward copy for
 * framebuffer memory, where each access is a bus transaction and 16 bytes
 * at a time beats 4. Overlap is fine when dst < src (scrolling up): each
 * 64 bytes are all read before any of them are written.
 */
.globl sse_copy
.type sse_copy, @function
sse_copy:
    SSE_BEGIN
    movl 8(%ebp), %edi
    movl 12(%ebp), %esi
    movl 16(%ebp), %ebx
    cmpl $64, %ebx
    jb 4f
    movl %edi, %ecx                 /* bytes up to a 16-byte boundary of dst */
    negl %ecx
    andl $15, %ecx
    subl %ecx, %ebx
    rep movsb
    movl %ebx, %edx
    shrl $6, %edx                   /* 64-byte blocks */
    andl $63, %ebx
    testl %edx, %edx
    jz 4f
    testl $15, %esi
    jnz 2f
1:  movaps (%esi), %xmm0
    movaps 16(%esi), %xmm1
    movaps 32(%esi), %xmm2
    movaps 48(%esi), %xmm3
    movaps %xmm0, (%edi)
    movaps %xmm1, 16(%edi)
    movaps %xmm2, 32(%edi)
    movaps %xmm3, 48(%edi)
    addl $64, %esi
    addl $64, %edi
    decl %edx
    jnz 1b
    jmp 4f
2:  movups (%esi), %xmm0
    movups 16(%esi), %xmm1
    movups 32(%esi), %xmm2
    movups 48(%esi), %xmm3
    movaps %xmm0, (%edi)
    movaps %xmm1, 16(%edi)
    movaps %xmm2, 32(%edi)
    movaps %xmm3, 48(%edi)
    addl $64, %esi
    addl $64, %edi
    decl %edx
    jnz 2b
4:  movl %ebx, %ecx                 /* the rest, a word at a time */
    shrl $2, %ecx
    rep movsl
    movl %ebx, %ecx
    andl $3, %ecx
    rep movsb
    SSE_END
.size sse_copy, . - sse_copy

/* void sse_fill(void *dst, uint32_t pattern, size_t n): dst and n multiples of 4. */
.globl sse_fill
.type sse_fill, @function
sse_fill:
    SSE_BEGIN
    movl 8(%ebp), %edi
    movl 12(%ebp), %eax
    movl 16(%ebp), %ebx
    shrl $2, %ebx                   /* in words from here on */
1:  testl $15, %edi                 /* words up to a 16-byte boundary */
    jz 2f
    testl %ebx, %ebx
    jz 5f
    stosl
    decl %ebx
    jmp 1b
2:  subl $16, %esp                  /* the pattern in all four lanes of xmm0 */
    movl %eax, (%esp)
    movss (%esp), %xmm0
    addl $16, %esp
    shufps $0, %xmm0, %xmm0
    movl %ebx, %edx
    shrl $4, %edx                   /* 64-byte blocks */
    jz 4f
3:  movaps %xmm0, (%edi)
    movaps %xmm0, 16(%edi)
    movaps %xmm0, 32(%edi)
    movaps %xmm0, 48(%edi)
    addl $64, %edi
    decl %edx
    jnz 3b
4:  movl %ebx, %ecx
    andl $15, %ecx
    rep stosl
5:  SSE_END
.size sse_fill, . - sse_fill

.section .note.GNU-stack, "", @progbits
