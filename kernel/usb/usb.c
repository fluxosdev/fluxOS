/*
 * USB core: the list of host controllers, transfers, device enumeration,
 * hub port scanning and the hotplug thread.
 *
 * At boot every controller's ports are scanned once, polling, so that a
 * USB keyboard works and a USB disk can hold the root filesystem. Then
 * the kusbd thread looks at the root hubs four times a second, and at a
 * hub device whenever its status endpoint reports a change: it
 * enumerates what was plugged in (reset, address, descriptors,
 * configuration) and offers each interface to the drivers, and takes
 * away what was unplugged.
 *
 * Controllers report completions by interrupt; the timer tick polls them
 * too, so a controller whose interrupt never arrives still works.
 * "usb=off" on the kernel command line leaves USB alone.
 *
 * usb_lock (usb.h) is taken on every way in: the controllers' interrupts,
 * the tick, kusbd, usbinfo(2) and the storage driver's block operations.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "usb.h"

#define MAX_DEVS        64
#define CONTROL_TIMEOUT 5000            /* ms */

static struct usb_hc *hcs;
static int nhcs;
static struct usb_dev *devtab[MAX_DEVS];
static int kusbd_chan;
static volatile int kusbd_kicked;
static int started;
static int in_callback;                 /* a completion callback is running */
static int suspended;                   /* suspend to RAM: the controllers are not to be touched */

spinlock_t usb_lock = SPINLOCK_INIT("usb");

static const struct usb_driver *const drivers[] = {
    &usbhub_driver, &usbhid_driver, &usbmsc_driver, &usbaudio_driver,
};

static const char *const speed_names[] = { "low", "full", "high", "super" };

/* The storage driver's block operations come both from the VFS and from
 * its own probe (reading the partition table), which holds the lock. */
int usb_enter(void)
{
    if (spin_held(&usb_lock))
        return 0;
    spin_lock(&usb_lock);
    return 1;
}

void usb_leave(int taken)
{
    if (taken)
        spin_unlock(&usb_lock);
}

void usb_mdelay(uint32_t ms)
{
    if (!spin_held(&usb_lock)) {
        mdelay(ms);
        return;
    }
    spin_unlock(&usb_lock);
    mdelay(ms);
    spin_lock(&usb_lock);
}

int usb_in_process(void)
{
    return current && current->pid > 0 && !in_callback;
}

/* Can the caller wait for the controller: a process, or the boot code
 * polling? Not a completion callback, which runs from an interrupt. */
int usb_can_wait(void)
{
    return !in_callback;
}

void usb_register_hc(struct usb_hc *hc)
{
    struct usb_hc **pp = &hcs;
    while (*pp)
        pp = &(*pp)->next;
    *pp = hc;
    hc->next = NULL;
    hc->root.hc = hc;
    nhcs++;
}

/* ---- memory for controller data structures ---- */

#define DMA_CLASSES 8                   /* 32 bytes to 4 KiB */

static void *dma_free_list[DMA_CLASSES];

static int dma_class(uint32_t size)
{
    int k = 0;
    while ((32u << k) < size)
        k++;
    return k;
}

void *usb_dma_alloc(uint32_t size)
{
    int k = dma_class(size);
    if (k >= DMA_CLASSES)
        return NULL;
    uint32_t sz = 32u << k;
    void *p = dma_free_list[k];
    if (p) {
        dma_free_list[k] = *(void **)p;
    } else {
        uint32_t page = pmm_alloc();
        if (!page)
            return NULL;
        for (uint32_t off = PAGE_SIZE - sz; off > 0; off -= sz) {
            *(void **)(page + off) = dma_free_list[k];
            dma_free_list[k] = (void *)(page + off);
        }
        p = (void *)page;
    }
    memset(p, 0, sz);
    return p;
}

void usb_dma_free(void *p, uint32_t size)
{
    if (!p)
        return;
    int k = dma_class(size);
    *(void **)p = dma_free_list[k];
    dma_free_list[k] = p;
}

/* ---- transfers ---- */

