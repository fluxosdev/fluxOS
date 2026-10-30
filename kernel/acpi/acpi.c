/*
 * ACPI: the firmware's description of the machine, and its AML.
 *
 * At boot, acpi_init() finds the RSDP in the BIOS areas and walks the
 * RSDT (or XSDT) itself: the MADT lists the processors for smp.c, and the
 * FADT and the \_S5 package (read straight out of the DSDT's bytes) are
 * enough to switch the machine off. Then uACPI (third_party/uacpi) takes
 * the tables too, and later, once PCI is up, acpi_namespace_init() has it
 * load and run the AML: the embedded controller first (ec.c), as so much
 * of a laptop's AML reads it, then the namespace's _INI methods, then the
 * devices power.c looks after. From then on the SCI is uACPI's: it runs
 * the GPE methods and passes on Notify()s, and the fixed power and sleep
 * buttons come to power.c.
 *
 * If uACPI cannot load the namespace, or "acpi=basic" is on the command
 * line, the old way stays: no AML, power-off by the \_S5 bytes (without
 * _PTS), and the power button through a handler of our own on the SCI.
 * "acpi=off" leaves ACPI alone altogether (no power-off, one processor).
 */
#include <string.h>
#include <uacpi/acpi.h>
#include <uacpi/uacpi.h>
#include <uacpi/event.h>
#include <uacpi/sleep.h>
#include <uacpi/tables.h>
#include <uacpi/utilities.h>
#include "kernel.h"
#include "acpi.h"
#include "drivers.h"
#include "mm.h"
#include "smp.h"
#include "x86.h"

#define SLP_EN    (1u << 13)
#define SLP_TYP   (7u << 10)
#define SCI_EN    1u
#define PWRBTN_EN 0x100u                /* PM1 enable and status: the power button */

struct sdt {                            /* every table starts with this */
    char sig[4];
    uint32_t length;
    uint8_t revision, checksum;
    char oem[6], oem_table[8];
    uint32_t oem_rev, creator, creator_rev;
} __attribute__((packed));

static struct {
    int found;                          /* \_S5 and PM1a control: power-off can be tried */
    uint16_t pm1a, pm1b;                /* PM1 control ports */
    uint16_t smi_cmd;
    uint8_t acpi_enable;
    uint8_t slp_typa, slp_typb;
    uint16_t sci_irq;
    uint16_t evt1a, evt1b;              /* PM1 event blocks: status, then enable at +len/2 */
    uint8_t evt_len;
    uint16_t gpe0;                      /* GPE0 block: status, then enable at +len/2 */
    uint8_t gpe0_len;
} acpi;

int acpi_aml;                           /* uACPI runs the AML */
static int uacpi_tables;                /* uACPI has the tables */
static uint32_t rsdp_phys;

/* ---- reading physical memory at boot ---- */

static uint32_t win_phys;               /* the 4 MiB window above the identity map, mapped */
static uint8_t *win_virt;

/* A pointer to len bytes of physical memory, or NULL. Only while booting. */
static const void *phys(uint32_t pa, uint32_t len)
{
    if (len > 0x400000u || pa + len < pa)
        return NULL;
    if (pa + len <= KERNEL_SPACE_END) {
        const void *p = (const void *)pa;       /* the kernel maps low memory one to one */
        __asm__("" : "+r"(p));          /* (so the compiler does not take 0x40E for a null pointer) */
        return p;
    }
    uint32_t base = pa & ~0x3FFFFFu;
    if (pa + len > base + 0x400000u)
        return NULL;                    /* across two windows: not worth it */
    if (!win_virt || win_phys != base) {
        win_virt = vmm_map_mmio(base, 0x400000u, 0);
        win_phys = base;
        if (!win_virt)
            return NULL;
    }
    return win_virt + (pa - base);
}

static int sum_ok(const void *p, uint32_t len)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s = (uint8_t)(s + ((const uint8_t *)p)[i]);
    return s == 0;
}

