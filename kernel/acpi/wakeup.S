/*
 * Where the firmware starts the machine when it wakes from suspend to
 * RAM. sleep.c copies this below 1 MiB, to WAKE_BASE, and gives that
 * address to the firmware as the FACS waking vector; the firmware comes
 * here in real mode, much as at power-on, and everything of ours in
 * memory is as we left it.
 *
 * WAKE_BASE is 512 KiB: above the first 64 KiB, which some firmware
 * writes over while it resumes (Linux keeps out of them for that reason),
 * and below any extended BIOS data area. The segment holds the code, a
 * buffer for what the VESA BIOS reports (WAKE_BUF) and a stack below
 * WAKE_STACK: a video BIOS's initialisation wants a deep one.
 *
 * In real mode, while the BIOS can still be called, the display gets
 * what it needs to show something again (the flags sleep.c fills in):
 * the video BIOS run again from its start (the firmware may not have,
 * and nothing else will), the boot loader's graphics mode set again, or
 * text mode 3. The boot loader does not say which VESA mode it set, only
 * its size, so the mode is found the way the loader found it: the first
 * of the BIOS's list with that size, colour depth and a linear
 * framebuffer. A beep, on request, shows that the firmware got this far.
 *
 * Then A20 on, a flat GDT of our own, protected mode, and from there long
 * mode, on vmm_boot_cr3()'s page tables (the kernel's, with the first
 * 2 MiB mapped one to one as well, where this code is), and
 * context_restore() with the state sleep.c saved, which loads the
 * kernel's own tables again and goes on in acpi_suspend() where it went
 * to sleep. sleep.c fills in the words at the end.
 */
#define WAKE_BASE  0x80000
#define WAKE_STACK 0x8000               /* the stack's top: from WAKE_BUF's end up */
#define WAKE_BUF   0x1000               /* the VESA BIOS's answers: 512 + 256 bytes */
#define W(x) ((x) - wakeup_start)

#define WF_POST  0x1                    /* run the video BIOS's start */
#define WF_VBE   0x2                    /* set the graphics mode of wakeup_vbe_* again */
#define WF_TEXT  0x4                    /* set text mode 3 */
#define WF_BEEP  0x8

#define CR4_PAE   0x20
#define MSR_EFER  0xC0000080
#define CR0_PG_WP 0x80010000

/* The segment registers and stack as this code wants them: a BIOS call may
 * have left anything. */
.macro OUR_SEGMENTS
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $WAKE_STACK, %esp
    cli
    cld
.endm

/* The same within a subroutine: the stack stays where it is. */
.macro OUR_SEGMENTS_KEEP_STACK
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    cli
    cld
.endm

.section .text
.align 16
.code16
.global wakeup_start
wakeup_start:
    cli
    cld
    ljmp $(WAKE_BASE >> 4), $W(1f)      /* CS:IP as we expect, whichever the firmware used */
1:  OUR_SEGMENTS
    /* Firmware need not leave IDTR or the high half of ESP usable. */
    lidtl W(wakeup_idt_desc)

    testb $WF_BEEP, W(wakeup_flags)
    jz 2f
    movb $0xB6, %al                     /* PIT channel 2, square wave */
    outb %al, $0x43
    movb $0xA9, %al                     /* 1193182 / 0x04A9: about 1 kHz */
    outb %al, $0x42
    movb $0x04, %al
    outb %al, $0x42
    inb $0x61, %al
    orb $0x03, %al                      /* the speaker on */
    outb %al, $0x61
    movl $0x40000, %ecx                 /* a quarter second: port 80h takes ~1 us */
9:  outb %al, $0x80
    loopl 9b
    inb $0x61, %al
    andb $0xFC, %al
    outb %al, $0x61

2:  testb $WF_POST, W(wakeup_flags)
    jz 3f
    movw $0xC000, %ax                   /* only with a ROM there (55 AA) */
    movw %ax, %es
    cmpw $0xAA55, %es:0
    jne 3f
    movw W(wakeup_vga_bdf), %ax         /* bus:device.function, as the PCI firmware spec has it */
    lcallw $0xC000, $3                  /* the option ROM's initialisation entry */
    OUR_SEGMENTS

3:  testb $WF_VBE, W(wakeup_flags)
    jz 4f
    call vbe_find                       /* BX: the mode, or 0 */
    testw %bx, %bx
    jz 4f
    orw $0x4000, %bx                    /* with the linear framebuffer */
    movw $0x4F02, %ax
    int $0x10
    OUR_SEGMENTS

4:  testb $WF_TEXT, W(wakeup_flags)
    jz 5f
    movw $0x0003, %ax
    int $0x10
    OUR_SEGMENTS

