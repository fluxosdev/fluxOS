/*
 * USB hub devices. A hub's ports are driven with class requests (power,
 * reset, status); its interrupt endpoint reports which ports changed,
 * and the hub thread then scans them (usb_hub_scan). USB 3 hubs speak
 * the SuperSpeed variants of the same requests.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "usb.h"

#define HUB_GET_STATUS    0
#define HUB_CLEAR_FEATURE 1
#define HUB_SET_FEATURE   3
#define HUB_SET_DEPTH     12

#define PORT_RESET        4
#define PORT_POWER        8
#define C_PORT_CONNECTION 16
#define PORT_ENABLE_FEAT  1

#define ST_CONNECTION 0x0001
#define ST_ENABLE     0x0002
#define ST_RESET      0x0010
#define ST_LOW_SPEED  0x0200
#define ST_HIGH_SPEED 0x0400
#define CH_RESET      0x0010
#define CH_BH_RESET   0x0020            /* SuperSpeed */

#define RT_PORT_OUT (USB_RT_CLASS | USB_RT_OTHER)
#define RT_PORT_IN  (USB_RT_IN | USB_RT_CLASS | USB_RT_OTHER)

struct hubdev {
    struct usb_hub hub;
    struct usb_dev *dev;
    int ss;
    struct usb_xfer status;             /* the status change endpoint */
    uint8_t bitmap[8];
    int errors;
};

static int get_port_status(struct hubdev *hd, int port, uint16_t *st, uint16_t *ch)
{
    uint8_t b[4];
    int r = usb_control(hd->dev, RT_PORT_IN, HUB_GET_STATUS, 0, (uint16_t)port, b, 4);
    if (r < 4)
        return r < 0 ? r : -EIO;
    *st = (uint16_t)(b[0] | b[1] << 8);
    *ch = (uint16_t)(b[2] | b[3] << 8);
    return 0;
}

static int port_feature(struct hubdev *hd, int port, int req, int feature)
{
    return usb_control(hd->dev, RT_PORT_OUT, (uint8_t)req, (uint16_t)feature, (uint16_t)port,
                       NULL, 0);
}

static int hub_port_status(struct usb_hub *h, int port)
{
    struct hubdev *hd = h->priv;
    uint16_t st, ch;
    if (get_port_status(hd, port, &st, &ch) < 0)
        return -1;
    /* Acknowledge every change. Bits 0-4 are features 16-20; a USB 3 hub
     * adds BH reset (29), link state (25) and config error (26). */
    static const uint8_t ss_feature[3] = { 29, 25, 26 };
    for (int b = 0; b < 8; b++) {
        if (!(ch & (1 << b)))
            continue;
        if (b < 5)
            port_feature(hd, port, HUB_CLEAR_FEATURE, C_PORT_CONNECTION + b);
        else if (hd->ss)
            port_feature(hd, port, HUB_CLEAR_FEATURE, ss_feature[b - 5]);
    }
    return ((st & ST_CONNECTION) ? PORT_CONNECT : 0) | ((st & ST_ENABLE) ? PORT_ENABLE : 0) |
           ((ch & 1) ? PORT_CHANGE : 0);
}

static int hub_port_reset(struct usb_hub *h, int port)
{
    struct hubdev *hd = h->priv;
    uint16_t st = 0, ch = 0;
    if (port_feature(hd, port, HUB_SET_FEATURE, PORT_RESET) < 0)
        return -EIO;
    for (int i = 0; i < 50; i++) {
        mdelay(10);
        if (get_port_status(hd, port, &st, &ch) < 0)
            return -EIO;
        if ((ch & CH_RESET) && !(st & ST_RESET))
            break;
    }
    port_feature(hd, port, HUB_CLEAR_FEATURE, C_PORT_CONNECTION + 4);     /* C_PORT_RESET */
    if (hd->ss && (ch & CH_BH_RESET))
        port_feature(hd, port, HUB_CLEAR_FEATURE, 29);                    /* C_BH_PORT_RESET */
    if (!(st & ST_CONNECTION) || !(st & ST_ENABLE))
        return -EIO;
    if (hd->ss)
        return USB_SPEED_SUPER;
    return (st & ST_LOW_SPEED) ? USB_SPEED_LOW : (st & ST_HIGH_SPEED) ? USB_SPEED_HIGH
                                                                     : USB_SPEED_FULL;
}

