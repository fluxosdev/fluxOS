/*
 * IDE controller driver: ATA hard disks (LBA28/LBA48, read/write) and
 * ATAPI CD/DVD drives (PACKET commands, read-only).
 *
 * Data moves by bus-master DMA when the controller supports it (the
 * PIIX-style engine behind PCI BAR4) and the drive has a DMA mode
 * selected: the controller copies a whole command's data (up to 64 KiB)
 * to or from memory by itself. Otherwise, or if a DMA command ever
 * fails, the driver falls back to PIO, where the CPU moves each sector.
 *
 * Once processes exist, completion is interrupt driven: the caller
 * sleeps until the drive raises IRQ14/15, so other processes run while
 * the disk works. Boot-time probing polls instead. Callers serialise
 * through the VFS lock, so one command is in flight at a time.
 *
 * Device names follow the old Linux IDE scheme: hda/hdb are the
 * primary master/slave, hdc/hdd the secondary ones.
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "x86.h"

#define REG_DATA     0
#define REG_ERROR    1
#define REG_FEATURES 1
#define REG_COUNT    2
#define REG_LBA0     3
#define REG_LBA1     4
#define REG_LBA2     5
#define REG_DRIVE    6
#define REG_STATUS   7
#define REG_COMMAND  7

#define ST_ERR  0x01
#define ST_DRQ  0x08
#define ST_DF   0x20
#define ST_BSY  0x80

#define CMD_READ        0x20
#define CMD_READ_EXT    0x24
#define CMD_WRITE       0x30
#define CMD_WRITE_EXT   0x34
#define CMD_READ_DMA      0xC8
#define CMD_READ_DMA_EXT  0x25
#define CMD_WRITE_DMA     0xCA
#define CMD_WRITE_DMA_EXT 0x35
#define CMD_FLUSH       0xE7
#define CMD_FLUSH_EXT   0xEA
#define CMD_PACKET      0xA0
#define CMD_ID_PACKET   0xA1
#define CMD_IDENTIFY    0xEC

#define ATAPI_SECTOR 2048
#define TIMEOUT      2000000
#define IRQ_TIMEOUT  (5 * HZ)          /* then fall back to polling */

/* Bus-master IDE registers, relative to the channel's base. */
#define BM_CMD     0
#define BM_STATUS  2
#define BM_PRD     4
#define BM_START   0x01
#define BM_READ    0x08                 /* direction: device -> memory */
#define BM_ACTIVE  0x01
#define BM_ERR     0x02
#define BM_INTR    0x04

#define DMA_MAX    65536u               /* bytes per command: one PRD entry */
#define PRD_EOT    0x80000000u
#define DMA_DEVERR 1                    /* the device reported an error */

struct ata_chan {
    uint16_t io, ctrl;
    int irq_line;
    volatile int armed;                 /* a command is waiting for an IRQ */
    volatile int fired;
    uint16_t bm;                        /* bus-master registers; 0 = no DMA */
    uint32_t *prd;                      /* physical region descriptor table */
};

static struct ata_chan chans[2] = {
    { .io = 0x1F0, .ctrl = 0x3F6, .irq_line = 14 },
    { .io = 0x170, .ctrl = 0x376, .irq_line = 15 },
};
static int irq_mode;
static uint8_t *dma_bounce;             /* 64 KiB, 64 KiB aligned */

struct ata_dev {
    uint16_t io, ctrl;
    struct ata_chan *ch;
    int slave;
    int atapi;
    int lba48;
    int dma_capable;                    /* IDENTIFY: a DMA mode is selected */
    int dma;                            /* use DMA for data transfers */
    struct blockdev bd;
};

static struct ata_dev drives[4];

static void delay400(struct ata_dev *d)
{
    for (int i = 0; i < 4; i++)
        (void)inb(d->ctrl);
}

static int wait_not_busy(struct ata_dev *d)
{
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(d->io + REG_STATUS);
        if (!(s & ST_BSY))
            return s;
    }
    return -EIO;
}

static int wait_drq(struct ata_dev *d)
{
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t s = inb(d->io + REG_STATUS);
        if (s & ST_BSY)
            continue;
        if (s & (ST_ERR | ST_DF))
            return -EIO;
        if (s & ST_DRQ)
            return 0;
    }
    return -EIO;
}

static void ata_irq(struct trapframe *tf)
{
    struct ata_chan *ch = &chans[(int)tf->int_no - IRQ_BASE == 15];
    (void)inb(ch->io + REG_STATUS);     /* acknowledge INTRQ */
    if (ch->armed) {
        ch->fired = 1;
        wakeup(ch);
    }
}

