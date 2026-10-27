/*
 * EHCI host controller driver (USB 2.0, high speed).
 *
 * Control and bulk endpoints sit in the asynchronous schedule, a ring of
 * queue heads (QHs) the controller goes round continuously; interrupt
 * endpoints hang off the periodic frame list, every entry of which points
 * at the same chain, so they are polled every frame. A transfer is a
 * chain of qTDs (up to 16 KiB each) given to the endpoint's QH. Low and
 * full speed devices on a root port are handed to the companion UHCI or
 * OHCI controller; behind a high-speed hub (such as the rate-matching hub
 * of newer Intel chipsets) they are reached with split transactions
 * through the hub's transaction translator.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "usb.h"

/* Capability registers */
#define CAP_LENGTH   0x00
#define CAP_HCSPARAMS 0x04
#define CAP_HCCPARAMS 0x08

/* Operational registers */
#define OP_USBCMD    0x00
#define OP_USBSTS    0x04
#define OP_USBINTR   0x08
#define OP_FRINDEX   0x0C
#define OP_CTRLDSSEG 0x10
#define OP_PERIODIC  0x14
#define OP_ASYNC     0x18
#define OP_CONFIGFLAG 0x40
#define OP_PORTSC    0x44

#define CMD_RS      (1u << 0)
#define CMD_HCRESET (1u << 1)
#define CMD_PSE     (1u << 4)
#define CMD_ASE     (1u << 5)
#define CMD_IAAD    (1u << 6)
#define CMD_ITC8    (8u << 16)
#define STS_INT     (1u << 0)
#define STS_ERR     (1u << 1)
#define STS_PCD     (1u << 2)
#define STS_HSE     (1u << 4)
#define STS_IAA     (1u << 5)
#define STS_HALTED  (1u << 12)
#define STS_ACK     0x3Fu

#define PORT_CCS    (1u << 0)
#define PORT_CSC    (1u << 1)
#define PORT_PE     (1u << 2)
#define PORT_PEC    (1u << 3)
#define PORT_OCC    (1u << 5)
#define PORT_PR     (1u << 8)
#define PORT_LS_K   (1u << 10)          /* line status K: a low-speed device */
#define PORT_LS     (3u << 10)
#define PORT_PP     (1u << 12)
#define PORT_OWNER  (1u << 13)
#define PORT_RWC    (PORT_CSC | PORT_PEC | PORT_OCC)

#define LP_T    1u
#define LP_QH   2u                      /* link type: queue head */

#define QTD_ACTIVE  0x80u
#define QTD_HALTED  0x40u
#define QTD_BUFERR  0x20u
#define QTD_BABBLE  0x10u
#define QTD_XACT    0x08u
#define QTD_PID_OUT   (0u << 8)
#define QTD_PID_IN    (1u << 8)
#define QTD_PID_SETUP (2u << 8)
#define QTD_CERR3   (3u << 10)
#define QTD_IOC     (1u << 15)
#define QTD_DT      (1u << 31)
#define QTD_MAX     0x4000u             /* bytes per qTD: 16 KiB fits any alignment */

#define QH_DTC      (1u << 14)
#define QH_HEAD     (1u << 15)
#define QH_CTRL     (1u << 27)

/* A qTD; with the extended (64-bit) buffer pointers zeroed, then ours. */
struct qtd {
    volatile uint32_t next, alt, token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    struct qtd *snext;                  /* this transfer's next qTD */
    struct qtd *rnext;                  /* next retired chain */
    uint16_t len;
    uint16_t frame;
    uint8_t is_data, is_in, is_status, pad;
};

struct qh {
    volatile uint32_t link, info1, info2, cur_qtd;
    /* overlay */
    volatile uint32_t next, alt, token;
    volatile uint32_t buf[5];
    volatile uint32_t buf_hi[5];
    uint32_t hw_pad[3];
    /* ours */
    struct qh *sprev, *snext;
    struct usb_xfer *xfer;
    struct qtd *tds;
    struct qtd *end;                    /* inactive: where a short packet goes */
    int type;
};

struct ehci {
    struct usb_hc hc;
    struct pci_dev pci;
    volatile uint8_t *cap, *op;
    uint32_t *frames;
    struct qh *async_head;              /* the ring's fixed head */
    struct qh *intr_head;               /* every frame list entry points here */
    struct qtd *reclaim;
    int ncompanions;
    volatile int iaa;
};

