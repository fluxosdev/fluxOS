/*
 * Global descriptor table: flat kernel and user segments plus a TSS for
 * each processor, used to find the kernel stack when an interrupt arrives
 * from ring 3 (and by this_cpu() to tell the processors apart).
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
    uint32_t base;
} __attribute__((packed));

struct tss {
    uint32_t prev_tss;
    uint32_t esp0, ss0;
    uint32_t esp1, ss1;
    uint32_t esp2, ss2;
    uint32_t cr3, eip, eflags;
    uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

static struct gdt_entry gdt[5 + MAX_CPUS];
static struct gdt_ptr gdt_desc;
static struct tss tss[MAX_CPUS];

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
    gdt_set(1, 0, 0xFFFFF, 0x9A, 0xCF);     /* kernel code, ring 0 */
    gdt_set(2, 0, 0xFFFFF, 0x92, 0xCF);     /* kernel data, ring 0 */
    gdt_set(3, 0, 0xFFFFF, 0xFA, 0xCF);     /* user code, ring 3 */
    gdt_set(4, 0, 0xFFFFF, 0xF2, 0xCF);     /* user data, ring 3 */

    memset(tss, 0, sizeof(tss));
    for (int i = 0; i < MAX_CPUS; i++) {
        tss[i].ss0 = SEG_KDATA;
        tss[i].iomap_base = sizeof(tss[i]);     /* no I/O permission bitmap */
        gdt_set(5 + i, (uint32_t)&tss[i], sizeof(tss[i]) - 1, 0x89, 0x00);
    }

    gdt_desc.limit = sizeof(gdt) - 1;
    gdt_desc.base = (uint32_t)&gdt;
    gdt_flush(&gdt_desc);
    tss_flush(SEG_TSS);
}

void gdt_init_ap(int cpu)
{
    gdt_flush(&gdt_desc);
    tss_flush(SEG_TSS + 8 * (uint32_t)cpu);
}

void tss_set_kernel_stack(uint32_t esp0)
{
    tss[this_cpu()->index].esp0 = esp0;
}
