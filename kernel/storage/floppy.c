/*
 * Floppy disk driver for the PC's floppy controller (an 82077AA or a
 * compatible one in the Super I/O chip, as QEMU, Bochs and VirtualBox
 * emulate): up to two drives on the controller at 0x3F0, registered as
 * the removable block devices /dev/fd0 and /dev/fd1.
 *
 * Data moves by ISA DMA on channel 2 through a buffer below 16 MiB that
 * holds one side of a track. A read fetches the whole track side, which
 * stays cached: a floppy turns five times a second, and reading its
 * sectors one by one would wait most of a revolution for each.
 * Commands complete by IRQ 6, and the caller sleeps meanwhile, so the
 * driver is only used from process context (callers hold the VFS lock).
 *
 * The drive types come from CMOS register 0x10. The format of a disk
 * is found when it is first used: the data rates the drive supports are
 * tried with READ ID, then the geometry is taken from the FAT boot
 * sector if it has one (so 1.68M DMF disks work too).
 *
 * A disk that is changed while mounted is not trusted: its cached
 * sectors are dropped and I/O fails until it is unmounted (the automounter,
 * automount.c, does that, and mounts the next disk). The kfloppyd thread
 * turns the motor off after 3 seconds without access, and at boot creates
 * /mnt/fdN and mounts a disk that is already in a drive.
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "fs.h"
#include "proc.h"
#include "vfs.h"
#include "x86.h"

#define FDC_DOR   0x3F2                 /* digital output: motors, select, reset */
#define FDC_MSR   0x3F4                 /* main status */
#define FDC_FIFO  0x3F5
#define FDC_DIR   0x3F7                 /* read: digital input, bit 7 = disk changed */
#define FDC_CCR   0x3F7                 /* write: data rate */

#define DOR_NRESET 0x04
#define DOR_DMA    0x08                 /* DMA and IRQ enable */
#define MSR_RQM    0x80                 /* ready for a byte */
#define MSR_DIO    0x40                 /* controller -> CPU */
#define DIR_CHANGE 0x80

#define CMD_SPECIFY     0x03
#define CMD_SENSE_DRIVE 0x04
#define CMD_WRITE       0x45            /* MFM */
#define CMD_READ        0x46            /* MFM */
#define CMD_RECALIBRATE 0x07
#define CMD_SENSE_INT   0x08
#define CMD_READ_ID     0x4A            /* MFM */
#define CMD_SEEK        0x0F

#define ST0_IC     0xC0                 /* interrupt code: 00 = normal termination */
#define ST0_SEEK   0x20
#define ST1_EN     0x80                 /* end of cylinder */
#define ST1_NW     0x02                 /* write protected */
#define ST3_WP     0x40

#define SECTOR      512
#define MAX_SPT     36
#define IRQ_FLOPPY  6
#define SPINS       100000              /* MSR polls before giving up on a byte */
#define CMD_TIMEOUT (2 * HZ)
#define SPINUP      (HZ / 2)
#define MOTOR_IDLE  (3 * HZ)
#define REPROBE     (2 * HZ)            /* an unmounted drive is probed at most this often */
#define DMA_LIMIT   0x1000000u          /* ISA DMA reaches the first 16 MiB */

struct fd_format {
    const char *name;
    uint8_t spt, heads, tracks;
    uint8_t rate;                       /* CCR value: 0 = 500, 1 = 300, 2 = 250 kbit/s */
    uint8_t gap;                        /* gap length for reads and writes */
    uint8_t double_step;                /* 40-track disk in an 80-track drive */
};

static const struct fd_format fmt_1440 = { "1.44M", 18, 2, 80, 0, 0x1B, 0 };
static const struct fd_format fmt_1200 = { "1.2M", 15, 2, 80, 0, 0x1B, 0 };
static const struct fd_format fmt_720 = { "720K", 9, 2, 80, 2, 0x2A, 0 };
static const struct fd_format fmt_360 = { "360K", 9, 2, 40, 2, 0x2A, 0 };
static const struct fd_format fmt_360_hd = { "360K", 9, 2, 40, 1, 0x2A, 1 };

/* What to try in each CMOS drive type, most likely first. 2.88M drives
 * are driven as 1.44M ones: ED media would need perpendicular mode. */
