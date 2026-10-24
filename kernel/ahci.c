/*
 * AHCI driver: SATA hard disks (LBA48/LBA28, read/write) and SATA
 * ATAPI CD/DVD drives on a controller in AHCI mode, as on most PCs
 * since about 2006 and on QEMU's q35 machine. A controller left in IDE
 * ("compatible") mode by the firmware is ata.c's.
 *
 * Each port has one command slot in use: the command is described in
 * memory (a register FIS, an ATAPI packet and a table of buffers) and
 * the controller moves the data by DMA itself. The caller sleeps until
 * the port interrupts, or polls before processes exist. Callers
 * serialise through the VFS lock, as for the IDE driver, so one bounce
 * buffer serves every port.
 *
 * Disks are named sda, sdb, ... and CD drives sr0, sr1, ..., as Linux
 * names them; USB storage takes the next sd names.
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "x86.h"

/* HBA registers */
#define HBA_CAP   0x00
#define HBA_GHC   0x04
#define HBA_IS    0x08
#define HBA_PI    0x0C
#define HBA_CAP2  0x24
#define HBA_BOHC  0x28

#define CAP_SSS   (1u << 27)            /* staggered spin-up: ports wait for SUD */
#define GHC_IE    (1u << 1)
#define GHC_AE    (1u << 31)
#define CAP2_BOH  (1u << 0)             /* BIOS/OS handoff */
#define BOHC_BOS  (1u << 0)
#define BOHC_OOS  (1u << 1)
#define BOHC_BB   (1u << 4)

/* Port registers, relative to 0x100 + 0x80 * port */
#define PX_CLB   0x00
#define PX_CLBU  0x04
#define PX_FB    0x08
#define PX_FBU   0x0C
#define PX_IS    0x10
#define PX_IE    0x14
#define PX_CMD   0x18
#define PX_TFD   0x20
#define PX_SIG   0x24
#define PX_SSTS  0x28
#define PX_SCTL  0x2C
#define PX_SERR  0x30
#define PX_CI    0x38

#define CMD_ST    (1u << 0)
#define CMD_SUD   (1u << 1)
#define CMD_POD   (1u << 2)
#define CMD_FRE   (1u << 4)
#define CMD_FR    (1u << 14)
#define CMD_CR    (1u << 15)
#define CMD_ATAPI (1u << 24)

#define IS_TFES  (1u << 30)             /* task file error */
#define IS_FATAL 0x78000000u            /* host bus fatal/data error, interface fatal error */
#define IE_MASK  0x7800002Fu            /* completions and errors, not hot plug */

#define TFD_ERR 0x01
#define TFD_DRQ 0x08
#define TFD_BSY 0x80
#define ERR_ABRT 0x04

#define SIG_ATA   0x00000101u
#define SIG_ATAPI 0xEB140101u

#define ATA_READ_DMA      0xC8
#define ATA_READ_DMA_EXT  0x25
#define ATA_WRITE_DMA     0xCA
#define ATA_WRITE_DMA_EXT 0x35
#define ATA_FLUSH         0xE7
#define ATA_FLUSH_EXT     0xEA
#define ATA_PACKET        0xA0
#define ATA_ID_PACKET     0xA1
#define ATA_IDENTIFY      0xEC

#define ATAPI_SECTOR 2048
#define BOUNCE_SIZE  65536u
#define CMD_TIMEOUT  10000              /* ms */
#define DEVERR       1                  /* the device reported an error */

struct cmd_header {
    uint16_t flags;                     /* FIS length in dwords, ATAPI, write */
    uint16_t prdtl;                     /* buffer table entries */
    volatile uint32_t prdbc;            /* bytes transferred */
    uint32_t ctba, ctbau;
    uint32_t reserved[4];
};

#define CH_ATAPI 0x20
#define CH_WRITE 0x40

struct prd {
    uint32_t dba, dbau, reserved;
    uint32_t dbc;                       /* byte count - 1; bit 31: interrupt */
};

struct cmd_table {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t reserved[48];
    struct prd prdt[1];
};

struct hba;

struct ahci_port {
    struct hba *hba;
    int num;
    volatile uint8_t *regs;
    struct cmd_header *cl;              /* 1 KiB command list (slot 0 used) */
    uint8_t *fis;                       /* 256-byte received FIS area */
    struct cmd_table *ct;
    volatile uint32_t is_seen;          /* interrupt status taken by the IRQ handler */
    int atapi;
    int lba48;
    int flush_ext;
    int no_flush;
    int media_changed;
    struct blockdev bd;
};

