/*
 * Where the firmware starts the machine when it wakes from suspend to
 * RAM. sleep.c copies this below 1 MiB, to WAKE_BASE, and gives that
 * address to the firmware as the FACS waking vector; the firmware comes
 * here in real mode, much as at power-on, and everything of ours in
 * memory is as we left it.
 *
 * WAKE_BASE is 512 KiB: above the first 64 KiB, which some firmware
 * writes over while it resumes (Linux keeps out of them for that reason),
 * and below any extended BIOS data area. The segment holds the code and
 * a 32 KiB stack: a video BIOS's initialisation wants a deep one.
 *
 * In real mode, while the BIOS can still be called, the display gets
 * what it needs to show something again (the flags sleep.c fills in):
 * the video BIOS run again from its start (the firmware may not have,
 * and nothing else will), the VESA mode the boot loader set, or text
 * mode 3. A beep, on request, shows that the firmware got this far. Then
 * A20 on, a flat GDT of our own, protected mode, and context_restore()
 * with the state sleep.c saved, which turns paging on again and goes on
 * in acpi_suspend() where it went to sleep.
 */
#define WAKE_BASE  0x80000
#define WAKE_STACK 0x8000               /* the stack's top: 32 KiB, from the code up */
#define W(x) ((x) - wakeup_start)

#define WF_POST  0x1                    /* run the video BIOS's start */
#define WF_VBE   0x2                    /* set VESA mode wakeup_vbe_mode */
#define WF_TEXT  0x4                    /* set text mode 3 */
#define WF_BEEP  0x8

.section .text
.align 16
.code16
.global wakeup_start
wakeup_start:
    cli
    cld
    ljmp $(WAKE_BASE >> 4), $W(1f)      /* CS:IP as we expect, whichever the firmware used */
1:  movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $WAKE_STACK, %esp

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
    movw %cs, %ax                       /* which may have used anything */
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $WAKE_STACK, %esp
    cli
    cld

3:  testb $WF_VBE, W(wakeup_flags)
    jz 4f
    movw $0x4F02, %ax
    movw W(wakeup_vbe_mode), %bx
    int $0x10
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $WAKE_STACK, %esp
    cli
    cld

4:  testb $WF_TEXT, W(wakeup_flags)
    jz 5f
    movw $0x0003, %ax
    int $0x10
    movw %cs, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movl $WAKE_STACK, %esp
    cli
    cld

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

.code32
wake32:
    movw $0x10, %ax
    movw %ax, %ds
    movw %ax, %es
    movw %ax, %ss
    movw %ax, %fs
    movw %ax, %gs
    movl $(WAKE_BASE + WAKE_STACK), %esp
    pushl $wake_context
    movl $context_restore, %eax         /* absolute: this code was moved */
    call *%eax
6:  cli
    hlt
    jmp 6b

.align 8
wakeup_gdt:
    .quad 0
    .quad 0x00CF9A000000FFFF            /* code: flat, ring 0 */
    .quad 0x00CF92000000FFFF            /* data: flat, ring 0 */
wakeup_idt_desc:
    .word 0x3FF
    .long 0
wakeup_gdt_desc:
    .word 3 * 8 - 1
    .long WAKE_BASE + W(wakeup_gdt)

.global wakeup_flags, wakeup_vbe_mode, wakeup_vga_bdf
wakeup_flags:
    .byte 0
.align 2
wakeup_vbe_mode:
    .word 0
wakeup_vga_bdf:
    .word 0

.global wakeup_end
wakeup_end:

.section .note.GNU-stack, "", @progbits
