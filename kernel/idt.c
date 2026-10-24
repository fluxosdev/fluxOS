/*
 * Interrupt descriptor table, 8259 PIC setup and the central trap
 * dispatcher that every exception, IRQ and system call goes through.
 */
#include "kernel.h"
#include "arch.h"
#include "x86.h"
#include "proc.h"
#include "syscall.h"

#define PIC1_CMD  0x20
#define PIC1_DATA 0x21
#define PIC2_CMD  0xA0
#define PIC2_DATA 0xA1
#define PIC_EOI   0x20

struct idt_entry {
    uint16_t base_low;
    uint16_t sel;
    uint8_t zero;
    uint8_t flags;
    uint16_t base_high;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

static struct idt_entry idt[256];
static struct idt_ptr idt_desc;
/* PCI devices share interrupt lines: each line has a few handlers, all
 * called on every interrupt, and each checks its own device. */
#define IRQ_SHARED 4
static irq_handler_t irq_handlers[16][IRQ_SHARED];

volatile int need_resched;

extern uint32_t isr_stub_table[48];
extern void isr128(void);
void idt_load(struct idt_ptr *p);

static const char *exception_names[32] = {
    "Divide error", "Debug", "NMI", "Breakpoint", "Overflow",
    "Bound range exceeded", "Invalid opcode", "Device not available",
    "Double fault", "Coprocessor segment overrun", "Invalid TSS",
    "Segment not present", "Stack-segment fault", "General protection fault",
    "Page fault", "Reserved", "x87 floating-point exception",
    "Alignment check", "Machine check", "SIMD floating-point exception",
    "Virtualization exception", "Control protection exception",
    "Reserved", "Reserved", "Reserved", "Reserved", "Reserved", "Reserved",
    "Hypervisor injection", "VMM communication", "Security exception",
    "Reserved",
};

static void idt_set(int vec, uint32_t handler, uint8_t flags)
{
    idt[vec].base_low = handler & 0xFFFF;
    idt[vec].base_high = (handler >> 16) & 0xFFFF;
    idt[vec].sel = SEG_KCODE;
    idt[vec].zero = 0;
    idt[vec].flags = flags;
}

/* Move the PIC vectors off the CPU exception range (to 32-47). */
static void pic_remap(void)
{
    outb(PIC1_CMD, 0x11); io_wait();
    outb(PIC2_CMD, 0x11); io_wait();
    outb(PIC1_DATA, IRQ_BASE); io_wait();
    outb(PIC2_DATA, IRQ_BASE + 8); io_wait();
    outb(PIC1_DATA, 4); io_wait();          /* slave on IRQ2 */
    outb(PIC2_DATA, 2); io_wait();
    outb(PIC1_DATA, 0x01); io_wait();       /* 8086 mode */
    outb(PIC2_DATA, 0x01); io_wait();
    outb(PIC1_DATA, 0xFB);                  /* everything masked but cascade */
    outb(PIC2_DATA, 0xFF);
}

void irq_unmask(int irq)
{
    uint16_t port = irq < 8 ? PIC1_DATA : PIC2_DATA;
    uint8_t bit = (uint8_t)(1u << (irq & 7));
    outb(port, inb(port) & (uint8_t)~bit);
}

int irq_registered(int irq)
{
    return irq_handlers[irq][0] != NULL;
}

void irq_register(int irq, irq_handler_t handler)
{
    for (int i = 0; i < IRQ_SHARED; i++) {
        if (irq_handlers[irq][i] == handler)
            break;
        if (!irq_handlers[irq][i]) {
            irq_handlers[irq][i] = handler;
            break;
        }
    }
    irq_unmask(irq);
}

void idt_init(void)
{
    memset(idt, 0, sizeof(idt));
    pic_remap();
    /* 0x8E: present, ring 0, 32-bit interrupt gate (IF cleared on entry) */
    for (int i = 0; i < 48; i++)
        idt_set(i, isr_stub_table[i], 0x8E);
    /* 0xEE: same, but callable from ring 3 */
    idt_set(SYSCALL_VEC, (uint32_t)isr128, 0xEE);

    idt_desc.limit = sizeof(idt) - 1;
    idt_desc.base = (uint32_t)&idt;
    idt_load(&idt_desc);
}

static void dump_frame(struct trapframe *tf)
{
    kprintf("  eax=%08x ebx=%08x ecx=%08x edx=%08x\n", tf->eax, tf->ebx, tf->ecx, tf->edx);
    kprintf("  esi=%08x edi=%08x ebp=%08x eip=%08x\n", tf->esi, tf->edi, tf->ebp, tf->eip);
    kprintf("  cs=%04x ds=%04x eflags=%08x err=%08x cr2=%08x\n",
            tf->cs, tf->ds, tf->eflags, tf->err_code, read_cr2());
}

static int exception_signal(uint32_t vec)
{
    switch (vec) {
    case 0: case 16: case 19: return SIGFPE;
    case 3: return SIGTRAP;
    case 6: return SIGILL;
    case 17: return SIGBUS;
    default: return SIGSEGV;
    }
}

static void handle_exception(struct trapframe *tf, int from_user)
{
    uint32_t vec = tf->int_no;

    if (from_user) {
        if (vec == 14 && proc_grow_stack(read_cr2()))
            return;
        if (vec == 7) {                 /* first FPU use since a task switch */
            fpu_trap();
            return;
        }
        klog("pid %d (%s): %s at eip %08x, addr %08x, err %x\n",
             current->pid, current->name, exception_names[vec], tf->eip,
             read_cr2(), tf->err_code);
        proc_signal_force(current, exception_signal(vec));
        return;
    }

    kprintf("\n\033[1;37;41m KERNEL PANIC \033[0m %s (vector %u) in kernel mode\n",
            exception_names[vec], vec);
    dump_frame(tf);
    panic("unrecoverable exception");
}

static void handle_irq(struct trapframe *tf)
{
    int irq = (int)tf->int_no - IRQ_BASE;

    /* Spurious IRQ7/IRQ15: the in-service bit is not set. */
    if (irq == 7 || irq == 15) {
        uint16_t cmd = irq == 7 ? PIC1_CMD : PIC2_CMD;
        outb(cmd, 0x0B);
        if (!(inb(cmd) & 0x80)) {
            if (irq == 15)
                outb(PIC1_CMD, PIC_EOI);
            return;
        }
    }

    random_add((uint32_t)irq);
    for (int i = 0; i < IRQ_SHARED && irq_handlers[irq][i]; i++)
        irq_handlers[irq][i](tf);

    if (irq >= 8)
        outb(PIC2_CMD, PIC_EOI);
    outb(PIC1_CMD, PIC_EOI);
}

void trap(struct trapframe *tf)
{
    int from_user = (tf->cs & 3) == 3;

    if (from_user)
        current->tf = tf;

    if (tf->int_no == SYSCALL_VEC)
        syscall_dispatch(tf);
    else if (tf->int_no < 32)
        handle_exception(tf, from_user);
    else if (tf->int_no < 48)
        handle_irq(tf);

    if (need_resched) {
        need_resched = 0;
        schedule();
    }

    /* Deliver pending signals right before returning to user mode. */
    if (from_user)
        proc_check_signals();
}
