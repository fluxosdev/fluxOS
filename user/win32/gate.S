/*
 * The gate between the loader (flat segments) and the program (segments
 * based at WIN_SEG_BASE: see win32.h).
 *
 * The program calls an imported function through a thunk in its own
 * memory (api.c writes them):
 *     push id ; call gate ; ret n
 * and the gate there is
 *     lcall flat_cs:api_gate_entry ; ret 4
 * so api_gate_entry starts in the flat code segment with the program's
 * stack, data and extra segments still loaded. It switches them, calls
 * api_dispatch with the arguments where the program pushed them, puts
 * the result where a Windows function would (eax, edx, or st(0)) and
 * far-returns. call_guest_on goes the other way for callbacks: it pushes
 * the arguments, a return address in the program's memory (an ljmp back
 * to call_guest_back) and the function's far address, switches segments
 * and far-returns into the program.
 */
#define WIN_BASE 0x40000000

    .text

    .global api_gate_entry
api_gate_entry:
    movl %esp, %edx
    addl $WIN_BASE, %edx
    movw %cs:gate_flat_ds, %cx
    movw %cx, %ss
    movl %edx, %esp                 /* the same stack, flat */
    movw %cx, %ds
    movw %cx, %es
    /* The stack now: 0 eip, 4 cs, 8 return into the thunk, 12 id,
     * 16 the program's return address, 20 its arguments. */
    leal 20(%esp), %eax
    subl $WIN_BASE, %eax
    pushl %eax                      /* struct call_frame.args (guest address) */
    pushl 20(%esp)                  /* .ret_addr */
    pushl %ebx
    pushl %esi
    pushl %edi
    pushl %ebp                      /* .ebp: the frame starts here */
    movl %esp, %eax
    leal 44(%esp), %ecx
    pushl %eax                      /* regs */
    pushl %ecx                      /* args */
    pushl 44(%esp)                  /* id */
    call api_dispatch
    addl $12, %esp
    popl %ebp
    popl %edi
    popl %esi
    popl %ebx
    addl $8, %esp
    movl api_edx, %edx
    movl api_ecx, %ecx
    cmpl $0, api_ebx_valid
    je 2f
    movl api_ebx, %ebx
2:  cmpl $0, api_fret_valid
    je 1f
    fldl api_fret
    movl $0, api_fret_valid
1:  movw gate_win_ds, %cx
    movw %cx, %ss
    leal -WIN_BASE(%esp), %esp
    movw %cx, %ds
    movw %cx, %es
    lret

/* uint32_t call_guest_on(gp_t fn, int nargs, const uint32_t *args, uint32_t new_stack) */
    .global call_guest_on
call_guest_on:
    pushl %ebp
    movl %esp, %ebp
    pushl %ebx
    pushl %esi
    pushl %edi
    movl 20(%ebp), %eax
    testl %eax, %eax
    jz 1f
    movl %eax, %esp                 /* the program's stack, for the first entry */
1:  movl 12(%ebp), %ecx
    movl 16(%ebp), %esi
2:  testl %ecx, %ecx
    jz 3f
    decl %ecx
    pushl (%esi,%ecx,4)
    jmp 2b
3:  pushl gate_return_guest         /* where the program's ret goes: the ljmp below */
    movzwl gate_win_cs, %eax
    pushl %eax
    pushl 8(%ebp)
    movw gate_win_ds, %cx
    movw %cx, %ds
    movw %cx, %es
    movw %cx, %ss
    leal -WIN_BASE(%esp), %esp
    lret

    .global call_guest_back
call_guest_back:
    movw %cs:gate_flat_ds, %cx
    movw %cx, %ss
    leal -12(%ebp), %esp
    movw %cx, %ds
    movw %cx, %es
    popl %edi
    popl %esi
    popl %ebx
    popl %ebp
    ret

/* void guest_resume(const struct guest_regs *r): go on in the program with
 * these registers (edi, esi, ebp, esp, ebx, edx, ecx, eax, eip, eflags). */
    .global guest_resume
guest_resume:
    movl 4(%esp), %edx
    movw gate_win_ds, %cx
    movzwl gate_win_cs, %eax
    movw %cx, %ss
    movl 12(%edx), %esp
    pushl 36(%edx)
    pushl %eax
    pushl 32(%edx)
    movl 0(%edx), %edi
    movl 4(%edx), %esi
    movl 8(%edx), %ebp
    movl 16(%edx), %ebx
    movl 24(%edx), %ecx
    movl 28(%edx), %eax
    pushl 20(%edx)
    movw gate_win_ds, %dx
    movw %dx, %ds
    movw %dx, %es
    popl %edx
    iret

/* void gate_load_fs(void): the program's FS (the thread block). */
    .global gate_load_fs
gate_load_fs:
    movw gate_win_fs, %ax
    movw %ax, %fs
    ret

/* uint32_t gate_read_fs(void): what FS holds now. */
    .global gate_read_fs
gate_read_fs:
    xorl %eax, %eax
    movw %fs, %ax
    ret

    .data
    .global gate_win_cs, gate_win_ds, gate_win_fs, gate_flat_cs, gate_flat_ds, gate_return_guest
gate_win_cs:   .word 0
gate_win_ds:   .word 0
gate_win_fs:   .word 0
gate_flat_cs:  .word 0
gate_flat_ds:  .word 0
    .align 4
gate_return_guest: .long 0

    .section .note.GNU-stack, "", @progbits
