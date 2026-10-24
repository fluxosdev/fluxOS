/*
 * UHCI host controller driver (Intel and VIA USB 1.1: the PIIX3/PIIX4
 * southbridges, and ICH companions of EHCI).
 *
 * The controller walks a frame list once a millisecond. Every frame
 * points at the same chain of queue heads: interrupt endpoints first,
 * then control, then bulk, so interrupt endpoints are polled every
 * frame. Each endpoint has a queue head; a transfer hangs a chain of
 * transfer descriptors (one per packet) off it. Data toggles are kept
 * here, in the endpoint. Descriptors the controller may still be
 * reading are freed two frames after they are done with.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "usb.h"
#include "x86.h"

#define USBCMD    0x00
#define USBSTS    0x02
#define USBINTR   0x04
#define FRNUM     0x06
#define FRBASEADD 0x08
#define SOFMOD    0x0C
#define PORTSC    0x10

#define CMD_RS      0x0001
#define CMD_HCRESET 0x0002
#define CMD_CF      0x0040
#define CMD_MAXP    0x0080
#define STS_HCH     0x0020
#define STS_ACK     0x001F              /* the write-1-to-clear bits */

#define PORT_CCS  0x0001
#define PORT_CSC  0x0002
#define PORT_PE   0x0004
#define PORT_PEC  0x0008
#define PORT_LSDA 0x0100
#define PORT_PR   0x0200
#define PORT_RW   0x1244                /* the bits written back as read (PE, RD, PR, SUSP) */

#define LEGSUP         0xC0             /* PCI config: legacy support */
#define LEGSUP_RWC     0x8F00
#define LEGSUP_PIRQ    0x2000

#define LP_T     0x1                    /* link: terminate */
#define LP_QH    0x2                    /* link: points at a queue head */
#define LP_VF    0x4                    /* depth first */

#define TD_ACTIVE  (1u << 23)
#define TD_STALLED (1u << 22)
#define TD_ERR     (0x76u << 16)        /* stalled, buffer, babble, CRC/timeout, bitstuff */
#define TD_BADPKT  (0x36u << 16)        /* the same, less stalled: a broken packet */
#define TD_IOC     (1u << 24)
#define TD_LS      (1u << 26)
#define TD_CERR3   (3u << 27)
#define TD_SPD     (1u << 29)

#define PID_SETUP 0x2D
#define PID_IN    0x69
#define PID_OUT   0xE1

struct uhci_td {
    volatile uint32_t link, ctrl, token, buf;
    struct uhci_td *next;               /* this transfer's next descriptor */
    struct uhci_td *rnext;              /* next retired chain */
    uint16_t len;
    uint16_t frame;                     /* the frame its chain was retired in */
    uint8_t is_data, is_in, pad[2];
};

struct uhci_qh {
    volatile uint32_t link, elem;
    struct uhci_qh *prev, *next;        /* in the controller's chain */
    struct usb_xfer *xfer;
    struct uhci_td *tds;
    int type;
};

struct uhci {
    struct usb_hc hc;
    uint16_t io;
    uint32_t *frames;
    struct uhci_qh *skel[3];            /* interrupt, control, bulk */
    struct uhci_td *reclaim;            /* chains waiting to be freed */
    int halted;
};

