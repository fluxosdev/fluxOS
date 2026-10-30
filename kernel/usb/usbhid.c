/*
 * USB keyboards, mice and tablets (HID class).
 *
 * Keyboards are put in the boot protocol, whose 8-byte reports list the
 * keys held down; a key that appears is pressed, one that goes away is
 * released, and each is passed to the PS/2 driver's keyboard_key() as
 * the scancode it would have had. USB keyboards do not repeat keys by
 * themselves, so a held key repeats from the timer tick here.
 *
 * Mice and tablets (such as QEMU's usb-tablet, which gives absolute
 * positions) are read through their report descriptor: the parser finds
 * the buttons, X, Y and the wheel, and the reports become /dev/input
 * mouse events. Without a usable descriptor a boot mouse falls back to
 * the 3-byte boot protocol.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "usb.h"

#define HID_SET_IDLE     0x0A
#define HID_SET_PROTOCOL 0x0B

#define REPEAT_DELAY (HZ / 2)
#define REPEAT_RATE  3                  /* ticks between repeats: about 33 a second */
#define MAX_HIDS     8

struct field {
    uint16_t bit, size;                 /* offset in the report, in bits */
    int32_t min, max;                   /* logical range */
    int present;
};

struct hid {
    struct usb_dev *dev;
    int keyboard;
    uint8_t iface;
    struct usb_xfer x;
    uint8_t buf[64];
    uint8_t prev[8];                    /* keyboard: the last report */
    /* mouse */
    int report_id;                      /* 0: reports have no ID byte */
    int absolute;
    struct field buttons, xf, yf, wheel;
    int nbuttons;
    /* key repeat */
    uint8_t repeat_key;
    uint32_t repeat_at;
    volatile int error;
    int errors;
};

static struct hid *hids[MAX_HIDS];

/* HID keyboard usages 0x00-0x65 as scancode set 1; 0x100 marks an E0 prefix. */
static const uint16_t usage_sc[0x66] = {
    [0x04] = 0x1E, [0x05] = 0x30, [0x06] = 0x2E, [0x07] = 0x20, [0x08] = 0x12, [0x09] = 0x21,
    [0x0A] = 0x22, [0x0B] = 0x23, [0x0C] = 0x17, [0x0D] = 0x24, [0x0E] = 0x25, [0x0F] = 0x26,
    [0x10] = 0x32, [0x11] = 0x31, [0x12] = 0x18, [0x13] = 0x19, [0x14] = 0x10, [0x15] = 0x13,
    [0x16] = 0x1F, [0x17] = 0x14, [0x18] = 0x16, [0x19] = 0x2F, [0x1A] = 0x11, [0x1B] = 0x2D,
    [0x1C] = 0x15, [0x1D] = 0x2C,
    [0x1E] = 0x02, [0x1F] = 0x03, [0x20] = 0x04, [0x21] = 0x05, [0x22] = 0x06, [0x23] = 0x07,
    [0x24] = 0x08, [0x25] = 0x09, [0x26] = 0x0A, [0x27] = 0x0B,
    [0x28] = 0x1C, [0x29] = 0x01, [0x2A] = 0x0E, [0x2B] = 0x0F, [0x2C] = 0x39, [0x2D] = 0x0C,
    [0x2E] = 0x0D, [0x2F] = 0x1A, [0x30] = 0x1B, [0x31] = 0x2B, [0x32] = 0x2B, [0x33] = 0x27,
    [0x34] = 0x28, [0x35] = 0x29, [0x36] = 0x33, [0x37] = 0x34, [0x38] = 0x35, [0x39] = 0x3A,
    [0x3A] = 0x3B, [0x3B] = 0x3C, [0x3C] = 0x3D, [0x3D] = 0x3E, [0x3E] = 0x3F, [0x3F] = 0x40,
    [0x40] = 0x41, [0x41] = 0x42, [0x42] = 0x43, [0x43] = 0x44, [0x44] = 0x57, [0x45] = 0x58,
    [0x46] = 0x137, [0x47] = 0x46, [0x49] = 0x152, [0x4A] = 0x147, [0x4B] = 0x149,
    [0x4C] = 0x153, [0x4D] = 0x14F, [0x4E] = 0x151, [0x4F] = 0x14D, [0x50] = 0x14B,
    [0x51] = 0x150, [0x52] = 0x148, [0x53] = 0x45, [0x54] = 0x135, [0x55] = 0x37,
    [0x56] = 0x4A, [0x57] = 0x4E, [0x58] = 0x11C, [0x59] = 0x4F, [0x5A] = 0x50, [0x5B] = 0x51,
    [0x5C] = 0x4B, [0x5D] = 0x4C, [0x5E] = 0x4D, [0x5F] = 0x47, [0x60] = 0x48, [0x61] = 0x49,
    [0x62] = 0x52, [0x63] = 0x53, [0x64] = 0x56, [0x65] = 0x15D,
};

