/*
 * The Yamaha OPL2/OPL3 FM synthesizer at 388h: the AdLib's chip, and the
 * Sound Blaster 16's (an OPL3, whose second bank of registers is at 38Ah).
 *
 * /dev/opl takes struct opl_write records and sends them to the chip in
 * order, with the waits the chip needs between writes. One process at a
 * time may have it open; when it closes it, every voice is silenced, so
 * a program that stops in the middle of a tune leaves no hanging notes.
 * The DOS emulator passes a program's AdLib writes through here.
 */
#include "kernel.h"
#include "drivers.h"
#include "proc.h"
#include "syscall.h"
#include "vfs.h"
#include "x86.h"

#define OPL_BASE 0x388

static int opl_type;                    /* 0: none, 2: OPL2, 3: OPL3 */
static int opl_owner;                   /* pid with /dev/opl open */

/* Waits measured in reads of the status port (about 1 us each): the OPL2
 * needs 3.3 us after an address and 23 us after data, the OPL3 far less. */
static void opl_wait(int reads)
{
    for (int i = 0; i < reads; i++)
        (void)inb(OPL_BASE);
}

static void opl_out(uint16_t reg, uint8_t val)
{
    uint16_t port = (uint16_t)(OPL_BASE + ((reg & 0x100) ? 2 : 0));
    outb(port, (uint8_t)reg);
    opl_wait(opl_type == 3 ? 2 : 6);
    outb((uint16_t)(port + 1), val);
    opl_wait(opl_type == 3 ? 2 : 35);
}

/* Key every voice off, turn every operator down and leave OPL3 mode. */
static void opl_silence(void)
{
    int banks = opl_type == 3 ? 2 : 1;
    for (int b = 0; b < banks; b++) {
        uint16_t bank = (uint16_t)(b << 8);
        for (int ch = 0; ch < 9; ch++)
            opl_out(bank | (uint16_t)(0xB0 + ch), 0);
        for (int op = 0x40; op <= 0x55; op++)
            opl_out(bank | (uint16_t)op, 0x3F);
        opl_out(bank | 0xBD, 0);
    }
    if (opl_type == 3) {
        opl_out(0x104, 0);              /* no 4-operator voices */
        opl_out(0x105, 0);              /* OPL2 compatibility mode */
    }
    opl_out(0x01, 0);
    opl_out(0x08, 0);
}

/* The AdLib detection: start timer 1 and see its flag go up. */
static int opl_detect(void)
{
    opl_out(0x04, 0x60);                /* mask and reset both timers */
    opl_out(0x04, 0x80);
    uint8_t s1 = inb(OPL_BASE);
    opl_out(0x02, 0xFF);                /* timer 1 runs out after 80 us */
    opl_out(0x04, 0x21);
    for (int i = 0; i < 200; i++)
        io_wait();
    uint8_t s2 = inb(OPL_BASE);
    opl_out(0x04, 0x60);
    opl_out(0x04, 0x80);
    if ((s1 & 0xE0) != 0 || (s2 & 0xE0) != 0xC0)
        return 0;
    /* An OPL2 reads 06h in the low bits of its status; an OPL3 reads 0. */
    return (s2 & 0x06) ? 2 : 3;
}

int opl_present(void)
{
    return opl_type;
}

static int opl_open(struct vnode *vn, int flags)
{
    (void)vn; (void)flags;
    if (opl_owner && opl_owner != current->pid) {
        struct proc *p = proc_find(opl_owner);
        if (p && p->state != P_ZOMBIE)
            return -EBUSY;
    }
    opl_owner = current->pid;
    return 0;
}

static void opl_close(struct vnode *vn, int flags)
{
    (void)vn; (void)flags;
    opl_silence();
    opl_owner = 0;
}

static int opl_dev_write(struct vnode *vn, const void *buf, uint64_t off, uint32_t n)
{
    (void)vn; (void)off;
    const struct opl_write *w = buf;
    uint32_t count = n / sizeof(*w);
    for (uint32_t i = 0; i < count; i++) {
        if (w[i].reg < 0x100 || (w[i].reg < 0x200 && opl_type == 3))
            opl_out(w[i].reg, w[i].val);
    }
    return (int)(count * sizeof(*w));
}

static int opl_ioctl(struct vnode *vn, uint32_t req, uint32_t arg)
{
    (void)vn;
    switch (req) {
    case OPL_RESET:
        opl_silence();
        return 0;
    case SOUND_INFO:
        if (!user_access_ok((void *)arg, sizeof(struct sound_info)))
            return -EFAULT;
        sound_get_info((struct sound_info *)arg);
        return 0;
    }
    return -EINVAL;
}

void opl_init(struct vnode *devdir)
{
    if (cmdline_has("opl=off"))
        return;
    opl_type = opl_detect();
    if (!opl_type)
        return;
    opl_silence();
    struct vnode *vn = vfs_mknod(devdir, "opl", MKDEV(SOUND_MAJOR, SOUND_MINOR_OPL), NULL, opl_dev_write);
    if (vn) {
        vn->dev_open = opl_open;
        vn->dev_close = opl_close;
        vn->dev_ioctl = opl_ioctl;
    }
    boot_ok("FM synthesizer: Yamaha %s at %xh", opl_type == 3 ? "OPL3" : "OPL2", OPL_BASE);
}