static inline uint32_t phys(const void *p)
{
    return (uint32_t)p;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static uint16_t frame_now(struct uhci *u)
{
    return inw(u->io + FRNUM) & 0x7FF;
}

/* Wait until the controller has started two new frames (it is done with
 * anything unlinked before). A halted controller reads nothing anyway. */
static void wait_frames(struct uhci *u)
{
    uint16_t f = frame_now(u);
    for (int i = 0; i < 60 && ((frame_now(u) - f) & 0x7FF) < 2; i++)
        udelay(50);
}

static void free_chain(struct uhci_td *td)
{
    while (td) {
        struct uhci_td *n = td->next;
        usb_dma_free(td, sizeof(*td));
        td = n;
    }
}

/* Retire a transfer's descriptors; they are freed once two frames pass. */
static void retire(struct uhci *u, struct uhci_td *tds)
{
    if (!tds)
        return;
    tds->frame = frame_now(u);
    tds->rnext = u->reclaim;
    u->reclaim = tds;
}

static void reclaim(struct uhci *u)
{
    uint16_t now = frame_now(u);
    struct uhci_td **pp = &u->reclaim;
    while (*pp) {
        struct uhci_td *td = *pp;
        if (((now - td->frame) & 0x7FF) >= 2 || u->halted) {
            *pp = td->rnext;
            free_chain(td);
        } else {
            pp = &td->rnext;
        }
    }
}

/* ---- queue heads ---- */

static struct uhci_qh *qh_new(int type)
{
    struct uhci_qh *qh = usb_dma_alloc(sizeof(*qh));
    if (!qh)
        return NULL;
    qh->link = LP_T;
    qh->elem = LP_T;
    qh->type = type;
    return qh;
}

static void qh_link(struct uhci *u, struct uhci_qh *qh)
{
    struct uhci_qh *s = u->skel[qh->type == USB_EP_INTR ? 0 : qh->type == USB_EP_CONTROL ? 1 : 2];
    qh->link = s->link;
    qh->next = s->next;
    qh->prev = s;
    if (s->next)
        s->next->prev = qh;
    s->next = qh;
    barrier();
    s->link = phys(qh) | LP_QH;
}

static void qh_unlink(struct uhci_qh *qh)
{
    qh->prev->link = qh->link;
    qh->prev->next = qh->next;
    if (qh->next)
        qh->next->prev = qh->prev;
}

static struct uhci_qh *ep_qh(struct uhci *u, struct usb_ep *ep)
{
    if (!ep->hcpriv) {
        struct uhci_qh *qh = qh_new(ep->type);
        if (!qh)
            return NULL;
        qh_link(u, qh);
        ep->hcpriv = qh;
    }
    return ep->hcpriv;
}

/* ---- transfers ---- */

static struct uhci_td *td_new(struct usb_dev *dev, struct usb_ep *ep, int pid, int toggle,
                              void *buf, uint32_t len)
{
    struct uhci_td *td = usb_dma_alloc(sizeof(*td));
    if (!td)
        return NULL;
    td->link = LP_T;
    td->ctrl = TD_ACTIVE | TD_CERR3 | (dev->speed == USB_SPEED_LOW ? TD_LS : 0);
    td->token = (uint32_t)pid | (uint32_t)dev->addr << 8 | (uint32_t)(ep->addr & 0xF) << 15 |
                (uint32_t)(toggle & 1) << 19 | ((len - 1) & 0x7FF) << 21;
    td->buf = len ? phys(buf) : 0;
    td->len = len;
    td->is_in = pid == PID_IN;
    return td;
}

static int uhci_submit(struct usb_hc *hc, struct usb_xfer *x)
{
    struct uhci *u = hc->priv;
    struct usb_ep *ep = x->ep;
    struct usb_dev *dev = x->dev;
    if (ep->type == USB_EP_ISO)
        return -EINVAL;
    struct uhci_qh *qh = ep_qh(u, ep);
    if (!qh)
        return -ENOMEM;
    if (qh->xfer)
        return -EBUSY;

    struct uhci_td *first = NULL, *last = NULL, *td;
#define APPEND(t) do { if (!(td = (t))) goto nomem; \
        if (last) { last->next = td; last->link = phys(td) | LP_VF; } else first = td; \
        last = td; } while (0)

    int in, toggle;
    uint32_t mps = ep->maxpacket ? ep->maxpacket : 8;
    if (ep->type == USB_EP_CONTROL) {
        APPEND(td_new(dev, ep, PID_SETUP, 0, x->setup, 8));
        in = (x->setup[0] & USB_RT_IN) != 0;
        toggle = 1;
    } else {
        in = (ep->addr & USB_DIR_IN) != 0;
        toggle = ep->toggle;
    }
    uint8_t *p = x->buf;
    uint32_t left = x->len;
    do {
        uint32_t n = MIN(left, mps);
        APPEND(td_new(dev, ep, in ? PID_IN : PID_OUT, toggle, p, n));
        td->is_data = 1;
        if (in)
            td->ctrl |= TD_SPD;
        toggle ^= 1;
        p += n;
        left -= n;
    } while (left);
    if (ep->type == USB_EP_CONTROL) {
        if (!x->len) {                  /* no data stage: drop the empty one */
            struct uhci_td *empty = last;
            last = first;
            last->next = NULL;
            last->link = LP_T;
            usb_dma_free(empty, sizeof(*empty));
        }
        APPEND(td_new(dev, ep, (in && x->len) ? PID_OUT : PID_IN, 1, NULL, 0));
    }
#undef APPEND
    last->ctrl |= TD_IOC;

    qh->xfer = x;
    qh->tds = first;
    x->hcpriv = qh;
    barrier();
    qh->elem = phys(first);
    return 0;

nomem:
    free_chain(first);
    return -ENOMEM;
}