/* The modifier bits of a boot report: left ctrl, shift, alt, GUI, then right. */
static const uint16_t mod_sc[8] = { 0x1D, 0x2A, 0x38, 0x15B, 0x11D, 0x36, 0x138, 0x15C };

static void key(uint16_t sc, int released)
{
    if (sc)
        keyboard_key((uint8_t)sc, (sc & 0x100) != 0, released);
}

static int repeats(uint8_t usage)
{
    return usage != 0x39 && usage != 0x47 && usage != 0x53;     /* the locks */
}

static int has_key(const uint8_t *keys, uint8_t k)
{
    for (int i = 0; i < 6; i++)
        if (keys[i] == k)
            return 1;
    return 0;
}

static void keyboard_report(struct hid *h, const uint8_t *r)
{
    if (r[2] == 0x01)
        return;                         /* too many keys at once: no information */
    uint8_t changed = r[0] ^ h->prev[0];
    for (int b = 0; b < 8; b++)
        if (changed & (1 << b))
            key(mod_sc[b], !(r[0] & (1 << b)));
    for (int i = 2; i < 8; i++) {
        uint8_t k = h->prev[i];
        if (k > 3 && !has_key(r + 2, k)) {
            if (k < ARRAY_SIZE(usage_sc))
                key(usage_sc[k], 1);
            if (k == h->repeat_key)
                h->repeat_key = 0;
        }
    }
    for (int i = 2; i < 8; i++) {
        uint8_t k = r[i];
        if (k > 3 && !has_key(h->prev + 2, k) && k < ARRAY_SIZE(usage_sc)) {
            key(usage_sc[k], 0);
            if (repeats(k) && usage_sc[k]) {
                h->repeat_key = k;
                h->repeat_at = ticks + REPEAT_DELAY;
            }
        }
    }
    memcpy(h->prev, r, 8);
}

/* Let go of everything held, as if the keys were released (unplugged). */
static void keyboard_release_all(struct hid *h)
{
    uint8_t none[8] = { 0 };
    keyboard_report(h, none);
    h->repeat_key = 0;
}

/* ---- mice and tablets ---- */

static int32_t get_field(const uint8_t *r, uint32_t len, const struct field *f)
{
    uint32_t v = 0;
    for (uint32_t i = 0; i < f->size && i < 32; i++) {
        uint32_t bit = f->bit + i;
        if (bit / 8 < len && (r[bit / 8] >> (bit % 8)) & 1)
            v |= 1u << i;
    }
    if (f->min < 0 && f->size < 32 && (v & (1u << (f->size - 1))))
        v |= ~0u << f->size;            /* sign-extend */
    return (int32_t)v;
}

