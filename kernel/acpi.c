/*
 * ACPI, just enough to switch the machine off.
 *
 * At boot: find the RSDP in the BIOS areas, follow the RSDT (or XSDT) to
 * the FADT for the PM1 control registers and the SMI port that hands ACPI
 * to the OS, and find the \_S5 package (soft off) in the DSDT, or an SSDT,
 * for the sleep type values to write. There is no AML interpreter: \_S5
 * is read straight out of the table's bytes, which is how it is written
 * in practice, and \_PTS is not run.
 *
 * To power off: enable ACPI mode if the firmware has not, then write the
 * S5 sleep type with SLP_EN to PM1a (and PM1b) control.
 */
#include <string.h>
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "x86.h"

#define SLP_EN    (1u << 13)
#define SLP_TYP   (7u << 10)
#define SCI_EN    1u

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
} acpi;

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

static uint32_t rd32(const void *p, uint32_t off)
{
    uint32_t v;
    memcpy(&v, (const uint8_t *)p + off, 4);
    return v;
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

void acpi_init(void)
{
    const uint8_t *rsdp = find_rsdp();
    if (!rsdp) {
        boot_warn("ACPI: no tables found (power-off will not work)");
        return;
    }
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

    /* The FADT ("FACP"): its registers and the DSDT. */
    uint32_t dsdt_pa = 0;
    for (int i = 0; i < nent && !acpi.pm1a; i++) {
        const struct sdt *f = table(ents[i]);
        if (!f || memcmp(f->sig, "FACP", 4))
            continue;
        uint32_t len = f->length;
        dsdt_pa = len >= 44 ? rd32(f, 40) : 0;
        acpi.smi_cmd = len >= 52 ? (uint16_t)rd32(f, 48) : 0;
        acpi.acpi_enable = len >= 53 ? ((const uint8_t *)f)[52] : 0;
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
    boot_ok("ACPI %s: power-off by PM1 control at %04x, sleep type %u",
            rev >= 2 ? "2.0+" : "1.0", acpi.pm1a, acpi.slp_typa);
}

/* Switch off. Returns only if the machine is still on. */
void acpi_poweroff(void)
{
    if (!acpi.found)
        return;
    /* Ask the firmware to hand ACPI over, unless it already has. */
    if (acpi.smi_cmd && acpi.acpi_enable && !(inw(acpi.pm1a) & SCI_EN)) {
        outb(acpi.smi_cmd, acpi.acpi_enable);
        for (int i = 0; i < 300 && !(inw(acpi.pm1a) & SCI_EN); i++)
            udelay(10000);
    }
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