static const struct fd_format *const type_formats[7][3] = {
    [1] = { &fmt_360 },
    [2] = { &fmt_1200, &fmt_360_hd },
    [3] = { &fmt_720 },
    [4] = { &fmt_1440, &fmt_720 },
    [5] = { &fmt_1440, &fmt_720 },
    [6] = { &fmt_1440, &fmt_720 },
};

static const char *const type_names[7] = {
    NULL, "5.25\" 360K", "5.25\" 1.2M", "3.5\" 720K", "3.5\" 1.44M", "3.5\" 2.88M", "3.5\" 2.88M",
};

struct fd_drive {
    int unit;
    int type;                           /* CMOS drive type */
    const struct fd_format *fmt;        /* data rate, gap, stepping of the disk */
    uint32_t spt, heads, tracks;        /* its geometry */
    int cyl;                            /* where the head is, -1 = unknown */
    int stale;                          /* changed while mounted: refuse I/O */
    int disk_in;                        /* the automounter saw a disk go in (readable or not) */
    int probed;
    uint32_t probe_tick;
    struct blockdev bd;
};

static struct fd_drive drives[2];
static int ndrives;
static int need_reset = 1;
static uint8_t motors;                  /* DOR motor bits */
static uint8_t selected;
static int cur_rate = -1;
static uint32_t last_use;
static volatile int fired;
static int nopoll;                      /* floppy.nopoll: never look for new disks */

/* One side of a track, in memory the DMA controller can reach: below
 * 16 MiB (the kernel is loaded at 1 MiB) and inside one 64 KiB page. */
static uint8_t track_buf[MAX_SPT * SECTOR] __attribute__((aligned(32768)));
static struct fd_drive *cache_drive;    /* whose track track_buf holds, if any */
static uint32_t cache_cyl, cache_head;

static uint32_t rd16(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | rd16(p + 2) << 16; }

static int in_process(void)
{
    return current && current->pid > 0;
}

/* Sleep for n ticks, even if a signal wakes us early. */
static void fd_sleep(uint32_t n)
{
    uint32_t deadline = ticks + n;
    while ((int32_t)(ticks - deadline) < 0)
        sleep_ticks((uint32_t)(deadline - ticks));
}

static void fd_irq(struct trapframe *tf)
{
    (void)tf;
    fired = 1;
    wakeup((void *)&fired);
}

/* `fired` is cleared before a command starts; wait_event looks at it
 * after preparing to sleep, so an IRQ in between is not missed. */
static int wait_irq(uint32_t timeout)
{
    wait_event_timeout((void *)&fired, fired, ticks + timeout);
    int ok = fired;
    fired = 0;
    return ok ? 0 : -EIO;
}

static void write_dor(void)
{
    outb(FDC_DOR, (uint8_t)(motors | DOR_NRESET | DOR_DMA | selected));
}

static int fdc_out(uint8_t b)
{
    for (int i = 0; i < SPINS; i++)
        if ((inb(FDC_MSR) & (MSR_RQM | MSR_DIO)) == MSR_RQM) {
            outb(FDC_FIFO, b);
            return 0;
        }
    need_reset = 1;
    return -EIO;
}

static int fdc_in(void)
{
    for (int i = 0; i < SPINS; i++)
        if ((inb(FDC_MSR) & (MSR_RQM | MSR_DIO)) == (MSR_RQM | MSR_DIO))
            return inb(FDC_FIFO);
    need_reset = 1;
    return -EIO;
}

static int fdc_cmd(const uint8_t *bytes, int n)
{
    for (int i = 0; i < n; i++)
        if (fdc_out(bytes[i]) < 0)
            return -EIO;
    return 0;
}

static int sense_interrupt(int *st0, int *cyl)
{
    if (fdc_out(CMD_SENSE_INT) < 0)
        return -EIO;
    *st0 = fdc_in();
    *cyl = fdc_in();
    return *st0 < 0 || *cyl < 0 ? -EIO : 0;
}

static void set_rate(int rate)
{
    if (rate != cur_rate) {
        outb(FDC_CCR, (uint8_t)rate);
        cur_rate = rate;
    }
}