/* An absolute position to 0..32767 (in 32-bit arithmetic: no 64-bit division here). */
static int16_t scale(int32_t v, const struct field *f)
{
    uint32_t range = (uint32_t)(f->max - f->min);
    uint32_t pos = (uint32_t)(MIN(MAX(v, f->min), f->max) - f->min);
    while (range > 0xFFFF) {
        range >>= 1;
        pos >>= 1;
    }
    return range ? (int16_t)(pos * 32767u / range) : 0;
}

static void mouse_report(struct hid *h, const uint8_t *r, uint32_t len)
{
    if (h->report_id) {
        if (!len || r[0] != h->report_id)
            return;
        r++;
        len--;
    }
    struct input_event ev = { .type = IEV_MOUSE };
    for (int b = 0; b < h->nbuttons && b < 3; b++) {
        struct field one = { (uint16_t)(h->buttons.bit + b), 1, 0, 1, 1 };
        if (get_field(r, len, &one))
            ev.mods |= (uint8_t)(1 << b);   /* left, right, middle, as MOUSE_* */
    }
    int32_t x = get_field(r, len, &h->xf), y = get_field(r, len, &h->yf);
    if (h->absolute) {
        ev.flags = IEV_ABS;
        ev.x = scale(x, &h->xf);
        ev.y = scale(y, &h->yf);
    } else {
        ev.x = (int16_t)x;
        ev.y = (int16_t)y;
    }
    if (h->wheel.present)
        ev.code = (uint16_t)(int16_t)get_field(r, len, &h->wheel);  /* positive: away from the user */
    input_push(&ev);
}

/*
 * Read a report descriptor for the pointer fields: buttons (page 9), X,
 * Y and the wheel (generic desktop 0x30, 0x31, 0x38), all in the first
 * report that has X. Returns 0 if X and Y were found.
 */
static int parse_report(struct hid *h, const uint8_t *d, uint32_t len)
{
    uint32_t page = 0, rsize = 0, rcount = 0, umin = 0, umax = 0;
    int32_t lmin = 0, lmax = 0;
    uint32_t usages[16];
    int nusages = 0, rid = 0, chosen_rid = -1, buttons_rid = 0;
    uint32_t bitpos[256] = { 0 };       /* per report ID */
    struct { uint32_t page, rsize, rcount; int32_t lmin, lmax; } stack[4];
    int sp = 0;

    for (uint32_t i = 0; i < len;) {
        uint8_t b = d[i++];
        if (b == 0xFE) {                /* long item */
            if (i + 1 >= len)
                break;
            i += 2 + d[i];
            continue;
        }
        uint32_t size = (b & 3) == 3 ? 4 : (b & 3);
        if (i + size > len)
            break;
        uint32_t v = 0;
        for (uint32_t k = 0; k < size; k++)
            v |= (uint32_t)d[i + k] << (8 * k);
        int32_t sv = size == 1 ? (int8_t)v : size == 2 ? (int16_t)v : (int32_t)v;
        i += size;
        int type = (b >> 2) & 3, tag = b >> 4;

        if (type == 1) {                /* global */
            switch (tag) {
            case 0: page = v; break;
            case 1: lmin = sv; break;
            case 2: lmax = size < 4 && lmin >= 0 ? (int32_t)v : sv; break;
            case 7: rsize = v; break;
            case 8: rid = (int)(v & 0xFF); break;
            case 9: rcount = v; break;
            case 10:
                if (sp < 4) {
                    stack[sp].page = page; stack[sp].rsize = rsize; stack[sp].rcount = rcount;
                    stack[sp].lmin = lmin; stack[sp].lmax = lmax;
                    sp++;
                }
                break;
            case 11:
                if (sp > 0) {
                    sp--;
                    page = stack[sp].page; rsize = stack[sp].rsize; rcount = stack[sp].rcount;
                    lmin = stack[sp].lmin; lmax = stack[sp].lmax;
                }
                break;
            }
        } else if (type == 2) {         /* local */
            uint32_t u = size == 4 ? v : (page << 16) | v;
            if (tag == 0 && nusages < 16)
                usages[nusages++] = u;
            else if (tag == 1)
                umin = u;
            else if (tag == 2)
                umax = u;
        } else if (type == 0) {         /* main */
            if (tag == 8) {             /* input */
                for (uint32_t k = 0; k < rcount; k++) {
                    uint32_t usage = nusages ? usages[MIN(k, (uint32_t)nusages - 1)]
                                             : umin + k;
                    if (umax && !nusages && usage > umax)
                        usage = 0;
                    uint32_t bit = bitpos[rid] + k * rsize;
                    if (v & 1)
                        continue;       /* constant: padding */
                    if (chosen_rid >= 0 && rid != chosen_rid)
                        continue;
                    struct field f = { (uint16_t)bit, (uint16_t)rsize, lmin, lmax, 1 };
                    if ((usage >> 16) == 0x09 && (usage & 0xFFFF) >= 1 && rsize == 1 &&
                        !h->buttons.present) {
                        h->buttons = f;
                        h->nbuttons = (int)(rcount - k);
                        buttons_rid = rid;
                    } else if (usage == 0x10030 && !h->xf.present && (v & 2)) {
                        h->xf = f;
                        h->absolute = !(v & 4);
                        chosen_rid = rid;
                    } else if (usage == 0x10031 && !h->yf.present && (v & 2)) {
                        h->yf = f;
                    } else if (usage == 0x10038 && !h->wheel.present && (v & 2)) {
                        h->wheel = f;
                    }
                }
                bitpos[rid] += rsize * rcount;
            }
            nusages = 0;
            umin = umax = 0;
        }
    }
    h->report_id = chosen_rid > 0 ? chosen_rid : 0;
    if (h->buttons.present && buttons_rid != h->report_id)
        h->nbuttons = 0;                /* in another report: not ours to read */
    return h->xf.present && h->yf.present ? 0 : -1;
}