static inline uint32_t op_rd(struct ehci *e, int reg)
{
    return *(volatile uint32_t *)(e->op + reg);
}

static inline void op_wr(struct ehci *e, int reg, uint32_t v)
{
    *(volatile uint32_t *)(e->op + reg) = v;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static uint16_t frame_now(struct ehci *e)
{
    return (uint16_t)((op_rd(e, OP_FRINDEX) >> 3) & 0x7FF);
}

static void wait_frames(struct ehci *e)
{
    uint16_t f = frame_now(e);
    for (int i = 0; i < 60 && ((frame_now(e) - f) & 0x7FF) < 2; i++)
        udelay(50);
}

static void free_chain(struct qtd *t)
{
    while (t) {
        struct qtd *n = t->snext;
        usb_dma_free(t, sizeof(*t));
        t = n;
    }
}

static void retire(struct ehci *e, struct qtd *tds)
{
    if (!tds)
        return;
    tds->frame = frame_now(e);
    tds->rnext = e->reclaim;
    e->reclaim = tds;
}

static void reclaim(struct ehci *e)
{
    uint16_t now = frame_now(e);
    struct qtd **pp = &e->reclaim;
    while (*pp) {
        struct qtd *t = *pp;
        if (((now - t->frame) & 0x7FF) >= 2) {
            *pp = t->rnext;
            free_chain(t);
        } else {
            pp = &t->rnext;
        }
    }
}

/*
 * Wait until the controller has let go of the async QHs unlinked before:
 * ring the "interrupt on async advance" doorbell and wait for its answer.
 */
static void async_doorbell(struct ehci *e)
{
    if (op_rd(e, OP_USBSTS) & STS_HALTED)
        return;
    e->iaa = 0;
    op_wr(e, OP_USBSTS, STS_IAA);
    op_wr(e, OP_USBCMD, op_rd(e, OP_USBCMD) | CMD_IAAD);
    for (int i = 0; i < 200 && !e->iaa && !(op_rd(e, OP_USBSTS) & STS_IAA); i++)
        udelay(50);
    op_wr(e, OP_USBSTS, STS_IAA);
}

/* ---- queue heads ---- */

static struct qh *qh_alloc(void)
{
    struct qh *qh = usb_dma_alloc(sizeof(*qh));
    if (!qh)
        return NULL;
    qh->link = LP_T;
    qh->next = LP_T;
    qh->alt = LP_T;
    return qh;
}

static void qh_link(struct ehci *e, struct qh *qh)
{
    struct qh *h = qh->type == USB_EP_INTR ? e->intr_head : e->async_head;
    qh->link = h->link;
    qh->snext = h->snext;
    qh->sprev = h;
    if (h->snext)
        h->snext->sprev = qh;
    h->snext = qh;
    barrier();
    h->link = (uint32_t)qh | LP_QH;
}

static void qh_unlink(struct qh *qh)
{
    qh->sprev->link = qh->link;
    qh->sprev->snext = qh->snext;
    if (qh->snext)
        qh->snext->sprev = qh->sprev;
}

/* Interrupt endpoints: which microframes to poll in (S-mask). */
static uint32_t smask_for(struct usb_dev *dev, struct usb_ep *ep)
{
    if (dev->speed != USB_SPEED_HIGH)
        return 0x01;                    /* start split in microframe 0 */
    int b = ep->interval ? ep->interval : 1;
    return b == 1 ? 0xFF : b == 2 ? 0x55 : b == 3 ? 0x11 : 0x01;
}

static struct qh *ep_qh(struct ehci *e, struct usb_dev *dev, struct usb_ep *ep)
{
    if (ep->hcpriv)
        return ep->hcpriv;
    struct qh *qh = qh_alloc();
    struct qtd *end = usb_dma_alloc(sizeof(struct qtd));
    if (!qh || !end) {
        usb_dma_free(qh, sizeof(*qh));
        usb_dma_free(end, sizeof(*end));
        return NULL;
    }
    end->next = LP_T;
    end->alt = LP_T;
    qh->end = end;
    qh->type = ep->type;

    uint32_t eps = dev->speed == USB_SPEED_HIGH ? 2 : dev->speed == USB_SPEED_LOW ? 1 : 0;
    uint32_t mps = ep->maxpacket ? ep->maxpacket : 8;
    qh->info1 = (uint32_t)dev->addr | (uint32_t)(ep->addr & 0xF) << 8 | eps << 12 | mps << 16;
    if (ep->type == USB_EP_CONTROL) {
        qh->info1 |= QH_DTC;            /* toggles from the qTDs */
        if (dev->speed != USB_SPEED_HIGH)
            qh->info1 |= QH_CTRL;
    }
    uint32_t mult = ep->type == USB_EP_INTR && dev->speed == USB_SPEED_HIGH ? ep->mult + 1u : 1u;
    qh->info2 = mult << 30;
    if (ep->type == USB_EP_INTR) {
        qh->info2 |= smask_for(dev, ep);
        if (dev->speed != USB_SPEED_HIGH)
            qh->info2 |= 0x1Cu << 8;    /* complete splits in microframes 2-4 */
    }
    if (dev->speed != USB_SPEED_HIGH && dev->tt_hub)
        qh->info2 |= (uint32_t)dev->tt_hub->addr << 16 | (uint32_t)dev->tt_port << 23;
    qh_link(e, qh);
    ep->hcpriv = qh;
    return qh;
}

/* ---- transfers ---- */

static struct qtd *qtd_new(uint32_t pid, int toggle, void *buf, uint32_t len)
{
    struct qtd *t = usb_dma_alloc(sizeof(*t));
    if (!t)
        return NULL;
    t->next = LP_T;
    t->alt = LP_T;
    t->token = QTD_ACTIVE | pid | QTD_CERR3 | len << 16 | (toggle ? QTD_DT : 0);
    uint32_t a = (uint32_t)buf;
    if (len) {
        t->buf[0] = a;
        for (int i = 1; i < 5; i++)
            t->buf[i] = (a & ~0xFFFu) + 0x1000u * (uint32_t)i;
    }
    t->len = (uint16_t)len;
    t->is_in = pid == QTD_PID_IN;
    return t;
}

static int ehci_submit(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ehci *e = hc->priv;
    struct usb_ep *ep = x->ep;
    if (ep->type == USB_EP_ISO)
        return -EINVAL;
    struct qh *qh = ep_qh(e, x->dev, ep);
    if (!qh)
        return -ENOMEM;
    if (qh->xfer)
        return -EBUSY;

    struct qtd *first = NULL, *last = NULL, *t, *status = NULL;
#define APPEND(n) do { if (!(t = (n))) goto nomem; \
        if (last) { last->snext = t; last->next = (uint32_t)t; } else first = t; \
        last = t; } while (0)

    int control = ep->type == USB_EP_CONTROL;
    int in = control ? (x->setup[0] & USB_RT_IN) != 0 : (ep->addr & USB_DIR_IN) != 0;
    if (control)
        APPEND(qtd_new(QTD_PID_SETUP, 0, x->setup, 8));
    uint8_t *p = x->buf;
    uint32_t left = x->len;
    int ndata = 0;
    if (x->len || !control) {
        do {
            uint32_t n = MIN(left, QTD_MAX);
            APPEND(qtd_new(in ? QTD_PID_IN : QTD_PID_OUT, control ? 1 : 0, p, n));
            t->is_data = 1;
            ndata++;
            p += n;
            left -= n;
        } while (left);
    }
    if (control) {
        APPEND(qtd_new((in && x->len) ? QTD_PID_OUT : QTD_PID_IN, 1, NULL, 0));
        t->is_status = 1;
        status = t;
    }
#undef APPEND
    last->token |= QTD_IOC;

    /* A short IN packet skips the rest: to the status stage, or to the end. */
    for (t = first; t; t = t->snext)
        if (t->is_data && t->is_in)
            t->alt = status ? (uint32_t)status : (uint32_t)qh->end;

    qh->xfer = x;
    qh->tds = first;
    x->hcpriv = qh;
    /* Give the idle QH its new qTDs; the overlay keeps the data toggle. */
    qh->token &= QTD_DT;
    qh->alt = LP_T;
    barrier();
    qh->next = (uint32_t)first;
    (void)ndata;
    return 0;

nomem:
    free_chain(first);
    return -ENOMEM;
}