/* Expect an interrupt for the command (phase) about to start. */
static void arm(struct ata_dev *d)
{
    d->ch->fired = 0;
    d->ch->armed = 1;
}

static void disarm(struct ata_dev *d)
{
    d->ch->armed = 0;
    d->ch->fired = 0;
}

/*
 * Wait for the end of a phase: sleep until the drive interrupts (other
 * processes run meanwhile), then read the settled status. Interrupts
 * stay off in the kernel, so the IRQ cannot slip in between the check
 * and the sleep. Without processes, or if the IRQ never comes, poll.
 */
static int wait_irq(struct ata_dev *d)
{
    struct ata_chan *ch = d->ch;
    if (irq_mode && current && current->pid > 0) {
        uint32_t deadline = ticks + IRQ_TIMEOUT;
        while (!ch->fired && (int32_t)(ticks - deadline) < 0)
            sleep_on_timeout(ch, deadline);
    }
    ch->fired = 0;
    return wait_not_busy(d);
}

static void select_drive(struct ata_dev *d, uint8_t extra)
{
    outb(d->io + REG_DRIVE, (uint8_t)(0xA0 | (d->slave << 4) | extra));
    delay400(d);
}

/* ---- bus-master DMA ---- */

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* Wait for BSY and DRQ to drop: the drive has finished the command. */
static int wait_idle(struct ata_dev *d)
{
    for (int i = 0; i < TIMEOUT; i++) {
        uint8_t st = inb(d->io + REG_STATUS);
        if (!(st & (ST_BSY | ST_DRQ)))
            return st;
    }
    return -EIO;
}

/*
 * Describe the transfer to the controller. Memory the controller can
 * reach directly (identity-mapped kernel memory, dword aligned) is used
 * as is; anything else, such as user buffers, goes through the bounce
 * buffer. Returns the memory actually used.
 */
static void *dma_prepare(struct ata_chan *ch, void *buf, uint32_t bytes, int to_device)
{
    uint32_t addr = (uint32_t)buf;
    void *mem = buf;
    if (addr < 0x100000 || addr + bytes > KERNEL_SPACE_END || addr + bytes < addr ||
        (addr & 3)) {
        mem = dma_bounce;
        if (to_device)
            memcpy(mem, buf, bytes);
        addr = (uint32_t)mem;
    }

    /* A PRD entry may not cross a 64 KiB boundary. */
    uint32_t n = 0, left = bytes;
    while (left) {
        uint32_t chunk = MIN(left, 0x10000u - (addr & 0xFFFF));
        ch->prd[n * 2] = addr;
        ch->prd[n * 2 + 1] = chunk & 0xFFFF;            /* 0 means 64 KiB */
        n++;
        addr += chunk;
        left -= chunk;
    }
    ch->prd[(n - 1) * 2 + 1] |= PRD_EOT;

    outl(ch->bm + BM_PRD, (uint32_t)ch->prd);
    outb(ch->bm + BM_STATUS, (uint8_t)((inb(ch->bm + BM_STATUS) & 0x60) | BM_ERR | BM_INTR));
    outb(ch->bm + BM_CMD, to_device ? 0 : BM_READ);
    barrier();
    return mem;
}

static void dma_start(struct ata_chan *ch)
{
    barrier();
    outb(ch->bm + BM_CMD, inb(ch->bm + BM_CMD) | BM_START);
}

static void dma_stop(struct ata_chan *ch)
{
    outb(ch->bm + BM_CMD, inb(ch->bm + BM_CMD) & ~BM_START);
}

/*
 * Wait for a started DMA command to finish and stop the engine. Returns
 * 0, DMA_DEVERR if the drive reported an error (for example a SCSI
 * check condition from an ATAPI drive), or -EIO if the transfer itself
 * failed or never finished.
 *
 * With processes, sleep until the drive interrupts. An interrupt that
 * arrives for another reason, such as an ATAPI drive asking for its
 * packet, does not set the controller's interrupt bit, so it is simply
 * slept through. The final check, drive idle and engine drained,
 * decides success either way, so a lost interrupt only costs a poll.
 */