struct hba {
    volatile uint8_t *regs;
    int irq;
    struct ahci_port *ports[32];
};

#define MAX_HBAS 4

static struct hba hbas[MAX_HBAS];
static int nhbas;
static uint8_t *bounce;

static inline uint32_t hba_rd(struct hba *h, int reg)
{
    return *(volatile uint32_t *)(h->regs + reg);
}

static inline void hba_wr(struct hba *h, int reg, uint32_t v)
{
    *(volatile uint32_t *)(h->regs + reg) = v;
}

static inline uint32_t px_rd(struct ahci_port *p, int reg)
{
    return *(volatile uint32_t *)(p->regs + reg);
}

static inline void px_wr(struct ahci_port *p, int reg, uint32_t v)
{
    *(volatile uint32_t *)(p->regs + reg) = v;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static int in_process(void)
{
    return current && current->pid > 0;
}

/* Wait up to `ms` for (reg & mask) == want. Returns 0, or -1 on timeout. */
static int px_wait(struct ahci_port *p, int reg, uint32_t mask, uint32_t want, uint32_t ms)
{
    for (uint32_t i = 0; i < ms * 10; i++) {
        if ((px_rd(p, reg) & mask) == want)
            return 0;
        udelay(100);
    }
    return (px_rd(p, reg) & mask) == want ? 0 : -1;
}

static void ahci_irq(struct trapframe *tf)
{
    (void)tf;
    for (int i = 0; i < nhbas; i++) {
        struct hba *h = &hbas[i];
        uint32_t is = hba_rd(h, HBA_IS);
        if (!is)
            continue;
        for (int n = 0; n < 32; n++) {
            if (!(is & (1u << n)))
                continue;
            struct ahci_port *p = h->ports[n];
            volatile uint8_t *regs = h->regs + 0x100 + 0x80 * n;
            uint32_t pis = *(volatile uint32_t *)(regs + PX_IS);
            *(volatile uint32_t *)(regs + PX_IS) = pis;
            if (p) {
                p->is_seen |= pis;
                wakeup(p);
            }
        }
        hba_wr(h, HBA_IS, is);
    }
}

/* ---- port control ---- */

static int port_stop(struct ahci_port *p)
{
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~CMD_ST);
    int r = px_wait(p, PX_CMD, CMD_CR, 0, 500);
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~CMD_FRE);
    if (px_wait(p, PX_CMD, CMD_FR, 0, 500) < 0)
        r = -1;
    return r;
}

static int link_up(struct ahci_port *p)
{
    return (px_rd(p, PX_SSTS) & 0xF) == 3;
}

/* COMRESET: make the device start over and the link train again. */
static void port_comreset(struct ahci_port *p)
{
    px_wr(p, PX_SCTL, (px_rd(p, PX_SCTL) & ~0xFu) | 1);
    mdelay(2);
    px_wr(p, PX_SCTL, px_rd(p, PX_SCTL) & ~0xFu);
    px_wait(p, PX_SSTS, 0xF, 3, 1000);
    px_wr(p, PX_SERR, 0xFFFFFFFFu);
}

static int port_start(struct ahci_port *p)
{
    if (px_wait(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000) < 0)
        return -1;
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) | CMD_FRE | CMD_ST);
    return 0;
}

/* After an error the port stops taking commands until it is restarted. */
static void port_recover(struct ahci_port *p)
{
    px_wr(p, PX_CMD, px_rd(p, PX_CMD) & ~CMD_ST);
    px_wait(p, PX_CMD, CMD_CR, 0, 500);
    px_wr(p, PX_SERR, 0xFFFFFFFFu);
    px_wr(p, PX_IS, 0xFFFFFFFFu);
    p->is_seen = 0;
    if (px_rd(p, PX_TFD) & (TFD_BSY | TFD_DRQ))
        port_comreset(p);
    if (port_start(p) < 0) {
        port_comreset(p);
        port_start(p);
    }
}

/* ---- commands ---- */

struct ata_cmd {
    uint8_t command;
    uint8_t features;
    uint8_t device;
    uint32_t lba;                       /* bits 0-31; LBA48 commands use 0 above */
    uint16_t count;
    const uint8_t *packet;              /* ATAPI: the 12-byte command */
};