/* A table by physical address, checked: its whole length, or NULL. */
static const struct sdt *table(uint32_t pa)
{
    const struct sdt *h = phys(pa, sizeof(*h));
    if (!h || h->length < sizeof(*h))
        return NULL;
    h = phys(pa, h->length);
    return h && sum_ok(h, h->length) ? h : NULL;
}

/* ---- finding the tables ---- */

static const uint8_t *find_rsdp_in(uint32_t start, uint32_t len)
{
    const uint8_t *p = phys(start, len);
    if (!p)
        return NULL;
    for (uint32_t off = 0; off + 20 <= len; off += 16)
        if (!memcmp(p + off, "RSD PTR ", 8) && sum_ok(p + off, 20))
            return p + off;
    return NULL;
}

static const uint8_t *find_rsdp(void)
{
    const uint8_t *bda = phys(0x40E, 2);         /* the BIOS data area: the EBDA's segment */
    uint32_t ebda = bda ? (uint32_t)(bda[0] | bda[1] << 8) << 4 : 0;
    const uint8_t *r = NULL;
    if (ebda >= 0x80000 && ebda < 0xA0000)
        r = find_rsdp_in(ebda, 1024);
    return r ? r : find_rsdp_in(0xE0000, 0x20000);
}

/* Below 1 MiB, so its virtual address is its physical one. */
uint32_t acpi_rsdp_phys(void)
{
    return rsdp_phys;
}

static uint32_t rd32(const void *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
}

/* The MADT ("APIC"): after the local APIC's address and flags, entries of
 * a type and a length; type 0 is a processor's local APIC. */
static void read_madt(const struct sdt *t)
{
    const uint8_t *p = (const uint8_t *)t + sizeof(*t) + 8;
    const uint8_t *end = (const uint8_t *)t + t->length;
    while (p + 2 <= end && p[1] >= 2 && p + p[1] <= end) {
        if (p[0] == 0 && p[1] >= 8 && (rd32(p, 4) & 1))        /* enabled */
            smp_add_cpu(p[3]);
        p += p[1];
    }
}

/*
 * \_S5 in an AML table: NameOp "_S5_" (perhaps as "\_S5_") then a package
 * whose first two elements are the PM1a and PM1b sleep types.
 */
static int find_s5(const struct sdt *t)
{
    const uint8_t *p = (const uint8_t *)t + sizeof(*t);
    const uint8_t *end = (const uint8_t *)t + t->length;
    for (; p + 8 <= end; p++) {
        if (memcmp(p, "_S5_", 4))
            continue;
        const uint8_t *s = p;
        if (!((s - 1 >= (const uint8_t *)t && s[-1] == 0x08) ||
              (s - 2 >= (const uint8_t *)t && s[-2] == 0x08 && s[-1] == '\\')))
            continue;
        s += 4;
        if (*s++ != 0x12)               /* PackageOp */
            continue;
        s += ((*s & 0xC0) >> 6) + 1;    /* PkgLength */
        s++;                            /* NumElements */
        uint8_t v[2];
        for (int i = 0; i < 2; i++) {
            if (s >= end)
                return 0;
            if (*s == 0x0A)             /* BytePrefix */
                s++;
            if (s >= end)
                return 0;
            v[i] = *s++;                /* or ZeroOp (0) / OneOp (1), which are their values */
            if (v[i] == 0xFF)           /* OnesOp: not a sleep type */
                v[i] = 0;
        }
        acpi.slp_typa = v[0] & 7;
        acpi.slp_typb = v[1] & 7;
        return 1;
    }
    return 0;
}

/* Hand the hardware to ACPI if the firmware has not (SCI_EN is its sign). */
static void enable_acpi_mode(void)
{
    if (acpi.smi_cmd && acpi.acpi_enable && !(inw(acpi.pm1a) & SCI_EN)) {
        outb(acpi.smi_cmd, acpi.acpi_enable);
        for (int i = 0; i < 300 && !(inw(acpi.pm1a) & SCI_EN); i++)
            udelay(10000);
    }
}