static int dma_wait(struct ata_dev *d)
{
    struct ata_chan *ch = d->ch;
    if (irq_mode && current && current->pid > 0) {
        uint32_t deadline = ticks + IRQ_TIMEOUT;
        while (!(inb(ch->bm + BM_STATUS) & (BM_INTR | BM_ERR)) &&
               (int32_t)(ticks - deadline) < 0) {
            ch->fired = 0;
            sleep_on_timeout(ch, deadline);
        }
    } else {
        delay400(d);                    /* BSY is set within 400 ns */
    }

    int st = wait_idle(d);
    int deverr = st >= 0 && (st & ST_ERR);
    if (st >= 0 && !deverr)
        for (int i = 0; i < TIMEOUT && (inb(ch->bm + BM_STATUS) & BM_ACTIVE); i++)
            ;
    uint8_t bm = inb(ch->bm + BM_STATUS);
    dma_stop(ch);
    outb(ch->bm + BM_STATUS, (uint8_t)((bm & 0x60) | BM_ERR | BM_INTR));
    barrier();

    if (st < 0 || (st & ST_DF))
        return -EIO;
    if (deverr)
        return DMA_DEVERR;
    if (bm & (BM_ERR | BM_ACTIVE))
        return -EIO;
    return 0;
}

/* Software reset, to get a drive out of a command that went wrong. */
static void chan_reset(struct ata_chan *ch)
{
    uint8_t nien = irq_mode ? 0x00 : 0x02;
    outb(ch->ctrl, (uint8_t)(nien | 0x04));
    for (int i = 0; i < 20; i++)
        io_wait();
    outb(ch->ctrl, nien);
    for (int i = 0; i < 4000; i++)                      /* ~4 ms */
        io_wait();
    outb(ch->io + REG_DRIVE, 0xA0);
    for (int i = 0; i < TIMEOUT && (inb(ch->io + REG_STATUS) & ST_BSY); i++)
        ;
}

/* ---- ATA disks ---- */

/* Select the drive and load the LBA and sector count registers. */
static void ata_load(struct ata_dev *d, uint32_t lba, uint32_t count, int ext)
{
    if (ext) {
        select_drive(d, 0x40);
        outb(d->io + REG_COUNT, (uint8_t)(count >> 8));
        outb(d->io + REG_LBA0, (uint8_t)(lba >> 24));
        outb(d->io + REG_LBA1, 0);
        outb(d->io + REG_LBA2, 0);
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
    } else {
        select_drive(d, (uint8_t)(0x40 | ((lba >> 24) & 0x0F)));
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
    }
}

static int ata_flush(struct ata_dev *d, int ext)
{
    arm(d);
    outb(d->io + REG_COMMAND, ext ? CMD_FLUSH_EXT : CMD_FLUSH);
    int st = wait_irq(d);
    return st < 0 || (st & (ST_ERR | ST_DF)) ? -EIO : 0;
}

static int ata_pio(struct ata_dev *d, uint32_t lba, uint32_t count, void *buf, int write)
{
    int ext = d->lba48 && (lba + count > 0x0FFFFFFF);
    if (wait_not_busy(d) < 0)
        return -EIO;
    arm(d);
    ata_load(d, lba, count, ext);
    outb(d->io + REG_COMMAND, write ? (ext ? CMD_WRITE_EXT : CMD_WRITE)
                                    : (ext ? CMD_READ_EXT : CMD_READ));

    /* Reads interrupt when each sector is ready; writes interrupt after
     * each sector is taken (the first one is requested without an IRQ). */
    int r = -EIO;
    uint8_t *p = buf;
    for (uint32_t i = 0; i < count; i++, p += 512) {
        if ((write && i == 0) || wait_irq(d) >= 0) {
            if (wait_drq(d) < 0)
                goto out;
        } else {
            goto out;
        }
        if (write)
            outsw(d->io + REG_DATA, p, 256);
        else
            insw(d->io + REG_DATA, p, 256);
    }
    if (write) {
        int st = wait_irq(d);
        if (st < 0 || (st & (ST_ERR | ST_DF)))
            goto out;
    }
    r = 0;
out:
    disarm(d);
    return r;
}

/* One command moving `count` (<= 128) sectors by DMA. */
static int ata_dma(struct ata_dev *d, uint32_t lba, uint32_t count, void *buf, int write)
{
    struct ata_chan *ch = d->ch;
    int ext = d->lba48 && (lba + count > 0x0FFFFFFF);
    uint32_t bytes = count * 512;
    if (wait_not_busy(d) < 0)
        return -EIO;

    void *mem = dma_prepare(ch, buf, bytes, write);
    arm(d);
    ata_load(d, lba, count, ext);
    outb(d->io + REG_COMMAND, write ? (ext ? CMD_WRITE_DMA_EXT : CMD_WRITE_DMA)
                                    : (ext ? CMD_READ_DMA_EXT : CMD_READ_DMA));
    dma_start(ch);

    int r = dma_wait(d);
    disarm(d);
    if (r == 0 && !write && mem != buf)
        memcpy(buf, mem, bytes);
    return r ? -EIO : 0;
}