int usb_submit(struct usb_xfer *x)
{
    if (x->dev->gone)
        return x->status = -ENODEV;
    if (x->len > USB_MAX_XFER || (x->ep && x->ep->type == USB_EP_ISO && !x->npackets))
        return x->status = -EINVAL;
    x->status = USB_PENDING;
    x->actual = 0;
    int r = x->dev->hc->ops->submit(x->dev->hc, x);
    if (r < 0)
        x->status = r;
    return r;
}

void usb_xfer_done(struct usb_xfer *x, int status, uint32_t actual)
{
    x->actual = actual;
    x->status = status;
    if (x->complete) {
        in_callback++;
        x->complete(x);
        in_callback--;
    } else {
        wakeup(x);
    }
}

void usb_cancel(struct usb_xfer *x)
{
    if (x->status != USB_PENDING)
        return;
    x->dev->hc->ops->cancel(x->dev->hc, x);
    if (x->status == USB_PENDING)
        x->status = -ETIMEDOUT;
}

/*
 * Wait for a transfer: a process sleeps until the controller's interrupt
 * (or the next tick) finishes it; the boot code polls. A transfer that
 * takes too long is cancelled.
 */
int usb_wait(struct usb_xfer *x, uint32_t timeout_ms)
{
    struct usb_hc *hc = x->dev->hc;
    if (usb_in_process()) {
        uint32_t deadline = ticks + timeout_ms / (1000 / HZ) + 1;
        while (x->status == USB_PENDING && (int32_t)(ticks - deadline) < 0) {
            hc->ops->poll(hc);
            if (x->status != USB_PENDING)
                break;
            sleep_on_lock_timeout(x, &usb_lock, ticks + 1);
        }
    } else {
        for (uint32_t us = 0; x->status == USB_PENDING && us < timeout_ms * 1000; us += 50) {
            hc->ops->poll(hc);
            udelay(50);
        }
    }
    if (x->status == USB_PENDING)
        usb_cancel(x);
    return x->status;
}

int usb_control(struct usb_dev *dev, uint8_t reqtype, uint8_t req, uint16_t value,
                uint16_t index, void *data, uint16_t len)
{
    struct usb_xfer x;
    memset(&x, 0, sizeof(x));
    x.dev = dev;
    x.ep = &dev->ep0;
    x.setup[0] = reqtype;
    x.setup[1] = req;
    x.setup[2] = (uint8_t)value;
    x.setup[3] = (uint8_t)(value >> 8);
    x.setup[4] = (uint8_t)index;
    x.setup[5] = (uint8_t)(index >> 8);
    x.setup[6] = (uint8_t)len;
    x.setup[7] = (uint8_t)(len >> 8);
    x.buf = data;
    x.len = len;
    if (usb_submit(&x) < 0)
        return x.status;
    int r = usb_wait(&x, CONTROL_TIMEOUT);
    return r < 0 ? r : (int)x.actual;
}

int usb_transfer(struct usb_dev *dev, struct usb_ep *ep, void *buf, uint32_t len,
                 uint32_t timeout_ms)
{
    struct usb_xfer x;
    memset(&x, 0, sizeof(x));
    x.dev = dev;
    x.ep = ep;
    x.buf = buf;
    x.len = len;
    if (usb_submit(&x) < 0)
        return x.status;
    int r = usb_wait(&x, timeout_ms);
    return r < 0 ? r : (int)x.actual;
}

int usb_clear_halt(struct usb_dev *dev, struct usb_ep *ep)
{
    int r = usb_control(dev, USB_RT_ENDPOINT, USB_REQ_CLEAR_FEATURE, 0 /* ENDPOINT_HALT */,
                        ep->addr, NULL, 0);
    ep->toggle = 0;
    if (dev->hc->ops->ep_reset)
        dev->hc->ops->ep_reset(dev->hc, dev, ep);
    return r < 0 ? r : 0;
}

struct usb_ep *usb_find_ep(struct usb_dev *dev, const uint8_t *desc, uint32_t len,
                           int type, int in)
{
    for (uint32_t off = 0; off + 2 <= len && desc[off] >= 2; off += desc[off]) {
        if (desc[off + 1] != USB_DT_ENDPOINT || off + 7 > len)
            continue;
        const struct usb_ep_desc *e = (const void *)(desc + off);
        if ((e->bmAttributes & 3) == type && !!(e->bEndpointAddress & USB_DIR_IN) == !!in)
            return &dev->eps[usb_ep_index(e->bEndpointAddress)];
    }
    return NULL;
}