/*
 * Has the transfer finished? Returns USB_PENDING, or its status with
 * *actual set. A short packet ends the data stage: the controller stops
 * at it (SPD), and a control transfer goes on with its status stage.
 */
static int check(struct uhci_qh *qh, uint32_t *actual, int *toggle)
{
    struct usb_xfer *x = qh->xfer;
    uint32_t got = 0;
    int control = x->ep->type == USB_EP_CONTROL;
    for (struct uhci_td *td = qh->tds; td; td = td->next) {
        uint32_t c = td->ctrl;
        if (c & TD_ACTIVE)
            return USB_PENDING;
        if (c & TD_ERR) {
            *toggle = (td->token >> 19) & 1;
            *actual = got;
            return (c & TD_STALLED) && !(c & TD_BADPKT) ? -EPIPE : -EIO;
        }
        uint32_t n = (c + 1) & 0x7FF;
        if (td->is_data) {
            got += n;
            *toggle = !((td->token >> 19) & 1);
        }
        if (td->is_data && td->is_in && n < td->len) {
            *actual = got;
            if (!control)
                return 0;
            /* Skip to the status stage. */
            struct uhci_td *st = td;
            while (st->next)
                st = st->next;
            if (st->ctrl & TD_ACTIVE) {
                qh->elem = phys(st);
                return USB_PENDING;
            }
            return (st->ctrl & TD_ERR) ? -EIO : 0;
        }
    }
    *actual = got;
    return 0;
}

static void uhci_poll(struct usb_hc *hc)
{
    struct uhci *u = hc->priv;
    uint16_t sts = inw(u->io + USBSTS);
    if (sts & STS_ACK)
        outw(u->io + USBSTS, sts & STS_ACK);
    if ((sts & 0x18) && !u->halted) {
        u->halted = 1;
        klog("uhci: controller error (status %04x), stopped\n", sts);
    }
    reclaim(u);

    struct usb_xfer *done = NULL;
    for (int s = 0; s < 3; s++) {
        for (struct uhci_qh *qh = u->skel[s]->next; qh; qh = qh->next) {
            if (!qh->xfer)
                continue;
            uint32_t actual = 0;
            int toggle = qh->xfer->ep->toggle;
            int st = check(qh, &actual, &toggle);
            if (st == USB_PENDING)
                continue;
            struct usb_xfer *x = qh->xfer;
            qh->elem = LP_T;            /* it may still point at a short or failed one */
            if (x->ep->type != USB_EP_CONTROL)
                x->ep->toggle = (uint8_t)toggle;
            retire(u, qh->tds);
            qh->tds = NULL;
            qh->xfer = NULL;
            x->status = st;
            x->actual = actual;
            x->hcnext = done;
            done = x;
        }
    }
    while (done) {
        struct usb_xfer *x = done;
        done = x->hcnext;
        usb_xfer_done(x, x->status, x->actual);
    }
}

