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
 *
 * Arguments arrive in %rdi, %rsi and %rdx (and stay there: nothing here
 * is called); %rax, %rcx, %r8 and %r9 are free.
 */
#define CR0_TS 8

.macro SSE_BEGIN
    pushq %rbp
    movq %rsp, %rbp
    pushfq
    cli
    movq %cr0, %rax
    pushq %rax
    clts
    andq $-16, %rsp
    subq $64, %rsp
    movaps %xmm0, (%rsp)
    movaps %xmm1, 16(%rsp)
    movaps %xmm2, 32(%rsp)
    movaps %xmm3, 48(%rsp)
.endm

.macro SSE_END
    movaps (%rsp), %xmm0
    movaps 16(%rsp), %xmm1
    movaps 32(%rsp), %xmm2
    movaps 48(%rsp), %xmm3
    leaq -16(%rbp), %rsp
    popq %rax
    testl $CR0_TS, %eax
    jz 9f
    movq %rax, %cr0                 /* TS was set: set it again */
9:  popfq
    popq %rbp
    ret
.endm

.text

/* void sse_zero_page(void *page): ordinary stores, as the page is about to be used. */
.globl sse_zero_page
.type sse_zero_page, @function
sse_zero_page:
    SSE_BEGIN
    xorps %xmm0, %xmm0
    movl $4096 / 64, %ecx
1:  movaps %xmm0, (%rdi)
    movaps %xmm0, 16(%rdi)
    movaps %xmm0, 32(%rdi)
    movaps %xmm0, 48(%rdi)
    addq $64, %rdi
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
    movl $4096 / 64, %ecx
1:  prefetchnta 256(%rsi)
    movaps (%rsi), %xmm0
    movaps 16(%rsi), %xmm1
    movaps 32(%rsi), %xmm2
    movaps 48(%rsi), %xmm3
    movntps %xmm0, (%rdi)
    movntps %xmm1, 16(%rdi)
    movntps %xmm2, 32(%rdi)
    movntps %xmm3, 48(%rdi)
    addq $64, %rsi
    addq $64, %rdi
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
    movq %rdx, %r8                  /* bytes left */
    cmpq $64, %r8
    jb 4f
    movq %rdi, %rcx                 /* bytes up to a 16-byte boundary of dst */
    negq %rcx
    andq $15, %rcx
    subq %rcx, %r8
    rep movsb
    movq %r8, %rdx
    shrq $6, %rdx                   /* 64-byte blocks */
    andq $63, %r8
    testq %rdx, %rdx
    jz 4f
    testq $15, %rsi
    jnz 2f
1:  movaps (%rsi), %xmm0
    movaps 16(%rsi), %xmm1
    movaps 32(%rsi), %xmm2
    movaps 48(%rsi), %xmm3
    movaps %xmm0, (%rdi)
    movaps %xmm1, 16(%rdi)
    movaps %xmm2, 32(%rdi)
    movaps %xmm3, 48(%rdi)
    addq $64, %rsi
    addq $64, %rdi
    decq %rdx
    jnz 1b
    jmp 4f
2:  movups (%rsi), %xmm0
    movups 16(%rsi), %xmm1
    movups 32(%rsi), %xmm2
    movups 48(%rsi), %xmm3
    movaps %xmm0, (%rdi)
    movaps %xmm1, 16(%rdi)
    movaps %xmm2, 32(%rdi)
    movaps %xmm3, 48(%rdi)
    addq $64, %rsi
    addq $64, %rdi
    decq %rdx
    jnz 2b
4:  movq %r8, %rcx                  /* the rest, eight bytes at a time */
    shrq $3, %rcx
    rep movsq
    movq %r8, %rcx
    andq $7, %rcx
    rep movsb
    SSE_END
.size sse_copy, . - sse_copy

/* void sse_fill(void *dst, uint32_t pattern, size_t n): dst and n multiples of 4. */
.globl sse_fill
.type sse_fill, @function
sse_fill:
    SSE_BEGIN
    movl %esi, %eax
    movq %rdx, %r8
    shrq $2, %r8                    /* in words from here on */
1:  testq $15, %rdi                 /* words up to a 16-byte boundary */
    jz 2f
    testq %r8, %r8
    jz 5f
    stosl
    decq %r8
    jmp 1b
2:  movd %eax, %xmm0                /* the pattern in all four lanes of xmm0 */
    shufps $0, %xmm0, %xmm0
    movq %r8, %rdx
    shrq $4, %rdx                   /* 64-byte blocks */
    jz 4f
3:  movaps %xmm0, (%rdi)
    movaps %xmm0, 16(%rdi)
    movaps %xmm0, 32(%rdi)
    movaps %xmm0, 48(%rdi)
    addq $64, %rdi
    decq %rdx
    jnz 3b
4:  movq %r8, %rcx
    andq $15, %rcx
    rep stosl
5:  SSE_END
.size sse_fill, . - sse_fill

.section .note.GNU-stack, "", @progbits