struct usb_ep *usb_ep_enable(struct usb_dev *dev, const struct usb_ep_desc *e)
{
    if (e->bLength < 7 || !(e->bEndpointAddress & 0x0F))
        return NULL;
    struct usb_ep *ep = &dev->eps[usb_ep_index(e->bEndpointAddress)];
    if (ep->hcpriv && dev->hc->ops->ep_disable)
        dev->hc->ops->ep_disable(dev->hc, dev, ep);
    memset(ep, 0, sizeof(*ep));
    ep->addr = e->bEndpointAddress;
    ep->type = e->bmAttributes & 3;
    ep->maxpacket = e->wMaxPacketSize & 0x7FF;
    ep->mult = (e->wMaxPacketSize >> 11) & 3;
    ep->interval = e->bInterval;
    if (dev->hc->ops->ep_enable && dev->hc->ops->ep_enable(dev->hc, dev, ep) < 0)
        return NULL;
    return ep;
}

void usb_ep_disable(struct usb_dev *dev, struct usb_ep *ep)
{
    if (dev->hc->ops->ep_disable && !dev->gone)
        dev->hc->ops->ep_disable(dev->hc, dev, ep);
}

void usb_kick(void)
{
    kusbd_kicked = 1;
    wakeup(&kusbd_chan);
}

/* ---- enumeration ---- */

static int alloc_address(struct usb_hc *hc)
{
    for (int a = 1; a < 128; a++) {
        if (!(hc->addr_used[a / 8] & (1 << (a % 8)))) {
            hc->addr_used[a / 8] |= (uint8_t)(1 << (a % 8));
            return a;
        }
    }
    return -1;
}

static void free_address(struct usb_hc *hc, int a)
{
    if (a > 0 && a < 128)
        hc->addr_used[a / 8] &= (uint8_t)~(1 << (a % 8));
}

static int get_descriptor(struct usb_dev *dev, int type, int index, uint16_t lang,
                          void *buf, uint16_t len)
{
    return usb_control(dev, USB_RT_IN, USB_REQ_GET_DESCRIPTOR,
                       (uint16_t)(type << 8 | index), lang, buf, len);
}

/* A string descriptor as ASCII; empty if the device has none. */
static void get_string(struct usb_dev *dev, int index, uint16_t lang, char *out, uint32_t size)
{
    uint8_t buf[128];
    out[0] = 0;
    if (!index)
        return;
    int n = get_descriptor(dev, USB_DT_STRING, index, lang, buf, sizeof(buf));
    if (n < 4 || buf[1] != USB_DT_STRING)
        return;
    n = MIN(n, buf[0]);
    uint32_t k = 0;
    for (int i = 2; i + 1 < n && k + 1 < size; i += 2) {
        uint16_t c = (uint16_t)(buf[i] | buf[i + 1] << 8);
        if (!c)
            break;                      /* (some pad the declared length with NULs) */
        out[k++] = c >= 0x20 && c < 0x7F ? (char)c : '?';
    }
    out[k] = 0;
    while (k > 0 && out[k - 1] == ' ')
        out[--k] = 0;
}

static uint16_t ep0_size(int speed, uint8_t b)
{
    if (speed == USB_SPEED_SUPER)
        return b >= 5 && b <= 9 ? (uint16_t)(1u << b) : 512;
    if (b == 8 || b == 16 || b == 32 || b == 64)
        return b;
    return speed == USB_SPEED_LOW ? 8 : 64;
}

