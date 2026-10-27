/*
 * USB mass storage: sticks, disks, card readers and CD drives that speak
 * the Bulk-Only Transport with SCSI commands (subclass 6, and the older
 * ATAPI-like 2 and 5).
 *
 * A command is a 31-byte wrapper (CBW) on the bulk OUT endpoint, the
 * data on bulk IN or OUT, and a 13-byte status (CSW) on bulk IN; a
 * device that gets confused is reset with the class reset request. Only
 * LUN 0 is used.
 *
 * Each device becomes a block device, sdX (or srN for a CD drive). An
 * unplugged device keeps its block device, emptied, and the next device
 * plugged in reuses it once nothing is mounted from it, so a stick that
 * is pulled and put back gets its old name. The automounter notices
 * through the media() hook and mounts or unmounts. I/O runs under the
 * VFS lock, and so do attaching and detaching, so a device never goes
 * away in the middle of a command.
 */
#include "kernel.h"
#include "blkdev.h"
#include "drivers.h"
#include "mm.h"
#include "usb.h"
#include "vfs.h"

#define CBW_SIG 0x43425355u
#define CSW_SIG 0x53425355u
#define MAX_MSC 8
#define IO_TIMEOUT 20000                /* ms: writes to a slow stick take a while */

#define T_READY    0
#define T_NOMEDIUM 1
#define T_CHANGED  2
#define T_BUSY     3

struct msc {
    struct blockdev bd;
    struct usb_dev *dev;                /* NULL while unplugged */
    struct usb_ep *in, *out;
    uint8_t iface;
    uint8_t cdb_len;                    /* 12 for the ATAPI-like subclasses */
    int removable;
    int changed;
    uint32_t tag;
    uint8_t *bounce;
    uint8_t cbw[32], csw[16];
};

static struct msc *pool[MAX_MSC];

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint32_t get32be(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void reset_recovery(struct msc *m)
{
    if (!m->dev || m->dev->gone)
        return;
    usb_control(m->dev, USB_RT_CLASS | USB_RT_INTERFACE, 0xFF, 0, m->iface, NULL, 0);
    usb_clear_halt(m->dev, m->in);
    usb_clear_halt(m->dev, m->out);
}

/*
 * One SCSI command. Returns 0, 1 for a check condition (the sense data
 * says why), or < 0 if the transport failed.
 */
static int scsi(struct msc *m, const uint8_t *cdb, void *data, uint32_t len, int in,
                uint32_t timeout)
{
    struct usb_dev *dev = m->dev;
    if (!dev || dev->gone)
        return -ENODEV;
    uint32_t addr = (uint32_t)data;
    void *buf = data;
    if (len && (addr < 0x100000 || addr + len > KERNEL_SPACE_END || addr + len < addr)) {
        buf = m->bounce;                /* not kernel memory the controller can reach */
        if (!in)
            memcpy(buf, data, len);
    }

    uint8_t *c = m->cbw;
    memset(c, 0, 31);
    put32(c, CBW_SIG);
    put32(c + 4, ++m->tag);
    put32(c + 8, len);
    c[12] = in ? 0x80 : 0;
    c[14] = m->cdb_len;
    memcpy(c + 15, cdb, m->cdb_len);
    if (usb_transfer(dev, m->out, c, 31, 5000) != 31) {
        reset_recovery(m);
        return -EIO;
    }

    if (len) {
        struct usb_ep *ep = in ? m->in : m->out;
        int r = usb_transfer(dev, ep, buf, len, timeout);
        if (r == -EPIPE) {
            usb_clear_halt(dev, ep);    /* the device ends the data early: the status says why */
        } else if (r < 0) {
            reset_recovery(m);
            return -EIO;
        } else if (in && buf != data) {
            memcpy(data, buf, (uint32_t)r);
        }
    }

    int r = usb_transfer(dev, m->in, m->csw, 13, 5000);
    if (r == -EPIPE) {
        usb_clear_halt(dev, m->in);
        r = usb_transfer(dev, m->in, m->csw, 13, 5000);
    }
    if (r != 13 || get32(m->csw) != CSW_SIG || get32(m->csw + 4) != m->tag || m->csw[12] > 1) {
        reset_recovery(m);
        return -EIO;
    }
    if (m->csw[12] == 1)
        return 1;
    return get32(m->csw + 8) ? 1 : 0;   /* data missing: count it as failed */
}

static void request_sense(struct msc *m, uint8_t *sense)
{
    uint8_t cdb[12] = { 0x03, 0, 0, 0, 18, 0 };
    uint8_t tmp[18];
    uint8_t *s = sense ? sense : tmp;
    memset(s, 0, 18);
    scsi(m, cdb, s, 18, 1, 5000);       /* the data may be shorter: fine */
}

static int test_ready(struct msc *m)
{
    static const uint8_t tur[12] = { 0 };
    int r = scsi(m, tur, NULL, 0, 0, 5000);
    if (r == 0)
        return T_READY;
    if (r < 0)
        return T_BUSY;
    uint8_t sense[18];
    request_sense(m, sense);
    int key = sense[2] & 15, asc = sense[12];
    if (key == 6)
        return T_CHANGED;
    if (key == 2 && asc == 0x3A)
        return T_NOMEDIUM;
    return T_BUSY;
}

static int read_capacity(struct msc *m)
{
    uint8_t cdb[12] = { 0x25, 0 };
    uint8_t cap[8];
    for (int tries = 0; tries < 3; tries++) {
        memset(cap, 0, sizeof(cap));
        int r = scsi(m, cdb, cap, 8, 1, 5000);
        if (r == 0) {
            uint32_t last = get32be(cap), bs = get32be(cap + 4);
            uint32_t want = m->bd.type == BLK_CDROM ? 2048 : 512;
            if (bs != want) {
                klog("usb: %s: %u-byte sectors are not supported\n", m->bd.name, bs);
                return -EINVAL;
            }
            m->bd.nsectors = last == 0xFFFFFFFFu ? 0xFFFFFFFFu : last + 1;
            return 0;
        }
        if (r < 0)
            return r;
        request_sense(m, NULL);
    }
    return -EIO;
}

/* ---- block device hooks ---- */

static int msc_rw(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf, int write)
{
    struct msc *m = bd->priv;
    uint8_t *p = buf;
    uint32_t ss = bd->sector_size;
    while (count) {
        uint32_t n = MIN(count, USB_MAX_XFER / ss);
        uint8_t cdb[12] = { write ? 0x2A : 0x28, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16),
                            (uint8_t)(lba >> 8), (uint8_t)lba, 0, (uint8_t)(n >> 8), (uint8_t)n };
        int r = -EIO;
        for (int tries = 0; tries < 3 && r != 0; tries++) {
            r = scsi(m, cdb, p, n * ss, !write, IO_TIMEOUT);
            if (r == -ENODEV)
                return -EIO;
            if (r == 1)
                request_sense(m, NULL);
        }
        if (r)
            return -EIO;
        lba += n;
        count -= n;
        p += n * ss;
    }
    return 0;
}