/* Without uACPI: the SCI's only source is the power button. */
static void sci_handler(struct trapframe *tf)
{
    (void)tf;
    uint16_t sts = inw(acpi.evt1a);
    if (acpi.evt1b)
        sts |= inw(acpi.evt1b);
    if (sts & PWRBTN_EN)
        power_fixed_button(0);
    outw(acpi.evt1a, sts);              /* writing a 1 clears a status bit */
    if (acpi.evt1b)
        outw(acpi.evt1b, sts);
}

void acpi_init(void)
{
    if (cmdline_has("acpi=off")) {
        boot_ok("ACPI: off (acpi=off)");
        return;
    }
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) {
        boot_warn("ACPI: no tables found (power-off will not work)");
        return;
    }
    rsdp_phys = (uint32_t)rsdp;
    uint8_t rev = rsdp[15];
    uint32_t rsdt_pa = rd32(rsdp, 16), xsdt_pa = 0;
    if (rev >= 2 && sum_ok(rsdp, rd32(rsdp, 20) < 36 ? 20 : 36) && rd32(rsdp, 28) == 0)
        xsdt_pa = rd32(rsdp, 24);       /* only if it is below 4 GiB */

    /* The RSDT's entries are 32-bit, the XSDT's 64-bit. */
    const struct sdt *root = rsdt_pa ? table(rsdt_pa) : NULL;
    int wide = 0;
    if (!root && xsdt_pa) {
        root = table(xsdt_pa);
        wide = 1;
    }
    if (!root) {
        boot_warn("ACPI: bad root table (power-off will not work)");
        return;
    }
    int nent = (int)((root->length - sizeof(*root)) / (wide ? 8 : 4));
    uint32_t ents[64];
    if (nent > 64)
        nent = 64;
    for (int i = 0; i < nent; i++)
        ents[i] = rd32(root, (uint32_t)sizeof(*root) + (uint32_t)i * (wide ? 8u : 4u));

    for (int i = 0; i < nent; i++) {
        const struct sdt *t = table(ents[i]);
        if (t && !memcmp(t->sig, "APIC", 4))
            read_madt(t);
    }

    /* The FADT ("FACP"): its registers and the DSDT. */
    uint32_t dsdt_pa = 0;
    uint16_t gpe0 = 0;
    uint8_t gpe0_len = 0;
    for (int i = 0; i < nent && !acpi.pm1a; i++) {
        const struct sdt *f = table(ents[i]);
        if (!f || memcmp(f->sig, "FACP", 4))
            continue;
        uint32_t len = f->length;
        dsdt_pa = len >= 44 ? rd32(f, 40) : 0;
        acpi.smi_cmd = len >= 52 ? (uint16_t)rd32(f, 48) : 0;
        acpi.acpi_enable = len >= 53 ? ((const uint8_t *)f)[52] : 0;
        acpi.sci_irq = len >= 48 ? (uint16_t)(rd32(f, 44) >> 16) : 0;
        acpi.evt1a = len >= 60 ? (uint16_t)rd32(f, 56) : 0;
        acpi.evt1b = len >= 64 ? (uint16_t)rd32(f, 60) : 0;
        acpi.evt_len = len >= 89 ? ((const uint8_t *)f)[88] : 0;
        gpe0 = len >= 84 ? (uint16_t)rd32(f, 80) : 0;
        gpe0_len = len >= 93 ? ((const uint8_t *)f)[92] : 0;
        acpi.pm1a = len >= 68 ? (uint16_t)rd32(f, 64) : 0;
        acpi.pm1b = len >= 72 ? (uint16_t)rd32(f, 68) : 0;
        /* ACPI 2.0's 64-bit forms, when the old ones are empty */
        if (!dsdt_pa && len >= 148 && !rd32(f, 144))
            dsdt_pa = rd32(f, 140);
        if (!acpi.pm1a && len >= 184 && ((const uint8_t *)f)[172] == 1 && !rd32(f, 180))
            acpi.pm1a = (uint16_t)rd32(f, 176);    /* X_PM1a_CNT_BLK in I/O space */
        if (!acpi.pm1b && len >= 196 && ((const uint8_t *)f)[184] == 1 && !rd32(f, 192))
            acpi.pm1b = (uint16_t)rd32(f, 188);
    }
    if (!acpi.pm1a) {
        boot_warn("ACPI: no PM1 control register (power-off will not work)");
        return;
    }

    /* \_S5: in the DSDT, or else in an SSDT. */
    const struct sdt *dsdt = dsdt_pa ? table(dsdt_pa) : NULL;
    int s5 = dsdt && find_s5(dsdt);
    for (int i = 0; i < nent && !s5; i++) {
        const struct sdt *t = table(ents[i]);
        if (t && !memcmp(t->sig, "SSDT", 4))
            s5 = find_s5(t);
    }
    if (!s5) {
        boot_warn("ACPI: no \\_S5 sleep state (power-off will not work)");
        return;
    }
    acpi.found = 1;
    acpi.gpe0 = gpe0;
    acpi.gpe0_len = gpe0_len;
    boot_ok("ACPI %s: power-off by PM1 control at %04x, sleep type %u",
            rev >= 2 ? "2.0+" : "1.0", acpi.pm1a, acpi.slp_typa);
    if (cmdline_has("acpi=basic")) {
        boot_ok("ACPI: firmware methods not run (acpi=basic)");
        return;
    }

    /* uACPI takes the tables too (and switches to ACPI mode). */
    osl_init();
    uacpi_status st = uacpi_initialize(0);
    if (uacpi_unlikely_error(st)) {
        boot_warn("ACPI: uACPI cannot use the tables (%s): firmware methods not run",
                  uacpi_status_to_string(st));
        return;
    }
    uacpi_tables = 1;
}

