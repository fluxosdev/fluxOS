/*
 * The processor. fluxOS needs a Pentium 4, or any other processor with
 * SSE2 (and the FXSAVE, CMOV, MMX and SSE that come with it): the whole
 * system is built for it. cpu_init() checks first thing at boot, and this file,
 * like everything else that runs before it, is built for any 386 (see the
 * Makefile), so an older machine says why it stops instead of hanging.
 *
 * It also reprograms the page attribute table, which every Pentium 4
 * has, so that pages can be write-combining (pte_wc): the screen's owner
 * gets the framebuffer that way, and its writes, SSE's streaming stores
 * above all, reach the card in whole bursts instead of one at a time.
 */
#include "kernel.h"
#include "drivers.h"
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
#define F_SSE2 (1u << 26)

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
    splash_end(1);
    kprintf("\n\033[1;31mfluxOS needs a Pentium 4 or a newer processor.\033[0m\n"
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
        { F_SSE2, "SSE2 (it is older than a Pentium 4)" },
    };
    for (uint32_t i = 0; i < ARRAY_SIZE(need); i++)
        if (!(edx & need[i].bit))
            cannot_run(name, need[i].what);

    if ((edx & (F_PAT | F_MSR)) == (F_PAT | F_MSR) && !cmdline_has("pat=off")) {
        wrmsr(MSR_PAT, PAT_VALUE);
        pte_wc = PTE_PWT;
    }
    boot_ok("Processor: %s%s", name, pte_wc ? ", write-combining framebuffer" : "");
    mtrr_save();
}

/* Another processor starting (smp.c): the page attribute table must be
 * the same on all of them. */
void cpu_init_ap(void)
{
    if (pte_wc)
        wrmsr(MSR_PAT, PAT_VALUE);
}

/*
 * The memory type range registers, across suspend to RAM. The firmware
 * set them at power-on and should set them again on waking, but not all
 * firmware does: left at their reset state they make every byte of memory
 * uncacheable, and the machine crawls as if hung. Linux puts them back
 * itself, and so do we: the boot processor's are kept at boot and written
 * to every processor that comes back (the SDM's procedure: caches off and
 * flushed, the MTRRs disabled while they change).
 */
#define CR0_NW (1u << 29)
#define CR0_CD (1u << 30)
#define CR4_PGE (1u << 7)
#define MSR_MTRRCAP     0xFE
#define MSR_MTRRDEFTYPE 0x2FF
#define MSR_MTRRFIX_FIRST 0x250         /* 64K_00000; then 0x258, 0x259, 0x268-0x26F */
#define MSR_MTRRPHYSBASE0 0x200
#define F_MTRR (1u << 12)
#define MAX_VAR_MTRRS 16

static const uint32_t fixed_msrs[11] = {
    0x250, 0x258, 0x259, 0x268, 0x269, 0x26A, 0x26B, 0x26C, 0x26D, 0x26E, 0x26F
};
static struct {
    int valid, have_fixed;
    uint32_t nvar;
    uint64_t deftype;
    uint64_t fixed[11];
    uint64_t var[MAX_VAR_MTRRS][2];     /* base, mask */
} mtrr;

void mtrr_save(void)
{
    uint32_t r[4];
    cpuid(0, r);
    if (r[0] < 1)
        return;
    cpuid(1, r);
    if (!(r[3] & F_MTRR) || !(r[3] & (1u << 5)))      /* MSRs */
        return;
    uint64_t cap = rdmsr(MSR_MTRRCAP);
    mtrr.nvar = MIN((uint32_t)(cap & 0xFF), (uint32_t)MAX_VAR_MTRRS);
    mtrr.have_fixed = (cap >> 8) & 1;
    mtrr.deftype = rdmsr(MSR_MTRRDEFTYPE);
    if (mtrr.have_fixed)
        for (int i = 0; i < 11; i++)
            mtrr.fixed[i] = rdmsr(fixed_msrs[i]);
    for (uint32_t i = 0; i < mtrr.nvar; i++) {
        mtrr.var[i][0] = rdmsr(MSR_MTRRPHYSBASE0 + 2 * i);
        mtrr.var[i][1] = rdmsr(MSR_MTRRPHYSBASE0 + 2 * i + 1);
    }
    mtrr.valid = 1;
}

/* Interrupts off. Nothing changes if what is there matches already. */
static void mtrr_restore(void)
{
    if (!mtrr.valid)
        return;
    int same = rdmsr(MSR_MTRRDEFTYPE) == mtrr.deftype;
    for (int i = 0; same && mtrr.have_fixed && i < 11; i++)
        same = rdmsr(fixed_msrs[i]) == mtrr.fixed[i];
    for (uint32_t i = 0; same && i < mtrr.nvar; i++)
        same = rdmsr(MSR_MTRRPHYSBASE0 + 2 * i) == mtrr.var[i][0] &&
               rdmsr(MSR_MTRRPHYSBASE0 + 2 * i + 1) == mtrr.var[i][1];
    if (same)
        return;

    uint32_t cr4 = read_cr4();
    if (cr4 & CR4_PGE)
        write_cr4(cr4 & ~CR4_PGE);      /* global pages out of the TLB too */
    uint32_t cr0 = read_cr0();
    write_cr0((cr0 | CR0_CD) & ~CR0_NW);        /* no-fill cache mode */
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());
    wrmsr(MSR_MTRRDEFTYPE, mtrr.deftype & ~0xC00ull);      /* E and FE off while they change */
    if (mtrr.have_fixed)
        for (int i = 0; i < 11; i++)
            wrmsr(fixed_msrs[i], mtrr.fixed[i]);
    for (uint32_t i = 0; i < mtrr.nvar; i++) {
        wrmsr(MSR_MTRRPHYSBASE0 + 2 * i, mtrr.var[i][0]);
        wrmsr(MSR_MTRRPHYSBASE0 + 2 * i + 1, mtrr.var[i][1]);
    }
    __asm__ volatile("wbinvd" ::: "memory");
    write_cr3(read_cr3());
    wrmsr(MSR_MTRRDEFTYPE, mtrr.deftype);
    write_cr0(cr0 & ~(CR0_CD | CR0_NW));
    write_cr4(cr4);
}

/* A processor back from suspend to RAM (the boot processor in sleep.c,
 * the others in smp.c), with interrupts off: what the power took. */
void cpu_resume(void)
{
    cpu_init_ap();
    mtrr_restore();
}
