/*
 * IDE controller driver: ATA hard disks (PIO, LBA28/LBA48, read/write)
 * and ATAPI CD/DVD drives (PACKET commands, read-only).
 *
 * Once processes exist, a command's data phases are interrupt driven:
 * the caller sleeps until the drive raises IRQ14/15, so other processes
 * run while the disk works. Boot-time probing polls instead. Callers
 * serialise through the VFS lock, so one command is in flight at a time.
 *
 * Device names follow the old Linux IDE scheme: hda/hdb are the
 * primary master/slave, hdc/hdd the secondary ones.
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "mm.h"
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
#define CMD_FLUSH       0xE7
#define CMD_FLUSH_EXT   0xEA
#define CMD_PACKET      0xA0
#define CMD_ID_PACKET   0xA1
#define CMD_IDENTIFY    0xEC

#define ATAPI_SECTOR 2048
#define TIMEOUT      2000000
#define IRQ_TIMEOUT  (5 * HZ)          /* then fall back to polling */

struct ata_chan {
    uint16_t io, ctrl;
    int irq_line;
    volatile int armed;                 /* a command is waiting for an IRQ */
    volatile int fired;
};

static struct ata_chan chans[2] = {
    { 0x1F0, 0x3F6, 14, 0, 0 },
    { 0x170, 0x376, 15, 0, 0 },
};
static int irq_mode;

struct ata_dev {
    uint16_t io, ctrl;
    struct ata_chan *ch;
    int slave;
    int atapi;
    int lba48;
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

/* ---- ATA disks ---- */

static int ata_pio(struct ata_dev *d, uint32_t lba, uint32_t count, void *buf, int write)
{
    int ext = d->lba48 && (lba + count > 0x0FFFFFFF);
    if (wait_not_busy(d) < 0)
        return -EIO;
    arm(d);
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
        outb(d->io + REG_COMMAND, write ? CMD_WRITE_EXT : CMD_READ_EXT);
    } else {
        select_drive(d, (uint8_t)(0x40 | ((lba >> 24) & 0x0F)));
        outb(d->io + REG_COUNT, (uint8_t)count);
        outb(d->io + REG_LBA0, (uint8_t)lba);
        outb(d->io + REG_LBA1, (uint8_t)(lba >> 8));
        outb(d->io + REG_LBA2, (uint8_t)(lba >> 16));
        outb(d->io + REG_COMMAND, write ? CMD_WRITE : CMD_READ);
    }

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
        arm(d);
        outb(d->io + REG_COMMAND, ext ? CMD_FLUSH_EXT : CMD_FLUSH);
        st = wait_irq(d);
        if (st < 0 || (st & (ST_ERR | ST_DF)))
            goto out;
    }
    r = 0;
out:
    disarm(d);
    return r;
}

static int ata_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    struct ata_dev *d = bd->priv;
    uint8_t *p = buf;
    while (count) {
        uint32_t n = MIN(count, 128u);
        if (ata_pio(d, lba, n, p, 0) < 0)
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
        if (ata_pio(d, lba, n, (void *)p, 1) < 0)
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
        int r = -EIO;
        for (int tries = 0; tries < 3 && r != (int)(n * ATAPI_SECTOR); tries++) {
            r = atapi_packet(d, pkt, p, n * ATAPI_SECTOR);
            if (r != (int)(n * ATAPI_SECTOR))
                request_sense(d);
        }
        if (r != (int)(n * ATAPI_SECTOR))
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
            d->lba48 = (id[83] & (1 << 10)) != 0;
            uint32_t lba28 = id[60] | ((uint32_t)id[61] << 16);
            uint32_t lba48 = id[100] | ((uint32_t)id[101] << 16);
            bd->nsectors = d->lba48 && (id[102] || id[103]) ? 0xFFFFFFFFu
                         : d->lba48 && lba48 ? lba48 : lba28;
        }
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
        return;
    }
    for (int c = 0; c < 2; c++) {
        irq_register(chans[c].irq_line, ata_irq);
        outb(chans[c].ctrl, 0x00);
    }
    irq_mode = 1;
}