/*
 * Load and run the firmware's AML. After PCI is usable (AML reads the
 * chipset's configuration) and the timer runs; before the drivers that
 * want to know about power.
 */
void acpi_namespace_init(void)
{
    if (!uacpi_tables)
        return;
    ec_early();
    uacpi_status st = uacpi_namespace_load();
    if (uacpi_unlikely_error(st)) {
        boot_warn("ACPI: the firmware's AML did not load (%s): firmware methods not run",
                  uacpi_status_to_string(st));
        return;
    }
    uacpi_set_interrupt_model(UACPI_INTERRUPT_MODEL_PIC);
    power_early();
    ec_init();
    st = uacpi_namespace_initialize();
    if (uacpi_unlikely_error(st)) {
        boot_warn("ACPI: the namespace did not initialise (%s): firmware methods not run",
                  uacpi_status_to_string(st));
        return;
    }
    acpi_aml = 1;
    ec_start();
    power_init();
    boot_ok("ACPI: firmware methods running (uACPI)%s", sleep_supported() ? ", suspend to RAM available" : "");
}

/* An integer from a method or named object; 0 if there was one. */
int acpi_eval_int(uacpi_namespace_node *node, const char *path, uint64_t *out)
{
    uacpi_u64 v;
    if (uacpi_eval_simple_integer(node, path, &v) != UACPI_STATUS_OK)
        return -1;
    *out = v;
    return 0;
}

/* Before switching off, with interrupts as they are: _PTS(5) (AML, perhaps
 * the EC). */
void acpi_prepare_poweroff(void)
{
    if (acpi_aml)
        uacpi_prepare_for_sleep_state(UACPI_SLEEP_STATE_S5);
}