/* Flush the drive's write cache; the block layer calls this on sync. */
static int ata_flush_cache(struct blockdev *bd)
{
    struct ata_dev *d = bd->priv;
    select_drive(d, 0);
    if (wait_not_busy(d) < 0)
        return -EIO;
    int r = ata_flush(d, d->lba48);
    disarm(d);
    return r;
}

/* DMA when the drive has it; if a DMA command fails, switch the drive to PIO. */
static int ata_xfer(struct ata_dev *d, uint32_t lba, uint32_t count, void *buf, int write)
{
    if (d->dma) {
        if (ata_dma(d, lba, count, buf, write) == 0)
            return 0;
        klog("%s: DMA error at sector %u, falling back to PIO\n", d->bd.name, lba);
        d->dma = 0;
        chan_reset(d->ch);
    }
    return ata_pio(d, lba, count, buf, write);
}

static int ata_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_dev *d = bd->priv;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        if (ata_xfer(d, lba, n, p, 0) < 0)
            return -EIO;
        lba += n;
        count -= n;
        p += n * 512;
    }
    return 0;
}

static int ata_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    struct ata_dev *d = bd->priv;
    const uint8_t *p = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        if (ata_xfer(d, lba, n, (void *)p, 1) < 0)
            return -EIO;
        lba += n;
        count -= n;
        p += n * 512;
    }
    return 0;
}

/* ---- ATAPI drives ---- */

/* Send a 12-byte packet; read up to `len` bytes of data into buf. */
static int atapi_packet(struct ata_dev *d, const uint8_t *pkt, void *buf, uint32_t len)
{
    select_drive(d, 0);
    if (wait_not_busy(d) < 0)
        return -EIO;
    outb(d->io + REG_FEATURES, 0);                      /* PIO, no DMA */
    outb(d->io + REG_LBA1, ATAPI_SECTOR & 0xFF);         /* byte count limit */
    outb(d->io + REG_LBA2, ATAPI_SECTOR >> 8);
    arm(d);
    outb(d->io + REG_COMMAND, CMD_PACKET);
    if (wait_drq(d) < 0) {
        disarm(d);
        return -EIO;
    }
    d->ch->fired = 0;
    outsw(d->io + REG_DATA, pkt, 6);

    /* The drive interrupts when each block of data is ready, and once
     * more with DRQ clear when the command completes. */
    uint8_t *p = buf;
    uint32_t got = 0;
    for (;;) {
        int s = wait_irq(d);
        if (s < 0 || (s & ST_ERR)) {
            disarm(d);
            return -EIO;
        }
        if (!(s & ST_DRQ))
            break;
        uint32_t n = inb(d->io + REG_LBA1) | ((uint32_t)inb(d->io + REG_LBA2) << 8);
        for (uint32_t i = 0; i < n; i += 2) {
            uint16_t w = inw(d->io + REG_DATA);
            if (got + 1 < len) {
                p[got] = (uint8_t)w;
                p[got + 1] = (uint8_t)(w >> 8);
            }
            got += 2;
        }
    }
    disarm(d);
    return (int)MIN(got, len);
}

/*
 * READ(10) by DMA: the packet goes out by PIO as usual, then the
 * controller moves the data. Start the engine only after the packet.
 */
static int atapi_dma_read(struct ata_dev *d, const uint8_t *pkt, void *buf, uint32_t len)
{
    struct ata_chan *ch = d->ch;
    select_drive(d, 0);
    if (wait_not_busy(d) < 0)
        return -EIO;

    void *mem = dma_prepare(ch, buf, len, 0);
    outb(d->io + REG_FEATURES, 1);                      /* DMA */
    outb(d->io + REG_LBA1, 0xFE);
    outb(d->io + REG_LBA2, 0xFF);
    arm(d);
    outb(d->io + REG_COMMAND, CMD_PACKET);
    if (wait_drq(d) < 0) {
        disarm(d);
        return -EIO;
    }
    /* Forget anything left over from the packet request phase. */
    outb(ch->bm + BM_STATUS, (uint8_t)((inb(ch->bm + BM_STATUS) & 0x60) | BM_ERR | BM_INTR));
    ch->fired = 0;
    outsw(d->io + REG_DATA, pkt, 6);
    dma_start(ch);

    int r = dma_wait(d);
    disarm(d);
    if (r == 0 && mem != buf)
        memcpy(buf, mem, len);
    return r;
}