static int fdc_reset(void)
{
    int st0, cyl;
    fired = 0;
    outb(FDC_DOR, 0);
    for (int i = 0; i < 10; i++)
        io_wait();
    write_dor();
    int r = wait_irq(HZ);
    for (int i = 0; i < 4; i++)         /* one status per drive after a reset */
        sense_interrupt(&st0, &cyl);
    cur_rate = -1;
    set_rate(0);
    /* Step rate 3 ms, head unload 240 ms, head load 4 ms, DMA mode. */
    static const uint8_t specify[] = { CMD_SPECIFY, 0xDF, 0x02 };
    if (r == 0)
        r = fdc_cmd(specify, sizeof(specify));
    for (int i = 0; i < ndrives; i++)
        drives[i].cyl = -1;
    need_reset = r < 0;
    if (r < 0)
        klog("floppy: controller reset failed\n");
    return r;
}

static void motor_on(struct fd_drive *d)
{
    uint8_t bit = (uint8_t)(0x10 << d->unit);
    selected = (uint8_t)d->unit;
    if (!(motors & bit)) {
        motors |= bit;
        write_dor();
        fd_sleep(SPINUP);
    } else {
        write_dor();
    }
    last_use = ticks;
}

static int recalibrate(struct fd_drive *d)
{
    /* One recalibrate steps at most 77 times: an 80-track drive may need two. */
    for (int tries = 0; tries < 2; tries++) {
        uint8_t cmd[] = { CMD_RECALIBRATE, (uint8_t)d->unit };
        int st0, cyl;
        fired = 0;
        if (fdc_cmd(cmd, sizeof(cmd)) < 0 || wait_irq(3 * HZ) < 0 ||
            sense_interrupt(&st0, &cyl) < 0) {
            need_reset = 1;
            return -EIO;
        }
        if (!(st0 & ST0_IC) && cyl == 0) {
            d->cyl = 0;
            return 0;
        }
    }
    d->cyl = -1;
    return -EIO;
}

static int seek(struct fd_drive *d, int cyl)
{
    uint8_t cmd[] = { CMD_SEEK, (uint8_t)d->unit, (uint8_t)cyl };
    int st0, pcn;
    fired = 0;
    if (fdc_cmd(cmd, sizeof(cmd)) < 0 || wait_irq(3 * HZ) < 0 ||
        sense_interrupt(&st0, &pcn) < 0) {
        need_reset = 1;
        d->cyl = -1;
        return -EIO;
    }
    if ((st0 & ST0_IC) || !(st0 & ST0_SEEK) || pcn != cyl) {
        d->cyl = -1;
        return -EIO;
    }
    d->cyl = cyl;
    fd_sleep(2);                        /* head settle time */
    return 0;
}

/* Read the next sector ID under head 0: proves the data rate matches the disk. */
static int read_id(struct fd_drive *d)
{
    uint8_t cmd[] = { CMD_READ_ID, (uint8_t)d->unit };
    uint8_t res[7];
    fired = 0;
    if (fdc_cmd(cmd, sizeof(cmd)) < 0)
        return -EIO;
    if (wait_irq(HZ) < 0) {
        need_reset = 1;                 /* no disk spinning: the command never ends */
        return -EIO;
    }
    for (int i = 0; i < 7; i++) {
        int b = fdc_in();
        if (b < 0)
            return -EIO;
        res[i] = (uint8_t)b;
    }
    return (res[0] & ST0_IC) ? -EIO : 0;
}

static void dma_setup(uint32_t addr, uint32_t len, int to_device)
{
    spin_lock(&isa_dma_lock);           /* the sound card uses this controller too */
    outb(0x0A, 0x06);                   /* mask channel 2 */
    outb(0x0C, 0xFF);                   /* reset the byte flip-flop */
    outb(0x04, (uint8_t)addr);
    outb(0x04, (uint8_t)(addr >> 8));
    outb(0x81, (uint8_t)(addr >> 16));  /* page register */
    outb(0x0C, 0xFF);
    outb(0x05, (uint8_t)(len - 1));
    outb(0x05, (uint8_t)((len - 1) >> 8));
    /* Single transfers, incrementing, channel 2: 0x46 writes memory (disk
     * read), 0x4A reads memory (disk write). */
    outb(0x0B, to_device ? 0x4A : 0x46);
    outb(0x0A, 0x02);                   /* unmask channel 2 */
    spin_unlock(&isa_dma_lock);
}