static int check(struct qh *qh, uint32_t *actual)
{
    struct usb_xfer *x = qh->xfer;
    int control = x->ep->type == USB_EP_CONTROL;
    uint32_t got = 0;
    for (struct qtd *t = qh->tds; t; t = t->snext) {
        uint32_t tok = t->token;
        if (tok & QTD_HALTED) {
            *actual = got;
            return (tok & (QTD_BUFERR | QTD_BABBLE | QTD_XACT)) ? -EIO : -EPIPE;
        }
        if (tok & QTD_ACTIVE)
            return USB_PENDING;
        uint32_t n = t->len - ((tok >> 16) & 0x7FFF);
        if (t->is_data)
            got += n;
        if (t->is_data && t->is_in && n < t->len) {
            *actual = got;
            if (!control)
                return 0;
            while (t->snext && !t->snext->is_status)
                t = t->snext;
        }
    }
    *actual = got;
    return 0;
}

/* Make a halted or abandoned QH idle again (it must not be in use). */
static void qh_idle(struct qh *qh, int keep_toggle)
{
    qh->next = LP_T;
    qh->alt = LP_T;
    qh->token = keep_toggle ? (qh->token & QTD_DT) : 0;
}

static void ehci_poll(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;
    uint32_t sts = op_rd(e, OP_USBSTS);
    if (sts & STS_IAA)
        e->iaa = 1;
    if (sts & (STS_INT | STS_ERR | STS_PCD | STS_HSE))
        op_wr(e, OP_USBSTS, sts & (STS_INT | STS_ERR | STS_PCD | STS_HSE));
    reclaim(e);

    struct usb_xfer *done = NULL;
    for (int l = 0; l < 2; l++) {
        struct qh *h = l ? e->async_head : e->intr_head;
        for (struct qh *qh = h->snext; qh; qh = qh->snext) {
            if (!qh->xfer)
                continue;
            uint32_t actual = 0;
            int st = check(qh, &actual);
            if (st == USB_PENDING)
                continue;
            struct usb_xfer *x = qh->xfer;
            if (st < 0)
                qh_idle(qh, 0);         /* the controller skips a halted QH */
            retire(e, qh->tds);
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

/* Take the QH out of the schedule until the controller has let go of it. */
static void qh_detach(struct ehci *e, struct qh *qh)
{
    qh_unlink(qh);
    if (qh->type == USB_EP_INTR)
        wait_frames(e);
    else
        async_doorbell(e);
}

static void ehci_cancel(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ehci *e = hc->priv;
    struct qh *qh = x->ep->hcpriv;
    if (!qh || qh->xfer != x)
        return;
    qh_detach(e, qh);
    qh_idle(qh, 1);
    free_chain(qh->tds);
    qh->tds = NULL;
    qh->xfer = NULL;
    qh_link(e, qh);
}

static void ehci_dev_free(struct usb_hc *hc, struct usb_dev *dev)
{
    struct ehci *e = hc->priv;
    struct usb_ep *eps[33];
    int async = 0, periodic = 0;
    eps[0] = &dev->ep0;
    for (int i = 0; i < 32; i++)
        eps[i + 1] = &dev->eps[i];
    for (int i = 0; i < 33; i++) {
        struct qh *qh = eps[i]->hcpriv;
        if (!qh)
            continue;
        qh_unlink(qh);
        if (qh->type == USB_EP_INTR)
            periodic = 1;
        else
            async = 1;
    }
    if (periodic)
        wait_frames(e);
    if (async)
        async_doorbell(e);
    for (int i = 0; i < 33; i++) {
        struct qh *qh = eps[i]->hcpriv;
        if (!qh)
            continue;
        if (qh->xfer) {
            qh->xfer->status = -ENODEV;
            wakeup(qh->xfer);
        }
        free_chain(qh->tds);
        usb_dma_free(qh->end, sizeof(struct qtd));
        usb_dma_free(qh, sizeof(*qh));
        eps[i]->hcpriv = NULL;
    }
}

static void ehci_ep_reset(struct usb_hc *hc, struct usb_dev *dev, struct usb_ep *ep)
{
    (void)hc; (void)dev;
    struct qh *qh = ep->hcpriv;
    if (qh && !qh->xfer)
        qh_idle(qh, 0);                 /* DATA0, not halted */
}

/* ---- root hub ---- */

static int port_reg(int port)
{
    return OP_PORTSC + 4 * (port - 1);
}

static int ehci_port_status(struct usb_hub *h, int port)
{
    struct ehci *e = h->hc->priv;
    uint32_t v = op_rd(e, port_reg(port));
    if (v & PORT_RWC)
        op_wr(e, port_reg(port), (v & ~PORT_RWC) | (v & PORT_RWC));
    if (v & PORT_OWNER)
        return (v & PORT_CSC) ? PORT_CHANGE : 0;        /* the companion's now */
    return ((v & PORT_CCS) ? PORT_CONNECT : 0) | ((v & PORT_PE) ? PORT_ENABLE : 0) |
           ((v & PORT_CSC) ? PORT_CHANGE : 0);
}

/* Give a low or full speed device to the companion controller. */
static int hand_over(struct ehci *e, int port)
{
    uint32_t v = op_rd(e, port_reg(port));
    if (!e->ncompanions) {
        klog("ehci: port %d: a low/full speed device, but no companion controller\n", port);
        return -ENODEV;
    }
    op_wr(e, port_reg(port), (v & ~PORT_RWC) | PORT_OWNER);
    return -EXDEV;
}

static int ehci_port_reset(struct usb_hub *h, int port)
{
    struct ehci *e = h->hc->priv;
    int reg = port_reg(port);
    uint32_t v = op_rd(e, reg);
    if ((v & PORT_LS) == PORT_LS_K)
        return hand_over(e, port);
    op_wr(e, reg, (v & ~(PORT_RWC | PORT_PE)) | PORT_PR);
    usb_mdelay(50);
    op_wr(e, reg, op_rd(e, reg) & ~(PORT_RWC | PORT_PR));
    for (int i = 0; i < 20 && (op_rd(e, reg) & PORT_PR); i++)
        usb_mdelay(1);
    usb_mdelay(2);
    v = op_rd(e, reg);
    if (!(v & PORT_CCS))
        return -ENODEV;
    if (!(v & PORT_PE))
        return hand_over(e, port);      /* it did not chirp: full speed */
    return USB_SPEED_HIGH;
}

static void ehci_port_disable(struct usb_hub *h, int port)
{
    struct ehci *e = h->hc->priv;
    uint32_t v = op_rd(e, port_reg(port));
    op_wr(e, port_reg(port), v & ~(PORT_RWC | PORT_PE));
}

static void ehci_suspend(struct usb_hc *hc);
static void ehci_resume(struct usb_hc *hc);

static const struct usb_hc_ops ehci_ops = {
    .submit = ehci_submit,
    .cancel = ehci_cancel,
    .poll = ehci_poll,
    .dev_free = ehci_dev_free,
    .ep_reset = ehci_ep_reset,
    .suspend = ehci_suspend,
    .resume = ehci_resume,
};

#define MAX_EHCI 4
static struct ehci *ehcis[MAX_EHCI];
static int nehci;

static void ehci_irq(struct trapframe *tf)
{
    (void)tf;
    spin_lock(&usb_lock);
    for (int i = 0; i < nehci; i++) {
        struct ehci *e = ehcis[i];
        uint32_t sts = op_rd(e, OP_USBSTS) & op_rd(e, OP_USBINTR);
        if (sts & STS_ACK) {
            if (sts & STS_IAA)
                e->iaa = 1;
            op_wr(e, OP_USBSTS, sts & STS_ACK);
            ehci_poll(&e->hc);
        }
    }
    spin_unlock(&usb_lock);
}

/* Take the controller from the BIOS (the USB legacy support capability). */
static void bios_handoff(const struct pci_dev *pci, uint32_t hccparams)
{
    int eecp = (hccparams >> 8) & 0xFF;
    for (int guard = 0; eecp >= 0x40 && guard < 16; guard++) {
        uint32_t v = pci_read32(pci, eecp);
        if ((v & 0xFF) == 1) {
            if (v & (1u << 16)) {
                pci_write32(pci, eecp, v | (1u << 24));
                for (int i = 0; i < 100 && (pci_read32(pci, eecp) & (1u << 16)); i++)
                    usb_mdelay(10);
                if (pci_read32(pci, eecp) & (1u << 16)) {
                    klog("ehci: BIOS would not let go; taking the controller anyway\n");
                    pci_write32(pci, eecp, (pci_read32(pci, eecp) & ~(1u << 16)) | (1u << 24));
                }
            }
            /* No more SMIs; clear the ones pending. */
            pci_write32(pci, eecp + 4, pci_read32(pci, eecp + 4) & 0xE0000000u);
        }
        eecp = (v >> 8) & 0xFF;
    }
}

/* From the BIOS, reset, the schedules in, running, every port ours and powered. */
static void ehci_setup(struct ehci *e)
{
    struct usb_hc *hc = &e->hc;
    uint32_t hcs = *(volatile uint32_t *)(e->cap + CAP_HCSPARAMS);
    uint32_t hcc = *(volatile uint32_t *)(e->cap + CAP_HCCPARAMS);
    bios_handoff(&e->pci, hcc);

    op_wr(e, OP_USBINTR, 0);
    op_wr(e, OP_USBCMD, op_rd(e, OP_USBCMD) & ~CMD_RS);
    for (int i = 0; i < 20 && !(op_rd(e, OP_USBSTS) & STS_HALTED); i++)
        usb_mdelay(1);
    op_wr(e, OP_USBCMD, CMD_HCRESET);
    for (int i = 0; i < 250 && (op_rd(e, OP_USBCMD) & CMD_HCRESET); i++)
        usb_mdelay(1);
    if (hcc & 1)
        op_wr(e, OP_CTRLDSSEG, 0);      /* 64-bit capable: our structures are below 4 GiB */

    /* The async ring starts as its head alone; the head never runs. */
    struct qh *ah = e->async_head;
    ah->link = (uint32_t)ah | LP_QH;
    ah->info1 = QH_HEAD | 2u << 12;
    ah->token = QTD_HALTED;
    ah->type = USB_EP_CONTROL;
    e->intr_head->type = USB_EP_INTR;
    for (int i = 0; i < 1024; i++)
        e->frames[i] = (uint32_t)e->intr_head | LP_QH;
    op_wr(e, OP_PERIODIC, (uint32_t)e->frames);
    op_wr(e, OP_ASYNC, (uint32_t)ah);
    op_wr(e, OP_USBSTS, STS_ACK);

    if (hc->irq >= 0)
        op_wr(e, OP_USBINTR, STS_INT | STS_ERR | STS_HSE);
    op_wr(e, OP_USBCMD, CMD_ITC8 | CMD_PSE | CMD_ASE | CMD_RS);
    for (int i = 0; i < 20 && (op_rd(e, OP_USBSTS) & STS_HALTED); i++)
        usb_mdelay(1);
    op_wr(e, OP_CONFIGFLAG, 1);         /* every port is ours unless handed over */
    usb_mdelay(5);

    e->ncompanions = (hcs >> 12) & 0xF;
    hc->root.nports = (int)MIN(hcs & 0xF, (uint32_t)USB_MAX_PORTS);
    if (hcs & (1u << 4)) {              /* port power control */
        for (int p = 1; p <= hc->root.nports; p++)
            op_wr(e, port_reg(p), (op_rd(e, port_reg(p)) & ~PORT_RWC) | PORT_PP);
        usb_mdelay(20);
    }
}

static void ehci_suspend(struct usb_hc *hc)
{
    struct ehci *e = hc->priv;
    op_wr(e, OP_USBINTR, 0);
    op_wr(e, OP_USBCMD, op_rd(e, OP_USBCMD) & ~CMD_RS);
}

static void ehci_resume(struct usb_hc *hc)
{
    ehci_setup(hc->priv);
}

static void ehci_init(const struct pci_dev *pci)
{
    struct ehci *e = kzalloc(sizeof(*e));
    if (!e)
        return;
    e->cap = pci_map_bar(pci, PCI_BAR0);
    uint32_t page = pmm_alloc();
    e->async_head = qh_alloc();
    e->intr_head = qh_alloc();
    if (!e->cap || !page || !e->async_head || !e->intr_head) {
        klog("ehci: cannot set up the controller\n");
        return;
    }
    e->op = e->cap + *(volatile uint8_t *)(e->cap + CAP_LENGTH);
    e->pci = *pci;
    e->frames = (uint32_t *)page;
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);

    struct usb_hc *hc = &e->hc;
    hc->name = "EHCI";
    hc->ops = &ehci_ops;
    hc->priv = e;
    hc->irq = cmdline_has("usb.poll") ? -1 : pci_irq(pci);
    hc->root.port_status = ehci_port_status;
    hc->root.port_reset = ehci_port_reset;
    hc->root.port_disable = ehci_port_disable;
    hc->root.priv = e;
    ehci_setup(e);

    ehcis[nehci++] = e;
    if (hc->irq >= 0)
        irq_register(hc->irq, ehci_irq);
    usb_register_hc(hc);
    boot_ok("USB: EHCI controller, IRQ %d, %d ports, %d companion controllers", hc->irq,
            hc->root.nports, e->ncompanions);
}

void ehci_probe(void)
{
    struct pci_dev pci;
    uint8_t prog_if;
    for (int i = 0; nehci < MAX_EHCI && pci_find_class_nth(0x0C03, i, &pci, &prog_if) == 0; i++)
        if (prog_if == 0x20)
            ehci_init(&pci);
}