/* Switch off, interrupts off. Returns only if the machine is still on. */
void acpi_poweroff(void)
{
    if (acpi_aml) {
        uacpi_enter_sleep_state(UACPI_SLEEP_STATE_S5);
        udelay(500000);
    }
    if (!acpi.found)
        return;
    /* Ask the firmware to hand ACPI over, unless it already has. */
    enable_acpi_mode();
    /* The sleep type first, then with SLP_EN: some chipsets want them apart. */
    uint16_t a = (uint16_t)((inw(acpi.pm1a) & ~(SLP_TYP | SLP_EN)) | ((uint32_t)acpi.slp_typa << 10));
    outw(acpi.pm1a, a);
    if (acpi.pm1b) {
        uint16_t b = (uint16_t)((inw(acpi.pm1b) & ~(SLP_TYP | SLP_EN)) | ((uint32_t)acpi.slp_typb << 10));
        outw(acpi.pm1b, b);
        outw(acpi.pm1a, (uint16_t)(a | SLP_EN));
        outw(acpi.pm1b, (uint16_t)(b | SLP_EN));
    } else {
        outw(acpi.pm1a, (uint16_t)(a | SLP_EN));
    }
    udelay(500000);                     /* it takes a moment to go */
}

/* ---- events ---- */

static int fixed_buttons;               /* which fixed events have handlers: 1 power, 2 sleep */

/* After S3 the fixed buttons' enable bits may be off again, as ACPICA
 * knows: on again (sleep.c). */
void acpi_events_resume(void)
{
    if (fixed_buttons & 1)
        uacpi_enable_fixed_event(UACPI_FIXED_EVENT_POWER_BUTTON);
    if (fixed_buttons & 2)
        uacpi_enable_fixed_event(UACPI_FIXED_EVENT_SLEEP_BUTTON);
}

static uacpi_interrupt_ret fixed_power(uacpi_handle ctx)
{
    (void)ctx;
    power_fixed_button(0);
    return UACPI_INTERRUPT_HANDLED;
}

static uacpi_interrupt_ret fixed_sleep(uacpi_handle ctx)
{
    (void)ctx;
    power_fixed_button(1);
    return UACPI_INTERRUPT_HANDLED;
}

/* Once processes run: the worker threads, the GPEs, the buttons, and the
 * power thread. */
void acpi_power_start(void)
{
    if (acpi_aml) {
        osl_start();
        /* The FADT says which buttons are fixed ones; the others are
         * devices (PNP0C0C, PNP0C0E) that power.c hears through Notify(). */
        struct acpi_fadt *fadt;
        uint32_t fflags = uacpi_table_fadt(&fadt) == UACPI_STATUS_OK ? fadt->flags
                                                                      : ACPI_PWR_BUTTON | ACPI_SLP_BUTTON;
        if (!(fflags & ACPI_PWR_BUTTON) &&
            uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_POWER_BUTTON, fixed_power, NULL) ==
            UACPI_STATUS_OK)
            fixed_buttons |= 1;
        if (!(fflags & ACPI_SLP_BUTTON) &&
            uacpi_install_fixed_event_handler(UACPI_FIXED_EVENT_SLEEP_BUTTON, fixed_sleep, NULL) ==
            UACPI_STATUS_OK)
            fixed_buttons |= 2;
        uacpi_finalize_gpe_initialization();
        power_start();
        return;
    }
    if (!acpi.found || !acpi.evt1a || acpi.evt_len < 4 || acpi.sci_irq == 0 || acpi.sci_irq > 15 ||
        cmdline_has("acpibutton=off"))
        return;
    /* The power button, through the SCI. Only the button may raise it. */
    enable_acpi_mode();
    uint16_t half = acpi.evt_len / 2;
    for (int i = 0; i < acpi.gpe0_len / 2; i++)
        outb((uint16_t)(acpi.gpe0 + acpi.gpe0_len / 2 + i), 0);        /* no GPE raises the SCI */
    outw(acpi.evt1a, 0xFFFF);
    outw((uint16_t)(acpi.evt1a + half), PWRBTN_EN);
    if (acpi.evt1b) {
        outw(acpi.evt1b, 0xFFFF);
        outw((uint16_t)(acpi.evt1b + half), PWRBTN_EN);
    }
    irq_register(acpi.sci_irq, sci_handler);
    power_start();
    boot_ok("ACPI power button on IRQ %u", acpi.sci_irq);
}