/* One READ or WRITE of sectors [sector, sector + count) of a track side,
 * to or from the same place in track_buf. The head is already there. */
static int transfer(struct fd_drive *d, uint32_t cyl, uint32_t head, uint32_t sector,
                    uint32_t count, int write)
{
    dma_setup((uint32_t)track_buf + (sector - 1) * SECTOR, count * SECTOR, write);
    set_rate(d->fmt->rate);
    uint8_t cmd[] = {
        write ? CMD_WRITE : CMD_READ, (uint8_t)(head << 2 | (uint32_t)d->unit),
        (uint8_t)cyl, (uint8_t)head, (uint8_t)sector, 2 /* 512 bytes */,
        (uint8_t)(sector + count - 1), d->fmt->gap, 0xFF,
    };
    uint8_t res[7];
    fired = 0;
    if (fdc_cmd(cmd, sizeof(cmd)) < 0)
        return -EIO;
    if (wait_irq(CMD_TIMEOUT) < 0) {
        need_reset = 1;
        return -EIO;
    }
    for (int i = 0; i < 7; i++) {
        int b = fdc_in();
        if (b < 0)
            return -EIO;
        res[i] = (uint8_t)b;
    }
    if (!(res[0] & ST0_IC))
        return 0;
    /* Stopping at the last sector before the DMA count ran out still
     * moved everything asked for. */
    if ((res[0] & ST0_IC) == 0x40 && res[1] == ST1_EN && res[2] == 0)
        return 0;
    if (res[1] & ST1_NW)
        return -EROFS;
    return -EIO;
}

/* Transfer within one track side, with retries: a recalibrate after a
 * failure, and a controller reset after two. */
static int track_io(struct fd_drive *d, uint32_t cyl, uint32_t head, uint32_t sector,
                    uint32_t count, int write)
{
    int r = -EIO;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (attempt == 2)
            need_reset = 1;
        if (need_reset && fdc_reset() < 0)
            continue;
        if (d->cyl < 0 && recalibrate(d) < 0)
            continue;
        int phys = (int)cyl * (d->fmt->double_step ? 2 : 1);
        if (d->cyl != phys && seek(d, phys) < 0)
            continue;
        r = transfer(d, cyl, head, sector, count, write);
        if (r == 0 || r == -EROFS)
            return r;
        d->cyl = -1;
        r = -EIO;
    }
    return r;
}

static void geometry_from_boot_sector(struct fd_drive *d, const uint8_t *b)
{
    uint32_t spt = rd16(b + 24), heads = rd16(b + 26);
    uint32_t total = rd16(b + 19) ? rd16(b + 19) : rd32(b + 32);
    if (rd16(b + 11) != SECTOR || spt < 8 || spt > MAX_SPT || heads < 1 || heads > 2 || !total)
        return;
    uint32_t tracks = total / (spt * heads);
    if (tracks < 35 || tracks > 83 || tracks * spt * heads != total)
        return;
    d->spt = spt;
    d->heads = heads;
    d->tracks = tracks;
}

/*
 * Find out whether there is a disk and how it is formatted. The disk
 * change line stays set while the drive is empty; with a disk in, a
 * step clears it.
 */
static void probe(struct fd_drive *d)
{
    d->probed = 1;
    d->probe_tick = ticks;
    d->bd.nsectors = 0;
    cache_drive = NULL;                 /* the probe reads into track_buf */
    motor_on(d);
    if (need_reset && fdc_reset() < 0)
        return;
    if (inb(FDC_DIR) & DIR_CHANGE) {
        if (seek(d, 1) < 0 || recalibrate(d) < 0) {
            d->cyl = -1;
        }
        if (inb(FDC_DIR) & DIR_CHANGE)
            return;                     /* no disk */
    } else if (d->cyl < 0 && recalibrate(d) < 0) {
        return;
    }

    for (int i = 0; i < 3 && type_formats[d->type][i]; i++) {
        const struct fd_format *f = type_formats[d->type][i];
        set_rate(f->rate);
        if (read_id(d) < 0) {
            if (need_reset)
                fdc_reset();
            continue;
        }
        d->fmt = f;
        d->spt = f->spt;
        d->heads = f->heads;
        d->tracks = f->tracks;
        if (track_io(d, 0, 0, 1, 1, 0) < 0)
            return;                     /* unreadable */
        geometry_from_boot_sector(d, track_buf);
        d->bd.nsectors = d->spt * d->heads * d->tracks;

        uint8_t cmd[] = { CMD_SENSE_DRIVE, (uint8_t)d->unit };
        int st3 = fdc_cmd(cmd, sizeof(cmd)) == 0 ? fdc_in() : -1;
        d->bd.readonly = st3 >= 0 && (st3 & ST3_WP);
        klog("%s: %s disk, %u sectors per track%s\n", d->bd.name, f->name, d->spt,
             d->bd.readonly ? ", write protected" : "");
        return;
    }
}