/* Is the command finished? 1 done, -1 failed, 0 still running. */
static int cmd_state(struct ahci_port *p)
{
    uint32_t is = p->is_seen | px_rd(p, PX_IS);
    if (is & (IS_TFES | IS_FATAL))
        return -1;
    return (px_rd(p, PX_CI) & 1) ? 0 : 1;
}

/*
 * Run one command on slot 0, moving `len` bytes to or from buf. Returns
 * the number of bytes moved, DEVERR negated if the device reported an
 * error (an ATAPI check condition, an aborted command), or -EIO.
 */
static int port_exec(struct ahci_port *p, const struct ata_cmd *c, void *buf, uint32_t len,
                     int write, uint32_t timeout_ms)
{
    if (len > BOUNCE_SIZE)
        return -EIO;
    uint32_t addr = (uint32_t)buf;
    void *mem = buf;
    if (len && (addr < 0x100000 || addr + len > KERNEL_SPACE_END || addr + len < addr ||
                (addr & 1))) {
        mem = bounce;
        if (write)
            memcpy(mem, buf, len);
    }

    if (px_wait(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 1000) < 0)
        port_recover(p);

    struct cmd_table *ct = p->ct;
    memset(ct, 0, sizeof(*ct));
    uint8_t *f = ct->cfis;
    f[0] = 0x27;                        /* register FIS, host to device */
    f[1] = 0x80;                        /* a command */
    f[2] = c->command;
    f[3] = c->features;
    f[4] = (uint8_t)c->lba;
    f[5] = (uint8_t)(c->lba >> 8);
    f[6] = (uint8_t)(c->lba >> 16);
    f[7] = c->device;
    f[8] = (uint8_t)(c->lba >> 24);
    f[12] = (uint8_t)c->count;
    f[13] = (uint8_t)(c->count >> 8);
    if (c->packet)
        memcpy(ct->acmd, c->packet, 12);
    if (len) {
        ct->prdt[0].dba = (uint32_t)mem;
        ct->prdt[0].dbc = ((len + 1) & ~1u) - 1;
    }

    struct cmd_header *h = &p->cl[0];
    h->flags = (uint16_t)(5 | (c->packet ? CH_ATAPI : 0) | (write ? CH_WRITE : 0));
    h->prdtl = len ? 1 : 0;
    h->prdbc = 0;
    h->ctba = (uint32_t)ct;
    h->ctbau = 0;

    px_wr(p, PX_IS, 0xFFFFFFFFu);
    p->is_seen = 0;
    barrier();
    px_wr(p, PX_CI, 1);

    int st;
    if (in_process()) {
        /* The interrupt wakes us; the one-tick timeout covers a lost one. */
        uint32_t deadline = ticks + timeout_ms / (1000 / HZ) + 1;
        while (!(st = cmd_state(p)) && (int32_t)(ticks - deadline) < 0)
            sleep_on_timeout(p, ticks + 1);
    } else {
        for (uint32_t i = 0; !(st = cmd_state(p)) && i < timeout_ms * 50; i++)
            udelay(20);
    }
    barrier();

    if (st <= 0) {
        uint32_t tfd = px_rd(p, PX_TFD);
        int deverr = st < 0 && (tfd & TFD_ERR) && !((p->is_seen | px_rd(p, PX_IS)) & IS_FATAL);
        if (st == 0)
            klog("%s: command %02x timed out\n", p->bd.name[0] ? p->bd.name : "ahci", c->command);
        port_recover(p);
        if (deverr)
            return (tfd >> 8) & ERR_ABRT ? -ENOSYS : -DEVERR;
        return -EIO;
    }
    uint32_t got = MIN(h->prdbc, len);
    if (!write && mem != buf)
        memcpy(buf, mem, got);
    return (int)got;
}

/* ---- disks ---- */

static int disk_rw(struct ahci_port *p, uint32_t lba, uint32_t count, void *buf, int write)
{
    struct ata_cmd c = { 0 };
    if (p->lba48) {
        c.command = write ? ATA_WRITE_DMA_EXT : ATA_READ_DMA_EXT;
        c.device = 0x40;
        c.lba = lba;
    } else {
        c.command = write ? ATA_WRITE_DMA : ATA_READ_DMA;
        c.device = (uint8_t)(0x40 | ((lba >> 24) & 0x0F));
        c.lba = lba & 0x00FFFFFF;
    }
    c.count = (uint16_t)count;
    int r = port_exec(p, &c, buf, count * 512, write, CMD_TIMEOUT);
    return r == (int)(count * 512) ? 0 : -EIO;
}