5:  inb $0x92, %al                      /* A20 on (and not bit 0: that resets) */
    orb $0x02, %al
    andb $0xFE, %al
    outb %al, $0x92
    lgdtl W(wakeup_gdt_desc)
    movl %cr0, %eax
    andl $0x9FFFFFFF, %eax              /* caches on: clear CD and NW */
    orl $1, %eax                        /* PE */
    movl %eax, %cr0
    ljmpl $0x08, $(WAKE_BASE + W(wake32))

/*
 * The VESA mode whose size is wakeup_vbe_width x wakeup_vbe_height at
 * wakeup_vbe_bpp bits a pixel, in BX, or 0 if the BIOS has none (or is not
 * a VESA one). The controller information goes to WAKE_BUF and each mode's
 * to WAKE_BUF + 512; the mode list may be in the former, so it is kept.
 */
vbe_find:
    movw $WAKE_BUF, %di                 /* ES:DI, in this segment */
    movl $0x32454256, (%di)             /* "VBE2": the full block, please */
    movw $0x4F00, %ax
    int $0x10
    cmpw $0x004F, %ax
    jne 8f
    OUR_SEGMENTS_KEEP_STACK
    movw WAKE_BUF + 14, %si             /* the mode list: a far pointer */
7:  movw WAKE_BUF + 16, %ax
    movw %ax, %fs
    movw %fs:(%si), %cx
    addw $2, %si
    cmpw $0xFFFF, %cx                   /* the list's end */
    je 8f
    pushw %si
    pushw %cx
    movw $(WAKE_BUF + 512), %di
    movw $0x4F01, %ax
    int $0x10
    movw %ax, %dx
    OUR_SEGMENTS_KEEP_STACK
    popw %cx
    popw %si
    cmpw $0x004F, %dx
    jne 7b
    movw WAKE_BUF + 512, %ax            /* attributes: supported, graphics, linear framebuffer */
    andw $0x0091, %ax
    cmpw $0x0091, %ax
    jne 7b
    movw WAKE_BUF + 512 + 18, %ax
    cmpw W(wakeup_vbe_width), %ax
    jne 7b
    movw WAKE_BUF + 512 + 20, %ax
    cmpw W(wakeup_vbe_height), %ax
    jne 7b
    movb WAKE_BUF + 512 + 25, %al
    cmpb W(wakeup_vbe_bpp), %al
    jne 7b
    movw %cx, %bx
    ret
8:  OUR_SEGMENTS_KEEP_STACK
    xorw %bx, %bx
    ret

.code32
wake32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $(WAKE_BASE + WAKE_STACK), %esp
    movl %cr4, %eax
    orl $CR4_PAE, %eax
    movl %eax, %cr4
    movl (WAKE_BASE + W(wakeup_cr3)), %eax
    movl %eax, %cr3
    movl $MSR_EFER, %ecx
    rdmsr
    orl (WAKE_BASE + W(wakeup_efer)), %eax      /* long mode (and what else it had on) */
    wrmsr
    movl %cr0, %eax
    orl $CR0_PG_WP, %eax                /* paging on: long mode is active */
    movl %eax, %cr0
    ljmpl $0x18, $(WAKE_BASE + W(wake64))

.code64
wake64:
    xorl %eax, %eax
    movw %ax, %fs
    movw %ax, %gs
    movq (WAKE_BASE + W(wakeup_context)), %rdi  /* the state sleep.c saved */
    movq (WAKE_BASE + W(wakeup_entry)), %rax    /* context_restore(), up in the kernel */
    call *%rax
6:  cli
    hlt
    jmp 6b

.align 8
wakeup_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* 0x08 code: flat, ring 0, 32-bit */
    .quad 0x00CF92000000FFFF            /* 0x10 data: flat, ring 0 */
    .quad 0x00AF9A000000FFFF            /* 0x18 code: ring 0, 64-bit */
wakeup_idt_desc:
    .word 0x3FF
    .long 0
wakeup_gdt_desc:
    .word 4 * 8 - 1
    .long WAKE_BASE + W(wakeup_gdt)

.align 8
.global wakeup_cr3, wakeup_efer, wakeup_context, wakeup_entry
wakeup_cr3:
    .long 0
wakeup_efer:
    .long 0
wakeup_context:
    .quad 0
wakeup_entry:
    .quad 0
.global wakeup_flags, wakeup_vga_bdf, wakeup_vbe_width, wakeup_vbe_height, wakeup_vbe_bpp
wakeup_vga_bdf:
    .word 0
wakeup_vbe_width:
    .word 0
wakeup_vbe_height:
    .word 0
wakeup_vbe_bpp:
    .byte 0
wakeup_flags:
    .byte 0

.global wakeup_end
wakeup_end:

.section .note.GNU-stack, "", @progbits