/* The boot mouse report: buttons, X, Y, all relative. */
static void boot_mouse_layout(struct hid *h)
{
    memset(&h->buttons, 0, sizeof(h->buttons) * 4);
    h->buttons = (struct field){ 0, 1, 0, 1, 1 };
    h->nbuttons = 3;
    h->xf = (struct field){ 8, 8, -127, 127, 1 };
    h->yf = (struct field){ 16, 8, -127, 127, 1 };
    h->absolute = 0;
    h->report_id = 0;
}

/* ---- the driver ---- */

static void report_done(struct usb_xfer *x)
{
    struct hid *h = x->priv;
    if (x->status == -ENODEV)
        return;
    if (x->status) {
        h->error = x->status;           /* the hub thread recovers it */
        usb_kick();
        return;
    }
    h->errors = 0;
    if (h->keyboard) {
        if (x->actual >= 8)
            keyboard_report(h, h->buf);
    } else if (x->actual) {
        mouse_report(h, h->buf, x->actual);
    }
    usb_submit(x);
}

void usbhid_tick(void)
{
    for (int i = 0; i < MAX_HIDS; i++) {
        struct hid *h = hids[i];
        if (!h || !h->repeat_key || (int32_t)(ticks - h->repeat_at) < 0)
            continue;
        key(usage_sc[h->repeat_key], 0);
        h->repeat_at = ticks + REPEAT_RATE;
    }
}

/* From the hub thread: restart a device's reports after an error. */
void usbhid_service(void)
{
    for (int i = 0; i < MAX_HIDS; i++) {
        struct hid *h = hids[i];
        if (!h || !h->error || h->dev->gone)
            continue;
        int err = h->error;
        h->error = 0;
        if (++h->errors > 20) {
            klog("usb: HID device keeps failing (%d), giving up on it\n", err);
            continue;
        }
        if (h->keyboard)
            keyboard_release_all(h);
        if (err == -EPIPE || err == -EIO)
            usb_clear_halt(h->dev, h->x.ep);
        usb_submit(&h->x);
    }
}