static void request_sense(struct ata_dev *d)
{
    uint8_t pkt[12] = { 0x03, 0, 0, 0, 18, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t sense[18];
    atapi_packet(d, pkt, sense, sizeof(sense));
}

static int atapi_capacity(struct blockdev *bd)
{
    struct ata_dev *d = bd->priv;
    uint8_t pkt[12] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t cap[8];
    /* The first command after a media change reports UNIT ATTENTION. */
    for (int tries = 0; tries < 4; tries++) {
        if (atapi_packet(d, pkt, cap, sizeof(cap)) == 8) {
            uint32_t last = (uint32_t)cap[0] << 24 | (uint32_t)cap[1] << 16 |
                            (uint32_t)cap[2] << 8 | cap[3];
            bd->nsectors = last + 1;
            return 0;
        }
        request_sense(d);
    }
    bd->nsectors = 0;
    return -EIO;
}

static int atapi_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_dev *d = bd->priv;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = MIN(count, 16u);
        uint8_t pkt[12] = { 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8),
                            (uint8_t)n, 0, 0, 0 };
        int want = (int)(n * ATAPI_SECTOR);
        int r = -EIO;
        for (int tries = 0; tries < 3 && r != want; tries++) {
            if (d->dma) {
                int dr = atapi_dma_read(d, pkt, p, (uint32_t)want);
                r = dr == 0 ? want : -EIO;
                if (dr == DMA_DEVERR) {
                    request_sense(d);           /* media change, not a DMA fault */
                } else if (dr < 0) {
                    klog("%s: DMA error at sector %u, falling back to PIO\n", bd->name, lba);
                    d->dma = 0;
                    chan_reset(d->ch);
                }
                continue;
            }
            r = atapi_packet(d, pkt, p, (uint32_t)want);
            if (r != want)
                request_sense(d);
        }
        if (r != want)
            return -EIO;
        lba += n;
        count -= n;
        p += n * ATAPI_SECTOR;
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

static int identify(struct ata_dev *d, uint16_t *id)
{
    if (inb(d->io + REG_STATUS) == 0xFF)
        return -1;                                      /* floating bus */
    select_drive(d, 0);
    outb(d->io + REG_COUNT, 0);
    outb(d->io + REG_LBA0, 0);
    outb(d->io + REG_LBA1, 0);
    outb(d->io + REG_LBA2, 0);
    outb(d->io + REG_COMMAND, CMD_IDENTIFY);
    uint8_t s = inb(d->io + REG_STATUS);
    if (s == 0 || s == 0xFF)
        return -1;
    if (wait_not_busy(d) < 0)
        return -1;

    uint8_t mid = inb(d->io + REG_LBA1), high = inb(d->io + REG_LBA2);
    if ((mid == 0x14 && high == 0xEB) || (mid == 0x69 && high == 0x96)) {
        d->atapi = 1;
        outb(d->io + REG_COMMAND, CMD_ID_PACKET);
        delay400(d);
    } else if (mid || high) {
        return -1;                                      /* SATA or unknown */
    }
    if (wait_drq(d) < 0)
        return -1;
    insw(d->io + REG_DATA, id, 256);
    return 0;
}

/*
 * Has the firmware put the drive in a DMA mode? We do not program
 * transfer modes ourselves (that would also need chipset timings), so a
 * drive left in PIO mode stays on PIO.
 */
static int dma_mode_selected(const uint16_t *id)
{
    if (!(id[49] & (1 << 8)))
        return 0;                                       /* no DMA support */
    if ((id[53] & (1 << 2)) && (id[88] & 0x7F00))
        return 1;                                       /* an Ultra DMA mode */
    if ((id[53] & (1 << 1)) && (id[63] & 0x0700))
        return 1;                                       /* a multiword DMA mode */
    return 0;
}

