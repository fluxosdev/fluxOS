/*
 * The processor. fluxOS needs a Pentium III, or any other processor with
 * SSE (and the FXSAVE, CMOV and MMX that come with it): the whole system
 * is built for it. cpu_init() checks first thing at boot, and this file,
 * like everything else that runs before it, is built for any 386 (see the
 * Makefile), so an older machine says why it stops instead of hanging.
 *
 * It also reprograms the page attribute table, which every Pentium III
 * has, so that pages can be write-combining (pte_wc): the screen's owner
 * gets the framebuffer that way, and its writes, SSE's streaming stores
 * above all, reach the card in whole bursts instead of one at a time.
 */
#include "kernel.h"
#include "mm.h"
#include "x86.h"

#define EFLAGS_ID (1u << 21)

/* CPUID 1, EDX */
#define F_FPU  (1u << 0)
#define F_TSC  (1u << 4)
#define F_MSR  (1u << 5)
#define F_CMOV (1u << 15)
#define F_PAT  (1u << 16)
#define F_MMX  (1u << 23)
#define F_FXSR (1u << 24)
#define F_SSE  (1u << 25)

#define MSR_PAT 0x277
/*
 * PAT entries 0-3, and 4-7 alike (the PAT bit of a PTE is unreliable on
 * the Pentium III): write-back, write-combining, uncached-minus, uncached.
 * Only entry 1 changes from the reset value (write-through), so PWT alone
 * now means write-combining; PCD | PWT stays uncached for registers.
 */
#define PAT_VALUE 0x0007010600070106ull

uint32_t pte_wc;

static char name[49];

static int have_cpuid(void)
{
    uint32_t before = read_eflags(), after;
    __asm__ volatile("pushl %1; popfl; pushfl; popl %0" : "=r"(after) : "r"(before ^ EFLAGS_ID) : "cc");
    __asm__ volatile("pushl %0; popfl" : : "r"(before) : "cc");
    return ((before ^ after) & EFLAGS_ID) != 0;
}

/* The brand string where there is one (from the Pentium 4 and Athlon on),
 * or else the vendor and model. */
static void read_name(uint32_t sig)
{
    uint32_t r[4], ext[12];
    cpuid(0x80000000, r);
    if (r[0] >= 0x80000004) {
        for (uint32_t i = 0; i < 3; i++)
            cpuid(0x80000002 + i, &ext[i * 4]);
        memcpy(name, ext, 48);
        name[48] = 0;
        char *p = name;
        while (*p == ' ')
            p++;
        if (*p) {
            memmove(name, p, strlen(p) + 1);
            return;
        }
    }
    char vendor[13];
    cpuid(0, r);
    memcpy(vendor, &r[1], 4);
    memcpy(vendor + 4, &r[3], 4);
    memcpy(vendor + 8, &r[2], 4);
    vendor[12] = 0;
    uint32_t family = (sig >> 8) & 15, model = (sig >> 4) & 15;
    if (!strcmp(vendor, "GenuineIntel") && family == 6 && model >= 7 && model <= 11)
        strlcpy(name, "Intel Pentium III", sizeof(name));
    else
        snprintf(name, sizeof(name), "%s family %u model %u", vendor, family, model);
}

static void cannot_run(const char *what, const char *missing)
{
    kprintf("\n\033[1;31mfluxOS needs a Pentium III or a newer processor.\033[0m\n"
            "This one (%s) has no %s.\n\nSystem halted.\n", what, missing);
    cli();
    for (;;)
        hlt();
}

void cpu_init(void)
{
    if (!have_cpuid())
        cannot_run("a 386 or 486", "CPUID instruction");
    uint32_t r[4];
    cpuid(0, r);
    uint32_t max = r[0], sig = 0, edx = 0;
    if (max >= 1) {
        cpuid(1, r);
        sig = r[0];
        edx = r[3];
    }
    read_name(sig);

    static const struct { uint32_t bit; const char *what; } need[] = {
        { F_FPU, "floating-point unit" },
        { F_TSC, "time stamp counter" },
        { F_CMOV, "CMOV instruction (it is older than a Pentium Pro)" },
        { F_MMX, "MMX" },
        { F_FXSR, "FXSAVE instruction" },
        { F_SSE, "SSE" },
    };
    for (uint32_t i = 0; i < ARRAY_SIZE(need); i++)
        if (!(edx & need[i].bit))
            cannot_run(name, need[i].what);

    if ((edx & (F_PAT | F_MSR)) == (F_PAT | F_MSR) && !cmdline_has("pat=off")) {
        wrmsr(MSR_PAT, PAT_VALUE);
        pte_wc = PTE_PWT;
    }
    boot_ok("Processor: %s%s", name, pte_wc ? ", write-combining framebuffer" : "");
}