/* Called before each access: notices a disk that was removed or swapped. */
static int check_disk(struct fd_drive *d)
{
    if (d->stale) {
        if (d->bd.fs)
            return -EIO;
        d->stale = 0;
        probe(d);
    } else if (inb(FDC_DIR) & DIR_CHANGE) {
        if (cache_drive == d)
            cache_drive = NULL;
        if (d->bd.fs) {
            klog("%s: disk changed while mounted; unmount it before use\n", d->bd.name);
            d->stale = 1;
            blk_discard(&d->bd);
            return -EIO;
        }
        blk_discard(&d->bd);            /* what is cached is of the old disk */
        probe(d);
    }
    return d->bd.nsectors ? 0 : -EIO;
}

static int fd_rw(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf, int write)
{
    struct fd_drive *d = bd->priv;
    uint8_t *p = buf;
    if (!in_process())
        return -EIO;
    motor_on(d);
    int r = check_disk(d);
    if (r < 0)
        return r;
    while (count) {
        uint32_t track = lba / d->spt;
        uint32_t cyl = track / d->heads, head = track % d->heads, sector = lba % d->spt + 1;
        uint32_t n = MIN(count, d->spt - (sector - 1));
        int cached = cache_drive == d && cache_cyl == cyl && cache_head == head;
        if (write) {
            /* Write through, keeping a cached copy of this track current. */
            if (!cached)
                cache_drive = NULL;
            memcpy(track_buf + (sector - 1) * SECTOR, p, n * SECTOR);
            r = track_io(d, cyl, head, sector, n, 1);
            if (r < 0) {
                cache_drive = NULL;
                return r;
            }
        } else {
            if (!cached) {
                cache_drive = NULL;
                r = track_io(d, cyl, head, 1, d->spt, 0);
                if (r < 0)
                    return r;
                cache_drive = d;
                cache_cyl = cyl;
                cache_head = head;
            }
            memcpy(p, track_buf + (sector - 1) * SECTOR, n * SECTOR);
        }
        p += n * SECTOR;
        lba += n;
        count -= n;
        last_use = ticks;
    }
    return 0;
}

static int fd_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    return fd_rw(bd, lba, count, buf, 0);
}

static int fd_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    return fd_rw(bd, lba, count, (void *)buf, 1);
}

/* The size of the disk in the drive. A mounted disk is not probed again. */
static int fd_refresh(struct blockdev *bd)
{
    struct fd_drive *d = bd->priv;
    if (bd->fs || !in_process())
        return 0;
    if (d->probed && !d->stale && (uint32_t)(ticks - d->probe_tick) < REPROBE)
        return 0;
    d->stale = 0;
    probe(d);
    return 0;
}

/*
 * The automounter's look at a drive. While a disk is believed to be in, only
 * the disk change line is read, with the drive selected for an instant: it
 * stays clear until the disk is taken out. An empty drive's line stays set
 * whether or not a disk is then put in, until a step pulse clears it, so
 * for an empty drive the head is stepped out and back to see if it does.
 * (That is how Linux notices floppies too, and it is why "floppy.nopoll" on
 * the kernel command line turns the polling off: on a real drive each look
 * at an empty one makes the head click.) A disk that has appeared is
 * probed by fd_refresh when the caller mounts it.
 */