static void uhci_cancel(struct usb_hc *hc, struct usb_xfer *x)
{
    struct uhci *u = hc->priv;
    struct uhci_qh *qh = x->ep->hcpriv;
    if (!qh || qh->xfer != x)
        return;
    qh->elem = LP_T;
    wait_frames(u);
    /* The toggle of the last packet that got through. */
    for (struct uhci_td *td = qh->tds; td; td = td->next)
        if (td->is_data && !(td->ctrl & (TD_ACTIVE | TD_ERR)))
            x->ep->toggle = !((td->token >> 19) & 1);
    free_chain(qh->tds);
    qh->tds = NULL;
    qh->xfer = NULL;
}

static void uhci_dev_free(struct usb_hc *hc, struct usb_dev *dev)
{
    struct uhci *u = hc->priv;
    struct usb_ep *eps[33];
    int unlinked = 0;
    eps[0] = &dev->ep0;
    for (int i = 0; i < 32; i++)
        eps[i + 1] = &dev->eps[i];
    for (int i = 0; i < 33; i++) {
        if (eps[i]->hcpriv) {
            qh_unlink(eps[i]->hcpriv);
            unlinked = 1;
        }
    }
    if (!unlinked)
        return;
    wait_frames(u);
    for (int i = 0; i < 33; i++) {
        struct uhci_qh *qh = eps[i]->hcpriv;
        if (!qh)
            continue;
        if (qh->xfer) {
            qh->xfer->status = -ENODEV;
            wakeup(qh->xfer);
        }
        free_chain(qh->tds);
        usb_dma_free(qh, sizeof(*qh));
        eps[i]->hcpriv = NULL;
    }
}

/* ---- root hub ---- */

static uint16_t port_reg(struct usb_hub *h, int port)
{
    return (uint16_t)(((struct uhci *)h->hc->priv)->io + PORTSC + 2 * (port - 1));
}

static int uhci_port_status(struct usb_hub *h, int port)
{
    uint16_t reg = port_reg(h, port);
    uint16_t v = inw(reg);
    if (v & (PORT_CSC | PORT_PEC))
        outw(reg, (uint16_t)((v & PORT_RW) | (v & (PORT_CSC | PORT_PEC))));
    return ((v & PORT_CCS) ? PORT_CONNECT : 0) | ((v & PORT_PE) ? PORT_ENABLE : 0) |
           ((v & PORT_CSC) ? PORT_CHANGE : 0);
}