/* Note the endpoints of every interface's first alternate setting. */
static void parse_config(struct usb_dev *dev)
{
    const uint8_t *d = dev->config;
    uint32_t len = dev->config_len;
    int alt0 = 0;
    struct usb_ep *last = NULL;
    for (uint32_t off = 0; off + 2 <= len && d[off] >= 2; off += d[off]) {
        if (off + d[off] > len)
            break;
        switch (d[off + 1]) {
        case USB_DT_INTERFACE: {
            const struct usb_iface_desc *ifd = (const void *)(d + off);
            alt0 = d[off] >= 9 && ifd->bAlternateSetting == 0;     /* a short one is skipped, with its endpoints */
            last = NULL;
            if (alt0 && dev->nifaces < USB_MAX_IFACES) {
                struct usb_iface *ifc = &dev->ifaces[dev->nifaces++];
                ifc->num = ifd->bInterfaceNumber;
                ifc->cls = ifd->bInterfaceClass;
                ifc->sub = ifd->bInterfaceSubClass;
                ifc->proto = ifd->bInterfaceProtocol;
            }
            break;
        }
        case USB_DT_ENDPOINT: {
            const struct usb_ep_desc *e = (const void *)(d + off);
            if (!alt0 || d[off] < 7 || !(e->bEndpointAddress & 0x0F))
                break;
            struct usb_ep *ep = &dev->eps[usb_ep_index(e->bEndpointAddress)];
            memset(ep, 0, sizeof(*ep));
            ep->addr = e->bEndpointAddress;
            ep->type = e->bmAttributes & 3;
            ep->maxpacket = e->wMaxPacketSize & 0x7FF;
            ep->mult = (e->wMaxPacketSize >> 11) & 3;
            ep->interval = e->bInterval;
            last = ep;
            break;
        }
        case USB_DT_SS_EP_COMP:
            if (last && d[off] >= 3)
                last->burst = d[off + 2];
            break;
        }
    }
}

/* Offer each interface to the drivers. */
static void bind_drivers(struct usb_dev *dev)
{
    const uint8_t *d = dev->config;
    uint32_t len = dev->config_len;
    int n = 0;
    for (uint32_t off = 0; off + 2 <= len && d[off] >= 2; off += d[off]) {
        if (off + d[off] > len || d[off + 1] != USB_DT_INTERFACE || d[off] < 9)
            continue;
        const struct usb_iface_desc *ifd = (const void *)(d + off);
        if (ifd->bAlternateSetting != 0)
            continue;
        if (n >= dev->nifaces)
            break;
        /* The interface's descriptors run up to the next interface. */
        uint32_t end = off + d[off];
        while (end + 2 <= len && d[end] >= 2 && d[end + 1] != USB_DT_INTERFACE)
            end += d[end];
        end = MIN(end, len);
        struct usb_iface *ifc = &dev->ifaces[n++];
        for (uint32_t i = 0; i < ARRAY_SIZE(drivers) && !dev->gone; i++) {
            void *priv = NULL;
            if (drivers[i]->probe(dev, ifd, d + off, end - off, &priv) == 0) {
                ifc->drv = drivers[i];
                ifc->priv = priv;
                break;
            }
        }
    }
}

static void devtab_add(struct usb_dev *dev)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        if (!devtab[i]) {
            devtab[i] = dev;
            return;
        }
    }
}

static void devtab_remove(struct usb_dev *dev)
{
    for (int i = 0; i < MAX_DEVS; i++)
        if (devtab[i] == dev)
            devtab[i] = NULL;
}

static void describe(struct usb_dev *dev, char *out, uint32_t size)
{
    if (dev->product[0])
        snprintf(out, size, "%s%s%s", dev->manufacturer, dev->manufacturer[0] ? " " : "",
                 dev->product);
    else
        snprintf(out, size, "device %04x:%04x", dev->desc.idVendor, dev->desc.idProduct);
}

static void free_dev(struct usb_dev *dev)
{
    struct usb_hc *hc = dev->hc;
    if (hc->ops->dev_free)
        hc->ops->dev_free(hc, dev);
    if (!hc->sets_address)
        free_address(hc, dev->addr);
    kfree(dev->config);
    kfree(dev);
}

static struct usb_dev *usb_enumerate(struct usb_hub *h, int port, int speed)
{
    struct usb_hc *hc = h->hc;
    struct usb_dev *parent = h->dev;
    struct usb_dev *dev = kzalloc(sizeof(*dev));
    if (!dev)
        return NULL;
    dev->hc = hc;
    dev->parent = parent;
    dev->port = port;
    dev->speed = speed;
    if (parent) {
        dev->root_port = parent->root_port;
        dev->depth = parent->depth + 1;
        dev->route = parent->route | (uint32_t)MIN(port, 15) << (4 * parent->depth);
        if (speed < USB_SPEED_HIGH && parent->speed == USB_SPEED_HIGH) {
            dev->tt_hub = parent;
            dev->tt_port = port;
        } else {
            dev->tt_hub = parent->tt_hub;
            dev->tt_port = parent->tt_port;
        }
    } else {
        dev->root_port = port;
    }
    dev->ep0.type = USB_EP_CONTROL;
    dev->ep0.maxpacket = speed == USB_SPEED_SUPER ? 512 : speed == USB_SPEED_HIGH ? 64 :
                         speed == USB_SPEED_FULL && hc->sets_address ? 64 : 8;