static int ahci_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = bd->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        if (disk_rw(p, lba, n, out, 0) < 0)
            return -EIO;
        lba += n;
        count -= n;
        out += n * 512;
    }
    return 0;
}

static int ahci_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    struct ahci_port *p = bd->priv;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        if (disk_rw(p, lba, n, (void *)in, 1) < 0)
            return -EIO;
        lba += n;
        count -= n;
        in += n * 512;
    }
    return 0;
}

/* As on IDE: a drive that aborts FLUSH CACHE has no cache to flush. */
static int ahci_flush(struct blockdev *bd)
{
    struct ahci_port *p = bd->priv;
    if (p->no_flush)
        return 0;
    struct ata_cmd c = { .command = p->flush_ext ? ATA_FLUSH_EXT : ATA_FLUSH, .device = 0x40 };
    int r = port_exec(p, &c, NULL, 0, 0, 30000);
    if (r == -ENOSYS && p->flush_ext) {
        p->flush_ext = 0;
        c.command = ATA_FLUSH;
        r = port_exec(p, &c, NULL, 0, 0, 30000);
    }
    if (r == -ENOSYS) {
        klog("%s: drive does not support FLUSH CACHE; not flushing its cache\n", bd->name);
        p->no_flush = 1;
        r = 0;
    }
    return r < 0 ? -EIO : 0;
}

/* ---- ATAPI drives ---- */

/*
 * Send a packet command, reading up to len bytes. READ commands move
 * their data by DMA, the small ones by PIO (some drives refuse DMA for
 * odd lengths); the controller fills the buffer either way.
 */
static int atapi_cmd(struct ahci_port *p, const uint8_t *pkt, void *buf, uint32_t len, int dma)
{
    struct ata_cmd c = { .command = ATA_PACKET, .packet = pkt };
    if (dma) {
        c.features = 1;
    } else {
        c.lba = (MIN(len, 0xFFFEu) & 0xFFFF) << 8;      /* byte count limit in LBA1/LBA2 */
    }
    return port_exec(p, &c, buf, len, 0, CMD_TIMEOUT);
}

static void request_sense(struct ahci_port *p, uint8_t *sense)
{
    static const uint8_t pkt[12] = { 0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t tmp[18];
    if (atapi_cmd(p, pkt, sense ? sense : tmp, 18, 0) < 0 && sense)
        memset(sense, 0, 18);
}

static int atapi_capacity(struct blockdev *bd)
{
    struct ahci_port *p = bd->priv;
    static const uint8_t pkt[12] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    /* The first command after a media change reports UNIT ATTENTION. */
    for (int tries = 0; tries < 4; tries++) {
        if (atapi_cmd(p, pkt, cap, sizeof(cap), 0) == 8) {
            uint32_t last = (uint32_t)cap[0] << 24 | (uint32_t)cap[1] << 16 |
                            (uint32_t)cap[2] << 8 | cap[3];
            bd->nsectors = last + 1;
            return 0;
        }
        request_sense(p, NULL);
    }
    bd->nsectors = 0;
    return -EIO;
}

/* TEST UNIT READY and the sense data, read as in ata.c's atapi_media. */
static int atapi_media(struct blockdev *bd)
{
    struct ahci_port *p = bd->priv;
    static const uint8_t tur[12] = { 0 };
    for (int tries = 0; tries < 4; tries++) {
        int r = atapi_cmd(p, tur, NULL, 0, 0);
        if (r == 0) {
            int changed = p->media_changed;
            p->media_changed = 0;
            return changed ? MEDIA_CHANGED : MEDIA_PRESENT;
        }
        if (r == -EIO)
            return MEDIA_BUSY;
        uint8_t sense[18];
        request_sense(p, sense);
        int key = sense[2] & 15, asc = sense[12];
        if (key == 6) {                 /* unit attention */
            p->media_changed = 1;
            continue;
        }
        if (key == 2 && asc == 0x3A) {  /* no medium */
            p->media_changed = 0;
            return MEDIA_NONE;
        }
        return MEDIA_BUSY;
    }
    return MEDIA_BUSY;
}

static int atapi_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    struct ahci_port *p = bd->priv;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, BOUNCE_SIZE / ATAPI_SECTOR);
        uint8_t pkt[12] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8),
                            (uint8_t)n, 0, 0, 0 };
        int want = (int)(n * ATAPI_SECTOR), r = -EIO;
        for (int tries = 0; tries < 3 && r != want; tries++) {
            r = atapi_cmd(p, pkt, out, (uint32_t)want, 1);
            if (r != want)
                request_sense(p, NULL);
        }
        if (r != want)
            return -EIO;
        lba += n;
        count -= n;
        out += want;
    }
    return 0;
}