static int uhci_port_reset(struct usb_hub *h, int port)
{
    uint16_t reg = port_reg(h, port);
    outw(reg, (uint16_t)((inw(reg) & PORT_RW) | PORT_PR));
    mdelay(50);
    outw(reg, (uint16_t)(inw(reg) & PORT_RW & ~PORT_PR));
    udelay(100);
    for (int i = 0; i < 10; i++) {
        uint16_t v = inw(reg);
        if (!(v & PORT_CCS))
            return -ENODEV;
        if (v & (PORT_CSC | PORT_PEC)) {
            outw(reg, (uint16_t)((v & PORT_RW) | (v & (PORT_CSC | PORT_PEC))));
            continue;
        }
        if (v & PORT_PE)
            return (v & PORT_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
        outw(reg, (uint16_t)((v & PORT_RW) | PORT_PE));
        mdelay(10);
    }
    return -EIO;
}

static void uhci_port_disable(struct usb_hub *h, int port)
{
    uint16_t reg = port_reg(h, port);
    outw(reg, (uint16_t)(inw(reg) & PORT_RW & ~PORT_PE));
}

static void uhci_irq(struct trapframe *tf);

static const struct usb_hc_ops uhci_ops = {
    .submit = uhci_submit,
    .cancel = uhci_cancel,
    .poll = uhci_poll,
    .dev_free = uhci_dev_free,
};

#define MAX_UHCI 8
static struct uhci *uhcis[MAX_UHCI];
static int nuhci;

static void uhci_irq(struct trapframe *tf)
{
    (void)tf;
    for (int i = 0; i < nuhci; i++)
        if (inw(uhcis[i]->io + USBSTS) & STS_ACK)
            uhci_poll(&uhcis[i]->hc);
}

/*
 * Ports are the 16-bit registers from 0x10 on that read with bit 7 set.
 * Some controllers (QEMU's among them) answer that way past their last
 * port too; eight of them means that, and the usual two, as Linux does.
 */
static int count_ports(uint16_t io)
{
    int n = 0;
    while (n < 8) {
        uint16_t v = inw((uint16_t)(io + PORTSC + 2 * n));
        if (!(v & 0x80) || v == 0xFFFF)
            break;
        n++;
    }
    return n == 0 || n == 8 ? 2 : n;
}

static void uhci_init(const struct pci_dev *pci)
{
    uint32_t bar = pci_read32(pci, PCI_BAR4);
    if (!(bar & 1) || !(bar & 0xFFE0))
        return;
    struct uhci *u = kzalloc(sizeof(*u));
    uint32_t page = pmm_alloc();
    if (!u || !page) {
        kfree(u);
        return;
    }
    u->io = (uint16_t)(bar & 0xFFE0);
    pci_enable(pci, PCI_CMD_IO | PCI_CMD_MASTER);

    /* Take it from the BIOS (no more SMIs or PS/2 emulation), then reset. */
    pci_write16(pci, LEGSUP, LEGSUP_RWC);
    outw(u->io + USBINTR, 0);
    outw(u->io + USBCMD, 0);
    for (int i = 0; i < 20 && !(inw(u->io + USBSTS) & STS_HCH); i++)
        mdelay(1);
    outw(u->io + USBCMD, CMD_HCRESET);
    for (int i = 0; i < 50 && (inw(u->io + USBCMD) & CMD_HCRESET); i++)
        mdelay(1);
    outw(u->io + USBINTR, 0);

    for (int s = 0; s < 3; s++)
        u->skel[s] = qh_new(s == 0 ? USB_EP_INTR : s == 1 ? USB_EP_CONTROL : USB_EP_BULK);
    if (!u->skel[0] || !u->skel[1] || !u->skel[2]) {
        pmm_free(page);
        kfree(u);
        return;
    }
    u->skel[0]->link = phys(u->skel[1]) | LP_QH;
    u->skel[1]->link = phys(u->skel[2]) | LP_QH;
    u->frames = (uint32_t *)page;
    for (int i = 0; i < 1024; i++)
        u->frames[i] = phys(u->skel[0]) | LP_QH;

    outl(u->io + FRBASEADD, page);
    outw(u->io + FRNUM, 0);
    outb(u->io + SOFMOD, 0x40);
    outw(u->io + USBSTS, 0xFFFF);

    struct usb_hc *hc = &u->hc;
    hc->name = "UHCI";
    hc->ops = &uhci_ops;
    hc->priv = u;
    hc->irq = cmdline_has("usb.poll") ? -1 : pci_irq(pci);
    hc->root.nports = count_ports(u->io);
    hc->root.port_status = uhci_port_status;
    hc->root.port_reset = uhci_port_reset;
    hc->root.port_disable = uhci_port_disable;
    hc->root.priv = u;

    outw(u->io + USBINTR, hc->irq >= 0 ? 0x000D : 0);  /* CRC/timeout, IOC, short packet */
    outw(u->io + USBCMD, CMD_RS | CMD_CF | CMD_MAXP);
    pci_write16(pci, LEGSUP, LEGSUP_PIRQ);

    uhcis[nuhci++] = u;
    if (hc->irq >= 0)
        irq_register(hc->irq, uhci_irq);
    usb_register_hc(hc);
    boot_ok("USB: UHCI controller at I/O %04x, IRQ %d, %d ports", u->io, hc->irq,
            hc->root.nports);
}

void uhci_probe(void)
{
    struct pci_dev pci;
    uint8_t prog_if;
    for (int i = 0; nuhci < MAX_UHCI && pci_find_class_nth(0x0C03, i, &pci, &prog_if) == 0; i++)
        if (prog_if == 0x00)
            uhci_init(&pci);
}