static int msc_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    return msc_rw(bd, lba, count, buf, 0);
}

static int msc_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    return msc_rw(bd, lba, count, (void *)buf, 1);
}

/* SYNCHRONIZE CACHE; plenty of sticks do not know it, which is fine. */
static int msc_flush(struct blockdev *bd)
{
    struct msc *m = bd->priv;
    uint8_t cdb[12] = { 0x35, 0 };
    int r = scsi(m, cdb, NULL, 0, 0, IO_TIMEOUT);
    if (r == 1)
        request_sense(m, NULL);
    return r < 0 ? -EIO : 0;
}

static int msc_refresh(struct blockdev *bd)
{
    struct msc *m = bd->priv;
    if (!m->dev || m->dev->gone)
        return -ENODEV;
    return bd->nsectors ? 0 : read_capacity(m);
}

/* Unplugged: nothing. Removable media (sticks say so too): ask the device. */
static int msc_media(struct blockdev *bd)
{
    struct msc *m = bd->priv;
    if (!m->dev || m->dev->gone)
        return MEDIA_NONE;
    if (m->removable) {
        int t = test_ready(m);
        if (t == T_CHANGED) {
            m->changed = 1;
            t = test_ready(m);
        }
        if (t == T_NOMEDIUM) {
            bd->nsectors = 0;
            return MEDIA_NONE;
        }
        if (t != T_READY)
            return MEDIA_BUSY;
        if (!bd->nsectors) {
            if (read_capacity(m) < 0)
                return MEDIA_BUSY;
            m->changed = 1;
        }
    }
    if (m->changed) {
        m->changed = 0;
        return MEDIA_CHANGED;
    }
    return MEDIA_PRESENT;
}

/* ---- attaching ---- */

static int in_use(struct msc *m)
{
    struct blockdev *bd;
    if (m->bd.fs)
        return 1;
    for (int i = 0; (bd = blk_get(i)); i++)
        if (bd->parent == &m->bd && bd->fs)
            return 1;
    return 0;
}

/* A free block device of this type: an unplugged one nothing is mounted
 * from, or a new one. */
static struct msc *get_slot(int type, int *fresh)
{
    for (int i = 0; i < MAX_MSC; i++) {
        if (pool[i] && !pool[i]->dev && pool[i]->bd.type == type && !in_use(pool[i])) {
            *fresh = 0;
            return pool[i];
        }
    }
    for (int i = 0; i < MAX_MSC; i++) {
        if (pool[i])
            continue;
        struct msc *m = kzalloc(sizeof(*m));
        if (!m || !(m->bounce = kmalloc(USB_MAX_XFER))) {
            kfree(m);
            return NULL;
        }
        pool[i] = m;
        *fresh = 1;
        return m;
    }
    return NULL;
}

static void trim(char *s)
{
    int n = (int)strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
}