static int fd_media(struct blockdev *bd)
{
    struct fd_drive *d = bd->priv;
    if (!in_process() || nopoll)
        return MEDIA_BUSY;
    int believed = d->disk_in || bd->nsectors != 0 || bd->fs != NULL;
    if (believed && d->stale)
        return MEDIA_CHANGED;           /* check_disk saw it first */

    uint8_t bit = (uint8_t)(0x10 << d->unit);
    int had_motor = (motors & bit) != 0;
    if (need_reset && fdc_reset() < 0)
        return MEDIA_BUSY;
    selected = (uint8_t)d->unit;
    motors |= bit;
    write_dor();
    for (int i = 0; i < 1000; i++)
        io_wait();                      /* about a millisecond for the drive's outputs to settle */
    int line = (inb(FDC_DIR) & DIR_CHANGE) != 0;
    if (!believed && line) {
        /* Empty, or a disk nobody has stepped yet: step out and back. */
        if (seek(d, 1) < 0 || recalibrate(d) < 0)
            d->cyl = -1;
        line = (inb(FDC_DIR) & DIR_CHANGE) != 0;
    }
    if (!had_motor) {
        motors &= (uint8_t)~bit;
        write_dor();
    }

    if (believed && !line)
        return MEDIA_PRESENT;
    if (!believed && line) {
        d->disk_in = 0;
        return MEDIA_NONE;
    }
    /* A disk was taken out or swapped, or one has been put in. */
    d->probed = 0;                      /* the next fd_refresh looks at once */
    if (cache_drive == d)
        cache_drive = NULL;
    d->disk_in = !line;
    if (line) {                         /* taken out (the mounted one is dropped by the caller) */
        if (!bd->fs)
            bd->nsectors = 0;
        return MEDIA_NONE;
    }
    return MEDIA_CHANGED;
}

static uint8_t cmos_read(uint8_t reg)
{
    outb(0x70, reg);
    io_wait();
    return inb(0x71);
}

void floppy_init(void)
{
    if ((uint32_t)track_buf + sizeof(track_buf) > DMA_LIMIT) {
        klog("floppy: no DMA buffer below 16 MiB\n");
        return;
    }
    nopoll = cmdline_has("floppy.nopoll");
    uint8_t types = cmos_read(0x10);
    if (!types || inb(FDC_MSR) == 0xFF)
        return;                         /* no drives, or no controller */
    for (int unit = 0; unit < 2; unit++) {
        int type = unit == 0 ? types >> 4 : types & 0x0F;
        if (type < 1 || type > 6)
            continue;
        struct fd_drive *d = &drives[ndrives++];
        d->unit = unit;
        d->type = type;
        d->fmt = type_formats[type][0];
        d->cyl = -1;
        struct blockdev *bd = &d->bd;
        snprintf(bd->name, sizeof(bd->name), "fd%d", unit);
        snprintf(bd->model, sizeof(bd->model), "%s floppy drive", type_names[type]);
        bd->type = BLK_FLOPPY;
        bd->sector_size = SECTOR;
        bd->read = fd_read;
        bd->write = fd_write;
        bd->refresh = fd_refresh;
        bd->media = fd_media;
        bd->priv = d;
        blk_register(bd);
        boot_ok("%s: %s floppy drive", bd->name, type_names[type]);
    }
    if (ndrives)
        irq_register(IRQ_FLOPPY, fd_irq);
}

/*
 * Kernel thread: create the mount points and mount a disk that is in a
 * drive at boot (from here, not at boot, because probing sleeps), then
 * turn the motors off when the drives have been idle a while.
 */
static void kfloppyd(void)
{
    vfs_lock();
    for (int i = 0; i < ndrives; i++) {
        struct blockdev *bd = &drives[i].bd;
        char path[16];
        struct vnode *mp;
        snprintf(path, sizeof(path), "mnt/%s", bd->name);
        if (vfs_resolve(path, NULL, &mp) < 0 && vfs_mkdir_p(path, &mp) < 0)
            continue;
        snprintf(path, sizeof(path), "/mnt/%s", bd->name);
        if (!bd->fs && vfs_mount_dev(bd, path, "auto") == 0)
            klog("Mounted /dev/%s (%s) on %s\n", bd->name, bd->fs->name, path);
    }
    vfs_unlock();

    for (;;) {
        fd_sleep(HZ);
        current->sig_pending = 0;
        if (!motors)
            continue;
        vfs_lock();
        if (motors && (uint32_t)(ticks - last_use) >= MOTOR_IDLE) {
            motors = 0;
            write_dor();
        }
        vfs_unlock();
    }
}

void floppy_start(void)
{
    if (ndrives)
        proc_spawn_kthread("kfloppyd", kfloppyd);
}