    /* Learn ep0's packet size from the first 8 bytes of the device
     * descriptor, and give the device an address. */
    const char *step = "address";
    int r = -EIO;
    if (hc->sets_address) {
        if (hc->ops->dev_init(hc, dev) < 0)
            goto fail;
        step = "device descriptor";
        if ((r = get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->desc, 8)) < 8)
            goto fail;
        uint16_t mps = ep0_size(speed, dev->desc.bMaxPacketSize0);
        if (mps != dev->ep0.maxpacket) {
            dev->ep0.maxpacket = mps;
            if (hc->ops->dev_update && hc->ops->dev_update(hc, dev) < 0)
                goto fail;
        }
    } else {
        step = "device descriptor";
        for (int tries = 0; tries < 3; tries++) {
            if ((r = get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->desc, 8)) >= 8)
                break;
            usb_mdelay(20);
        }
        if (r < 8)
            goto fail;
        dev->ep0.maxpacket = ep0_size(speed, dev->desc.bMaxPacketSize0);
        step = "address";
        int addr = alloc_address(hc);
        if (addr < 0)
            goto fail;
        r = usb_control(dev, 0, USB_REQ_SET_ADDRESS, (uint16_t)addr, 0, NULL, 0);
        /* What the controller knows of address 0 is stale now. */
        if (hc->ops->dev_free)
            hc->ops->dev_free(hc, dev);
        if (r < 0) {
            free_address(hc, addr);
            goto fail;
        }
        dev->addr = addr;
        usb_mdelay(10);
    }

    step = "device descriptor";
    if ((r = get_descriptor(dev, USB_DT_DEVICE, 0, 0, &dev->desc, sizeof(dev->desc))) <
        (int)sizeof(dev->desc))
        goto fail;

    step = "configuration";
    struct usb_config_desc cd;
    if ((r = get_descriptor(dev, USB_DT_CONFIG, 0, 0, &cd, sizeof(cd))) < (int)sizeof(cd))
        goto fail;
    uint32_t total = MIN(MAX(cd.wTotalLength, sizeof(cd)), 4096u);
    if (!(dev->config = kmalloc(total)))
        goto fail;
    if ((r = get_descriptor(dev, USB_DT_CONFIG, 0, 0, dev->config, (uint16_t)total)) <
        (int)sizeof(cd))
        goto fail;
    dev->config_len = (uint32_t)r;

    /* Names are only for people: a device without them is fine. */
    uint8_t langs[4];
    uint16_t lang = 0x0409;
    if ((dev->desc.iManufacturer || dev->desc.iProduct) &&
        get_descriptor(dev, USB_DT_STRING, 0, 0, langs, sizeof(langs)) >= 4)
        lang = (uint16_t)(langs[2] | langs[3] << 8);
    get_string(dev, dev->desc.iManufacturer, lang, dev->manufacturer, sizeof(dev->manufacturer));
    get_string(dev, dev->desc.iProduct, lang, dev->product, sizeof(dev->product));

    if ((r = usb_control(dev, 0, USB_REQ_SET_CONFIGURATION, cd.bConfigurationValue, 0,
                         NULL, 0)) < 0)
        goto fail;
    parse_config(dev);
    if (hc->ops->dev_configure && (r = hc->ops->dev_configure(hc, dev)) < 0)
        goto fail;

    devtab_add(dev);
    char name[96];
    describe(dev, name, sizeof(name));
    if (started)
        klog("usb: %s port %d: %s (%04x:%04x), %s speed\n", hc->name, port, name,
             dev->desc.idVendor, dev->desc.idProduct, speed_names[speed]);
    else
        boot_ok("USB: %s (%04x:%04x), %s speed", name, dev->desc.idVendor,
                dev->desc.idProduct, speed_names[speed]);
    bind_drivers(dev);
    return dev;