/* Find the PCI IDE controller's bus-master registers and enable DMA. */
static void dma_init(void)
{
    struct pci_dev ide;
    uint8_t prog_if;
    if (pci_find_class(0x0101, &ide, &prog_if) < 0 || !(prog_if & 0x80))
        return;                                         /* no bus mastering */
    uint32_t bar4 = pci_read32(&ide, PCI_BAR4);
    if (!(bar4 & 1) || !(bar4 & 0xFFFC))
        return;
    uint16_t bm = (uint16_t)(bar4 & 0xFFFC);

    /* One 64 KiB-aligned 64 KiB bounce buffer, carved out of 128 KiB. */
    uint32_t raw = pmm_alloc_contig(32);
    if (!raw)
        return;
    uint32_t aligned = ALIGN_UP(raw, 0x10000u);
    for (uint32_t f = raw; f < aligned; f += PAGE_SIZE)
        pmm_free(f);
    for (uint32_t f = aligned + 0x10000; f < raw + 0x20000; f += PAGE_SIZE)
        pmm_free(f);
    dma_bounce = (uint8_t *)aligned;

    for (int c = 0; c < 2; c++) {
        /* Only channels at the legacy ports this driver uses. */
        if (prog_if & (c ? 0x04 : 0x01))
            continue;
        uint32_t page = pmm_alloc();                    /* PRD tables: dword aligned */
        if (!page)
            continue;
        chans[c].prd = (uint32_t *)page;
        chans[c].bm = (uint16_t)(bm + 8 * c);
    }

    uint32_t cmd = pci_read32(&ide, PCI_COMMAND) & 0xFFFF;      /* keep RW1C status bits clear */
    pci_write32(&ide, PCI_COMMAND, cmd | PCI_CMD_IO | PCI_CMD_MASTER);

    char names[32] = "";
    for (int i = 0; i < 4; i++) {
        struct ata_dev *d = &drives[i];
        if (!d->bd.name[0] || !d->dma_capable || !d->ch->bm)
            continue;
        d->dma = 1;
        strlcat(names, names[0] ? " " : "", sizeof(names));
        strlcat(names, d->bd.name, sizeof(names));
    }
    if (names[0])
        boot_ok("IDE: bus-master DMA (%s)", names);
}

void ata_init(void)
{
    uint16_t *id = kmalloc(512);
    if (!id)
        return;

    for (int i = 0; i < 4; i++) {
        struct ata_dev *d = &drives[i];
        memset(d, 0, sizeof(*d));
        d->ch = &chans[i / 2];
        d->io = d->ch->io;
        d->ctrl = d->ch->ctrl;
        d->slave = i % 2;
        outb(d->ctrl, 0x02);                            /* nIEN: polled I/O */

        if (identify(d, id) < 0)
            continue;

        struct blockdev *bd = &d->bd;
        snprintf(bd->name, sizeof(bd->name), "hd%c", 'a' + i);
        ident_string(id, 27, 20, bd->model);
        bd->priv = d;
        if (d->atapi) {
            bd->type = BLK_CDROM;
            bd->sector_size = ATAPI_SECTOR;
            bd->readonly = 1;
            bd->read = atapi_read;
            bd->refresh = atapi_capacity;
            atapi_capacity(bd);
        } else {
            bd->type = BLK_DISK;
            bd->sector_size = 512;
            bd->read = ata_read;
            bd->write = ata_write;
            bd->flush = ata_flush_cache;
            d->lba48 = (id[83] & (1 << 10)) != 0;
            uint32_t lba28 = id[60] | ((uint32_t)id[61] << 16);
            uint32_t lba48 = id[100] | ((uint32_t)id[101] << 16);
            bd->nsectors = d->lba48 && (id[102] || id[103]) ? 0xFFFFFFFFu
                         : d->lba48 && lba48 ? lba48 : lba28;
        }
        d->dma_capable = dma_mode_selected(id);
        blk_register(bd);
        if (d->atapi)
            boot_ok("%s: ATAPI %s, %s", bd->name, bd->model,
                    bd->nsectors ? "media present" : "no media");
        else
            boot_ok("%s: ATA %s, %u MiB", bd->name, bd->model, bd->nsectors / 2048);
        if (!d->atapi)
            blk_scan_partitions(bd);
    }
    kfree(id);

    /* From here on, commands complete by interrupt (nIEN clear), unless
     * "ata.poll" on the command line asks for the polled fallback. */
    if (cmdline_has("ata.poll")) {
        boot_ok("IDE: polled I/O (ata.poll)");
    } else {
        for (int c = 0; c < 2; c++) {
            irq_register(chans[c].irq_line, ata_irq);
            outb(chans[c].ctrl, 0x00);
        }
        irq_mode = 1;
    }

    /* Data moves by DMA unless "ata.nodma" asks for PIO. */
    if (cmdline_has("ata.nodma"))
        boot_ok("IDE: PIO only (ata.nodma)");
    else
        dma_init();
}