/* ---- probing ---- */

static void ident_string(const uint16_t *id, int first, int words, char *out)
{
    int k = 0;
    for (int i = 0; i < words; i++) {
        out[k++] = (char)(id[first + i] >> 8);
        out[k++] = (char)(id[first + i] & 0xFF);
    }
    out[k] = 0;
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 0))
        out[--k] = 0;
}

/* Bring the link up and wait for the device's first status. */
static int port_link(struct ahci_port *p, int spin_up)
{
    px_wr(p, PX_SERR, 0xFFFFFFFFu);
    px_wr(p, PX_IS, 0xFFFFFFFFu);
    uint32_t cmd = px_rd(p, PX_CMD) | CMD_FRE;
    if (spin_up)
        cmd |= CMD_SUD | CMD_POD;
    px_wr(p, PX_CMD, cmd);

    /* An empty port shows nothing within a few ms; a device that is
     * there but not talking yet gets a second and then a COMRESET. */
    px_wait(p, PX_SSTS, 0xF, 3, 20);
    if ((px_rd(p, PX_SSTS) & 0xF) == 1)
        px_wait(p, PX_SSTS, 0xF, 3, 1000);
    if ((px_rd(p, PX_SSTS) & 0xF) == 1)
        port_comreset(p);
    if (!link_up(p))
        return -1;
    px_wr(p, PX_SERR, 0xFFFFFFFFu);

    /* A disk that has to spin up stays busy for a few seconds. */
    if (px_wait(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 5000) < 0) {
        port_comreset(p);
        if (px_wait(p, PX_TFD, TFD_BSY | TFD_DRQ, 0, 5000) < 0)
            return -1;
    }
    return 0;
}

static void port_probe(struct hba *h, int n, uint16_t *id)
{
    struct ahci_port *p = kzalloc(sizeof(*p));
    uint32_t page = pmm_alloc();
    if (!p || !page) {
        kfree(p);
        if (page)
            pmm_free(page);
        return;
    }
    memset((void *)page, 0, PAGE_SIZE);
    p->hba = h;
    p->num = n;
    p->regs = h->regs + 0x100 + 0x80 * n;
    p->cl = (struct cmd_header *)page;                  /* 1 KiB aligned */
    p->fis = (uint8_t *)page + 0x400;                   /* 256 aligned */
    p->ct = (struct cmd_table *)(page + 0x500);         /* 128 aligned */

    if (port_stop(p) < 0) {
        port_comreset(p);
        port_stop(p);
    }
    px_wr(p, PX_CLB, (uint32_t)p->cl);
    px_wr(p, PX_CLBU, 0);
    px_wr(p, PX_FB, (uint32_t)p->fis);
    px_wr(p, PX_FBU, 0);

    if (port_link(p, (hba_rd(h, HBA_CAP) & CAP_SSS) != 0) < 0)
        goto empty;
    uint32_t sig = px_rd(p, PX_SIG);
    if (sig != SIG_ATA && sig != SIG_ATAPI)
        goto empty;                     /* a port multiplier or enclosure: not handled */
    p->atapi = sig == SIG_ATAPI;
    if (p->atapi)
        px_wr(p, PX_CMD, px_rd(p, PX_CMD) | CMD_ATAPI);
    if (port_start(p) < 0)
        goto empty;
    h->ports[n] = p;
    px_wr(p, PX_IE, h->irq >= 0 ? IE_MASK : 0);

    struct ata_cmd c = { .command = p->atapi ? ATA_ID_PACKET : ATA_IDENTIFY };
    if (port_exec(p, &c, id, 512, 0, CMD_TIMEOUT) != 512) {
        boot_warn("AHCI: port %d: a drive is there but does not answer IDENTIFY", n);
        goto fail;
    }

    struct blockdev *bd = &p->bd;
    blk_new_name(bd->name, sizeof(bd->name), p->atapi ? "sr" : "sd", p->atapi);
    ident_string(id, 27, 20, bd->model);
    bd->priv = p;
    if (p->atapi) {
        bd->type = BLK_CDROM;
        bd->sector_size = ATAPI_SECTOR;
        bd->readonly = 1;
        bd->read = atapi_read;
        bd->refresh = atapi_capacity;
        bd->media = atapi_media;
        atapi_capacity(bd);
    } else {
        bd->type = BLK_DISK;
        bd->sector_size = 512;
        bd->read = ahci_read;
        bd->write = ahci_write;
        bd->flush = ahci_flush;
        p->lba48 = (id[83] & (1 << 10)) != 0;
        p->flush_ext = p->lba48 && (id[83] & 0xC000) == 0x4000 && (id[83] & (1 << 13));
        uint32_t lba28 = id[60] | ((uint32_t)id[61] << 16);
        uint32_t lba48 = id[100] | ((uint32_t)id[101] << 16);
        bd->nsectors = p->lba48 && (id[102] || id[103]) ? 0xFFFFFFFFu
                     : p->lba48 && lba48 ? lba48 : lba28;
    }
    if (blk_register(bd) < 0)
        goto fail;
    if (p->atapi)
        boot_ok("%s: SATA ATAPI %s, %s", bd->name, bd->model,
                bd->nsectors ? "media present" : "no media");
    else
        boot_ok("%s: SATA %s, %u MiB", bd->name, bd->model, bd->nsectors / 2048);
    if (!p->atapi)
        blk_scan_partitions(bd);
    return;

fail:
    h->ports[n] = NULL;
    px_wr(p, PX_IE, 0);
empty:
    port_stop(p);
    px_wr(p, PX_CLB, 0);
    px_wr(p, PX_FB, 0);
    pmm_free(page);
    kfree(p);
}