fail:
    klog("usb: %s port %d: no answer to the %s request (%d)\n", hc->name, port, step, r);
    free_dev(dev);
    return NULL;
}

/* A device is unplugged (or its hub is): let go of it and what hangs off it. */
static void usb_disconnect(struct usb_dev *dev)
{
    dev->gone = 1;
    if (dev->hub) {
        for (int p = 1; p <= dev->hub->nports && p <= USB_MAX_PORTS; p++) {
            struct usb_dev *child = dev->hub->child[p];
            dev->hub->child[p] = NULL;
            if (child)
                usb_disconnect(child);
        }
    }
    for (int i = 0; i < dev->nifaces; i++)
        if (dev->ifaces[i].drv)
            dev->ifaces[i].drv->disconnect(dev, dev->ifaces[i].priv);
    devtab_remove(dev);
    char name[96];
    describe(dev, name, sizeof(name));
    klog("usb: %s disconnected\n", name);
    free_dev(dev);
}

/*
 * Look at a hub's ports: let go of devices that went away and enumerate
 * new ones, then do the same for hubs below (all of them, or those that
 * reported a change).
 */
void usb_hub_scan(struct usb_hub *h, int all)
{
    for (int port = 1; port <= h->nports && port <= USB_MAX_PORTS; port++) {
        if (h->dev && h->dev->gone)
            return;
        int st = h->port_status(h, port);
        if (st < 0)
            continue;
        struct usb_dev *child = h->child[port];
        if (child && (!(st & PORT_CONNECT) || (st & PORT_CHANGE))) {
            h->child[port] = NULL;
            usb_disconnect(child);
            child = NULL;
        }
        if (st & PORT_CHANGE)
            h->failed[port] = 0;
        if (child || !(st & PORT_CONNECT) || h->failed[port])
            continue;

        usb_mdelay(100);                /* let the contacts settle */
        st = h->port_status(h, port);
        if (!(st & PORT_CONNECT))
            continue;
        int speed = h->port_reset(h, port);
        if (speed < 0) {
            h->failed[port] = 1;
            continue;
        }
        usb_mdelay(10);                 /* reset recovery */
        struct usb_dev *dev = usb_enumerate(h, port, speed);
        if (!dev) {
            h->failed[port] = 1;
            if (h->port_disable)
                h->port_disable(h, port);
            continue;
        }
        h->child[port] = dev;
    }

    for (int port = 1; port <= h->nports && port <= USB_MAX_PORTS; port++) {
        struct usb_dev *c = h->child[port];
        if (c && c->hub && !c->gone && (all || c->hub->changed)) {
            c->hub->changed = 0;
            usb_hub_scan(c->hub, all);
            usbhub_resubmit(c->hub);
        }
    }
}

/* ---- the hotplug thread ---- */

static void kusbd(void)
{
    uint32_t last_full = ticks;
    for (;;) {
        wait_event_timeout(&kusbd_chan, kusbd_kicked, ticks + HZ / 4);
        kusbd_kicked = 0;
        current->sig_pending = 0;
        spin_lock(&usb_lock);
        if (suspended) {
            spin_unlock(&usb_lock);
            continue;
        }
        /* Hub devices report changes; every few seconds look anyway. */
        int all = (int32_t)(ticks - last_full) >= 3 * HZ;
        if (all)
            last_full = ticks;
        for (struct usb_hc *hc = hcs; hc; hc = hc->next)
            usb_hub_scan(&hc->root, all);
        usbhid_service();
        spin_unlock(&usb_lock);
    }
}

/* From the timer interrupt. Another processor in the USB code is already
 * looking at the controllers. */
void usb_tick(void)
{
    if (!hcs || !spin_trylock(&usb_lock))
        return;
    if (suspended) {
        spin_unlock(&usb_lock);
        return;
    }
    for (struct usb_hc *hc = hcs; hc; hc = hc->next)
        hc->ops->poll(hc);
    usbhid_tick();
    usbaudio_tick();
    spin_unlock(&usb_lock);
}