static int hid_probe(struct usb_dev *dev, const struct usb_iface_desc *ifd,
                     const uint8_t *desc, uint32_t len, void **priv)
{
    if (ifd->bInterfaceClass != USB_CLASS_HID)
        return -1;
    int boot = ifd->bInterfaceSubClass == 1;
    int keyboard = boot && ifd->bInterfaceProtocol == 1;
    struct usb_ep *ep = usb_find_ep(dev, desc, len, USB_EP_INTR, 1);
    if (!ep)
        return -1;
    int slot = -1;
    for (int i = 0; i < MAX_HIDS && slot < 0; i++)
        if (!hids[i])
            slot = i;
    if (slot < 0)
        return -1;

    struct hid *h = kzalloc(sizeof(*h));
    if (!h)
        return -1;
    h->dev = dev;
    h->keyboard = keyboard;
    h->iface = ifd->bInterfaceNumber;

    const uint8_t rt = USB_RT_CLASS | USB_RT_INTERFACE;
    if (keyboard) {
        usb_control(dev, rt, HID_SET_PROTOCOL, 0, h->iface, NULL, 0);   /* boot protocol */
        usb_control(dev, rt, HID_SET_IDLE, 0, h->iface, NULL, 0);       /* report changes only */
    } else {
        /* The report descriptor's length is in the HID descriptor. */
        uint32_t rlen = 0;
        for (uint32_t off = 0; off + 9 <= len && desc[off] >= 2; off += desc[off])
            if (desc[off + 1] == USB_DT_HID && desc[off] >= 9 && desc[off + 6] == USB_DT_REPORT)
                rlen = desc[off + 7] | (uint32_t)desc[off + 8] << 8;
        uint8_t *rd = rlen ? kmalloc(rlen) : NULL;
        int ok = -1;
        if (rd) {
            int n = usb_control(dev, USB_RT_IN | USB_RT_INTERFACE, USB_REQ_GET_DESCRIPTOR,
                                USB_DT_REPORT << 8, h->iface, rd, (uint16_t)rlen);
            if (n > 0)
                ok = parse_report(h, rd, (uint32_t)n);
            kfree(rd);
        }
        if (ok == 0) {
            if (boot)
                usb_control(dev, rt, HID_SET_PROTOCOL, 1, h->iface, NULL, 0);   /* report protocol */
        } else if (boot && ifd->bInterfaceProtocol == 2) {
            boot_mouse_layout(h);
            usb_control(dev, rt, HID_SET_PROTOCOL, 0, h->iface, NULL, 0);
        } else {
            kfree(h);
            return -1;                  /* not a pointer we understand */
        }
        usb_control(dev, rt, HID_SET_IDLE, 0, h->iface, NULL, 0);
    }

    h->x.dev = dev;
    h->x.ep = ep;
    h->x.buf = h->buf;
    h->x.len = MIN((uint32_t)ep->maxpacket, sizeof(h->buf));
    h->x.complete = report_done;
    h->x.priv = h;
    hids[slot] = h;
    if (usb_submit(&h->x) < 0) {
        hids[slot] = NULL;
        kfree(h);
        return -1;
    }
    klog("usb: %s%s\n", keyboard ? "keyboard" : h->absolute ? "tablet (absolute pointer)" :
         "mouse", !keyboard && h->wheel.present ? " with a wheel" : "");
    *priv = h;
    return 0;
}

static void hid_disconnect(struct usb_dev *dev, void *priv)
{
    (void)dev;
    struct hid *h = priv;
    usb_cancel(&h->x);
    for (int i = 0; i < MAX_HIDS; i++)
        if (hids[i] == h)
            hids[i] = NULL;
    if (h->keyboard)
        keyboard_release_all(h);
    kfree(h);
}

const struct usb_driver usbhid_driver = {
    .name = "hid",
    .probe = hid_probe,
    .disconnect = hid_disconnect,
};