static int msc_probe(struct usb_dev *dev, const struct usb_iface_desc *ifd,
                     const uint8_t *desc, uint32_t len, void **priv)
{
    if (ifd->bInterfaceClass != USB_CLASS_STORAGE || ifd->bInterfaceProtocol != 0x50)
        return -1;
    int sub = ifd->bInterfaceSubClass;
    if (sub != 6 && sub != 2 && sub != 5)
        return -1;
    struct usb_ep *in = usb_find_ep(dev, desc, len, USB_EP_BULK, 1);
    struct usb_ep *out = usb_find_ep(dev, desc, len, USB_EP_BULK, 0);
    if (!in || !out)
        return -1;

    /* A probe-only state to talk to the device before it has a slot. */
    struct msc *t = kzalloc(sizeof(*t));
    if (!t)
        return -1;
    t->dev = dev;
    t->in = in;
    t->out = out;
    t->iface = ifd->bInterfaceNumber;
    t->cdb_len = sub == 6 ? 10 : 12;
    t->bounce = kmalloc(512);
    mdelay(300);                        /* many sticks need a moment after configuration */

    uint8_t inq[36];
    uint8_t cdb[12] = { 0x12, 0, 0, 0, sizeof(inq), 0 };
    memset(inq, 0, sizeof(inq));
    if (!t->bounce || scsi(t, cdb, inq, sizeof(inq), 1, 5000) < 0) {
        klog("usb: storage device does not answer INQUIRY\n");
        kfree(t->bounce);
        kfree(t);
        return -1;
    }
    int ptype = inq[0] & 0x1F;
    if (ptype != 0x00 && ptype != 0x0E && ptype != 0x05) {
        klog("usb: storage device of SCSI type %d not supported\n", ptype);
        kfree(t->bounce);
        kfree(t);
        return -1;
    }
    int type = ptype == 0x05 ? BLK_CDROM : BLK_DISK;
    t->removable = (inq[1] & 0x80) != 0;
    t->bd.type = type;
    char model[44];
    char vendor[9], product[17];
    memcpy(vendor, inq + 8, 8);
    vendor[8] = 0;
    memcpy(product, inq + 16, 16);
    product[16] = 0;
    trim(vendor);
    trim(product);
    snprintf(model, sizeof(model), "%s%s%s", vendor, vendor[0] ? " " : "", product);

    /* Sticks report "becoming ready" or a unit attention at first. */
    int ready = T_BUSY;
    for (int i = 0; i < 10 && ready != T_READY && ready != T_NOMEDIUM; i++) {
        ready = test_ready(t);
        if (ready == T_BUSY)
            mdelay(100);
    }
    if (ready == T_READY)
        read_capacity(t);

    vfs_lock();
    int fresh;
    struct msc *m = get_slot(type, &fresh);
    if (!m) {
        vfs_unlock();
        klog("usb: too many storage devices\n");
        kfree(t->bounce);
        kfree(t);
        return -1;
    }
    uint8_t *bounce = m->bounce;
    struct blockdev saved = m->bd;
    m->dev = dev;
    m->in = in;
    m->out = out;
    m->iface = t->iface;
    m->cdb_len = t->cdb_len;
    m->removable = t->removable;
    m->tag = t->tag;
    m->bounce = bounce;
    m->bd = fresh ? t->bd : saved;
    m->bd.nsectors = t->bd.nsectors;
    strlcpy(m->bd.model, model, sizeof(m->bd.model));
    kfree(t->bounce);
    kfree(t);

    struct blockdev *bd = &m->bd;
    if (fresh) {
        blk_new_name(bd->name, sizeof(bd->name), type == BLK_CDROM ? "sr" : "sd",
                     type == BLK_CDROM);
        bd->sector_size = type == BLK_CDROM ? 2048 : 512;
        bd->readonly = type == BLK_CDROM;
        bd->read = msc_read;
        bd->write = type == BLK_CDROM ? NULL : msc_write;
        bd->flush = type == BLK_CDROM ? NULL : msc_flush;
        bd->refresh = msc_refresh;
        bd->media = msc_media;
        bd->priv = m;
    }
    int hotplug = usb_in_process();
    m->changed = hotplug;               /* new to the automounter; at boot it is mounted now */
    if (fresh && blk_register(bd) < 0) {
        m->dev = NULL;
        bd->type = -1;                  /* never registered: not to be reused */
        vfs_unlock();
        return -1;
    }
    if (hotplug)
        klog("usb: %s: %s, %u MiB\n", bd->name, bd->model,
             bd->nsectors / (1048576 / bd->sector_size));
    else
        boot_ok("%s: USB %s, %u MiB", bd->name, bd->model,
                bd->nsectors / (1048576 / bd->sector_size));
    if (!hotplug && type == BLK_DISK)
        blk_scan_partitions(bd);
    vfs_unlock();
    *priv = m;
    return 0;
}

static void msc_disconnect(struct usb_dev *dev, void *priv)
{
    (void)dev;
    struct msc *m = priv;
    vfs_lock();                         /* waits for a command in progress to end */
    m->dev = NULL;
    m->in = m->out = NULL;
    m->bd.nsectors = 0;
    vfs_unlock();
}

const struct usb_driver usbmsc_driver = {
    .name = "storage",
    .probe = msc_probe,
    .disconnect = msc_disconnect,
};