void usb_init(void)
{
    if (cmdline_has("usb=off"))
        return;
    spin_lock(&usb_lock);
    /* EHCI before its companions: it decides which ports they get. */
    xhci_probe();
    ehci_probe();
    ohci_probe();
    uhci_probe();
    if (hcs) {
        usb_mdelay(100);                /* devices on freshly powered ports to connect */
        for (struct usb_hc *hc = hcs; hc; hc = hc->next)
            usb_hub_scan(&hc->root, 1);
    }
    spin_unlock(&usb_lock);
}

void usb_start(void)
{
    started = 1;
    if (hcs)
        proc_spawn_kthread("kusbd", kusbd);
}

/* ---- suspend to RAM ---- */

/*
 * Every device goes, as if unplugged (their drivers let go of them), and
 * the controllers stop. The power then goes from the controllers, so
 * coming back they are set up from the start and the hotplug thread
 * finds the devices again, EHCI first so it hands its full- and
 * low-speed devices to its companions as at boot.
 */
void usb_suspend(void)
{
    if (!hcs)
        return;
    spin_lock(&usb_lock);
    for (struct usb_hc *hc = hcs; hc; hc = hc->next) {
        for (int port = 1; port <= hc->root.nports && port <= USB_MAX_PORTS; port++) {
            struct usb_dev *child = hc->root.child[port];
            hc->root.child[port] = NULL;
            if (child)
                usb_disconnect(child);
        }
    }
    for (struct usb_hc *hc = hcs; hc; hc = hc->next)
        if (hc->ops->suspend)
            hc->ops->suspend(hc);
    suspended = 1;
    spin_unlock(&usb_lock);
}

void usb_resume(void)
{
    if (!hcs)
        return;
    spin_lock(&usb_lock);
    for (struct usb_hc *hc = hcs; hc; hc = hc->next) {
        if (hc->ops->resume)
            hc->ops->resume(hc);
        memset(hc->root.failed, 0, sizeof(hc->root.failed));
    }
    suspended = 0;
    spin_unlock(&usb_lock);
    usb_kick();
}

/* ---- usbinfo(2) ---- */

static int info(int index, struct usbinfo *ui);

/* ui is the caller's, checked beforehand: it is filled holding the lock. */
int usb_info(int index, struct usbinfo *ui)
{
    if (index < 0)
        return -ENODEV;
    spin_lock(&usb_lock);
    int r = info(index, ui);
    spin_unlock(&usb_lock);
    return r;
}

static int info(int index, struct usbinfo *ui)
{
    for (int i = 0; i < MAX_DEVS; i++) {
        struct usb_dev *dev = devtab[i];
        if (!dev || index-- > 0)
            continue;
        memset(ui, 0, sizeof(*ui));
        ui->vendor = dev->desc.idVendor;
        ui->product = dev->desc.idProduct;
        ui->cls = dev->desc.bDeviceClass;
        ui->subclass = dev->desc.bDeviceSubClass;
        ui->protocol = dev->desc.bDeviceProtocol;
        if (!ui->cls && dev->nifaces) {
            ui->cls = dev->ifaces[0].cls;
            ui->subclass = dev->ifaces[0].sub;
            ui->protocol = dev->ifaces[0].proto;
        }
        ui->speed = (uint8_t)dev->speed;
        ui->addr = (uint8_t)dev->addr;
        ui->port = (uint8_t)dev->port;
        ui->depth = (uint8_t)dev->depth;
        int bus = 0;
        for (struct usb_hc *hc = hcs; hc && hc != dev->hc; hc = hc->next)
            bus++;
        ui->bus = (uint8_t)bus;
        strlcpy(ui->hc, dev->hc->name, sizeof(ui->hc));
        strlcpy(ui->manufacturer, dev->manufacturer, sizeof(ui->manufacturer));
        strlcpy(ui->name, dev->product, sizeof(ui->name));
        for (int k = 0; k < dev->nifaces; k++) {
            if (!dev->ifaces[k].drv || strstr(ui->driver, dev->ifaces[k].drv->name))
                continue;
            if (ui->driver[0])
                strlcat(ui->driver, ",", sizeof(ui->driver));
            strlcat(ui->driver, dev->ifaces[k].drv->name, sizeof(ui->driver));
        }
        return 0;
    }
    return -ENODEV;
}