/* Take the controller from the firmware, if it says it is using it. */
static void bios_handoff(struct hba *h)
{
    if (!(hba_rd(h, HBA_CAP2) & CAP2_BOH))
        return;
    hba_wr(h, HBA_BOHC, hba_rd(h, HBA_BOHC) | BOHC_OOS);
    for (int i = 0; i < 25 && (hba_rd(h, HBA_BOHC) & BOHC_BOS); i++)
        mdelay(1);
    if (hba_rd(h, HBA_BOHC) & BOHC_BB)
        for (int i = 0; i < 200 && (hba_rd(h, HBA_BOHC) & BOHC_BOS); i++)
            mdelay(10);
}

static void hba_init(const struct pci_dev *pci)
{
    struct hba *h = &hbas[nhbas];
    h->regs = pci_map_bar(pci, PCI_BAR5);
    if (!h->regs) {
        boot_warn("AHCI: controller registers out of reach");
        return;
    }
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    h->irq = cmdline_has("ahci.poll") ? -1 : pci_irq(pci);
    bios_handoff(h);
    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_AE);
    hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) & ~GHC_IE);

    uint32_t cap = hba_rd(h, HBA_CAP), pi = hba_rd(h, HBA_PI);
    uint32_t vs = hba_rd(h, 0x10);
    boot_ok("AHCI %u.%u controller: %u ports, IRQ %d%s", vs >> 16, (vs >> 8) & 0xFF,
            (cap & 0x1F) + 1, h->irq, h->irq < 0 ? " (polled)" : "");
    nhbas++;

    uint16_t *id = kmalloc(512);
    if (!id)
        return;
    for (int n = 0; n < 32; n++)
        if (pi & (1u << n))
            port_probe(h, n, id);
    kfree(id);

    if (h->irq >= 0) {
        hba_wr(h, HBA_IS, 0xFFFFFFFFu);
        irq_register(h->irq, ahci_irq);
        hba_wr(h, HBA_GHC, hba_rd(h, HBA_GHC) | GHC_IE);
    }
}

void ahci_init(void)
{
    struct pci_dev pci;
    uint8_t prog_if;
    for (int i = 0; nhbas < MAX_HBAS && pci_find_class_nth(0x0106, i, &pci, &prog_if) == 0; i++) {
        if (prog_if != 0x01)
            continue;                   /* not AHCI (vendor-specific) */
        if (!bounce && !(bounce = kmalloc(BOUNCE_SIZE)))
            return;
        hba_init(&pci);
    }
}
