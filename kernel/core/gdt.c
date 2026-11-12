/*
 * Global descriptor table: the kernel's 64-bit code segment, flat 32-bit
 * segments for user programs (which run in compatibility mode), the
 * segments a Win32 program runs in (based at the start of user space, so
 * that it sees its own addresses: winrun), plus a TSS for each processor,
 * which says where the kernel stack is when an interrupt arrives from ring
 * 3 (and lets this_cpu() tell the processors apart).
 *
 * Each processor has its own copy. The scheduler switches libc's GS TLS
 * base (SEG_USER_TLS) and Windows' FS TEB base (SEG_WIN_FS) independently;
 * segment registers are reloaded on the way back to user mode.
 *
 * The user segments' selectors are what they were on the 32-bit kernel
 * (programs have them built in), which left entries 8 to 23, once the
 * 32-bit TSSs, unused: the first is now the code segment of 64-bit
 * programs, and the 64-bit TSSs, two entries each, follow the TLS segment.
 */
#include "kernel.h"
#include "arch.h"
#include "smp.h"

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t base_mid;
    uint8_t access;
    uint8_t gran;
    uint8_t base_high;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

struct tss {
    uint32_t reserved0;
    uint64_t rsp0, rsp1, rsp2;          /* rsp0: the kernel stack of the process running here */
    uint64_t reserved1;
    uint64_t ist[7];                    /* stacks an interrupt gate may name (the double fault's) */
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iomap_base;
} __attribute__((packed));
_Static_assert(sizeof(struct tss) == 104, "the 64-bit TSS");

#define GDT_TLS 24                      /* where it always was: SEG_USER_TLS is in programs */
#define GDT_TSS (GDT_TLS + 1)           /* the first processor's TSS descriptor */
#define GDT_ENTRIES (GDT_TSS + 2 * MAX_CPUS)
_Static_assert(SEG_USER_TLS == (GDT_TLS << 3 | 3), "SEG_USER_TLS is the TLS entry");
_Static_assert(SEG_TSS == GDT_TSS << 3, "SEG_TSS is the first TSS entry");
static struct gdt_entry gdts[MAX_CPUS][GDT_ENTRIES];
static struct gdt_entry *const gdt = gdts[0];         /* the boot processor's, which the others copy */
static struct gdt_ptr gdt_desc[MAX_CPUS];
static struct tss tss[MAX_CPUS];
/* A double fault usually means the kernel stack is gone: it gets one of its own. */
static uint8_t df_stack[MAX_CPUS][4096] __attribute__((aligned(16)));

void gdt_flush(struct gdt_ptr *p);
void tss_flush(uint32_t sel);

static void gdt_set(int i, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran)
{
    gdt[i].base_low = base & 0xFFFF;
    gdt[i].base_mid = (base >> 16) & 0xFF;
    gdt[i].base_high = (base >> 24) & 0xFF;
    gdt[i].limit_low = limit & 0xFFFF;
    gdt[i].gran = (uint8_t)(((limit >> 16) & 0x0F) | (gran & 0xF0));
    gdt[i].access = access;
}

void gdt_init(void)
{
    gdt_set(0, 0, 0, 0, 0);
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xAF);     /* kernel code, ring 0, 64-bit (L) */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);     /* kernel data, ring 0 */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);     /* user code, ring 3, 32-bit */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);     /* user data, ring 3 */
    /* Win32 programs: 2 GiB from WIN_SEG_BASE, and the thread block */
    gdt_set(5, WIN_SEG_BASE, (WIN_SEG_LIMIT >> 12) - 1, 0xFA, 0xC0);
    gdt_set(6, WIN_SEG_BASE, (WIN_SEG_LIMIT >> 12) - 1, 0xF2, 0xC0);
    gdt_set(7, WIN_TEB_LINEAR, (WIN_TEB_SIZE >> 12) - 1, 0xF2, 0xC0);
    gdt_set(8, 0, 0xFFFFF, 0xFA, 0xAF);     /* user code, ring 3, 64-bit (L) */
    _Static_assert(SEG_UCODE64 == (8 << 3 | 3), "SEG_USER_CODE64 is entry 8");
    gdt_set(GDT_TLS, 0, 0xFFFFF, 0xF2, 0xCF);           /* user data until a thread sets its base */

    memset(tss, 0, sizeof(tss));
    for (int i = 0; i < MAX_CPUS; i++) {
        tss[i].ist[0] = (uint64_t)(uintptr_t)df_stack[i] + sizeof(df_stack[i]);
        tss[i].iomap_base = sizeof(tss[i]);     /* no I/O permission bitmap */
        /* A system descriptor is 16 bytes in long mode: the base's high half follows. */
        uint64_t base = (uint64_t)(uintptr_t)&tss[i];
        gdt_set(GDT_TSS + 2 * i, (uint32_t)base, sizeof(tss[i]) - 1, 0x89, 0x00);
        uint32_t high[2] = { (uint32_t)(base >> 32), 0 };
        memcpy(&gdt[GDT_TSS + 2 * i + 1], high, sizeof(high));
    }

    for (int i = 0; i < MAX_CPUS; i++) {
        if (i)
            memcpy(gdts[i], gdts[0], sizeof(gdts[0]));
        gdt_desc[i].limit = sizeof(gdts[i]) - 1;
        gdt_desc[i].base = (uint64_t)(uintptr_t)gdts[i];
    }
    gdt_flush(&gdt_desc[0]);
    tss_flush(SEG_TSS);
}

/* The TLS segment of this processor: based at the thread block of the thread about to run here. */
void gdt_set_tls(uint32_t base)
{
    struct gdt_entry *e = &gdts[this_cpu()->index][GDT_TLS];
    e->base_low = base & 0xFFFF;
    e->base_mid = (base >> 16) & 0xFF;
    e->base_high = (base >> 24) & 0xFF;
}

/* Windows uses FS independently of libc's GS TLS. Each CPU has its own descriptor. */
void gdt_set_win_teb(uint32_t base)
{
    struct gdt_entry *e=&gdts[this_cpu()->index][7];
    if(!base) base=WIN_TEB_LINEAR;
    e->base_low=base&0xFFFF;e->base_mid=(base>>16)&0xFF;e->base_high=base>>24;
}

/* Load a processor's TSS. LTR marks the descriptor busy and refuses one
 * already marked, as it still is after the processor was switched off
 * (suspend to RAM): it is made available again first. */
void gdt_load_tr(int cpu)
{
    gdts[cpu][GDT_TSS + 2 * cpu].access = 0x89;
    tss_flush(SEG_TSS + 16 * (uint32_t)cpu);
}

void gdt_init_ap(int cpu)
{
    gdt_flush(&gdt_desc[cpu]);
    gdt_load_tr(cpu);
}

void tss_set_kernel_stack(uint64_t rsp0)
{
    tss[this_cpu()->index].rsp0 = rsp0;
}