static void hub_port_disable(struct usb_hub *h, int port)
{
    struct hubdev *hd = h->priv;
    if (!hd->ss)
        port_feature(hd, port, HUB_CLEAR_FEATURE, PORT_ENABLE_FEAT);
}

/* The status endpoint answered: some port changed. The hub thread looks. */
static void status_done(struct usb_xfer *x)
{
    struct hubdev *hd = x->priv;
    if (x->status == -ENODEV)
        return;
    hd->errors = x->status ? hd->errors + 1 : 0;
    hd->hub.changed = 1;
    usb_kick();
}

void usbhub_resubmit(struct usb_hub *h)
{
    struct hubdev *hd = h->priv;
    if (hd->dev->gone || hd->status.status == USB_PENDING || hd->errors > 10)
        return;
    if (hd->status.status == -EPIPE)
        usb_clear_halt(hd->dev, hd->status.ep);
    usb_submit(&hd->status);
}

static int hub_probe(struct usb_dev *dev, const struct usb_iface_desc *ifd,
                     const uint8_t *desc, uint32_t len, void **priv)
{
    if (ifd->bInterfaceClass != USB_CLASS_HUB)
        return -1;
    if (dev->depth >= 5) {
        klog("usb: hub nested too deep, ignored\n");
        return -1;
    }
    struct usb_ep *ep = usb_find_ep(dev, desc, len, USB_EP_INTR, 1);
    if (!ep)
        return -1;

    int ss = dev->speed == USB_SPEED_SUPER;
    uint8_t d[16];
    int r = usb_control(dev, USB_RT_IN | USB_RT_CLASS, USB_REQ_GET_DESCRIPTOR,
                        (uint16_t)((ss ? USB_DT_SS_HUB : USB_DT_HUB) << 8), 0, d, sizeof(d));
    if (r < 7)
        return -1;
    int nports = d[2];
    uint16_t chars = (uint16_t)(d[3] | d[4] << 8);
    uint32_t power_ms = (uint32_t)d[5] * 2;

    struct hubdev *hd = kzalloc(sizeof(*hd));
    if (!hd)
        return -1;
    hd->dev = dev;
    hd->ss = ss;
    struct usb_hub *h = &hd->hub;
    h->hc = dev->hc;
    h->dev = dev;
    h->nports = MIN(nports, USB_MAX_PORTS);
    h->port_status = hub_port_status;
    h->port_reset = hub_port_reset;
    h->port_disable = hub_port_disable;
    h->priv = hd;

    if (dev->hc->ops->dev_hub)
        dev->hc->ops->dev_hub(dev->hc, dev, nports, (chars >> 5) & 3, 0);
    if (ss)
        usb_control(dev, USB_RT_CLASS, HUB_SET_DEPTH, (uint16_t)dev->depth, 0, NULL, 0);
    for (int p = 1; p <= h->nports; p++)
        port_feature(hd, p, HUB_SET_FEATURE, PORT_POWER);
    mdelay(MAX(power_ms, 20u) + 100);   /* power good, then devices connect */

    hd->status.dev = dev;
    hd->status.ep = ep;
    hd->status.buf = hd->bitmap;
    hd->status.len = MIN((uint32_t)(nports + 8) / 8, sizeof(hd->bitmap));
    hd->status.complete = status_done;
    hd->status.priv = hd;
    dev->hub = h;
    usb_submit(&hd->status);
    klog("usb: %s hub with %d ports\n", ss ? "USB 3" : dev->speed == USB_SPEED_HIGH ? "USB 2" :
         "USB 1.1", nports);
    *priv = hd;
    return 0;
}

static void hub_disconnect(struct usb_dev *dev, void *priv)
{
    struct hubdev *hd = priv;
    usb_cancel(&hd->status);
    dev->hub = NULL;
    kfree(hd);
}

const struct usb_driver usbhub_driver = {
    .name = "hub",
    .probe = hub_probe,
    .disconnect = hub_disconnect,
};
