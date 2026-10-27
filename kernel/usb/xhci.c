/*
 * xHCI host controller driver (USB 3, and every speed below it: an xHCI
 * controller has no companions).
 *
 * The controller is driven through rings of 16-byte TRBs: commands go on
 * the command ring, transfers on a ring per endpoint, and the controller
 * reports completions on the event ring. Each device gets a slot with a
 * device context the controller keeps, set up by commands that take an
 * input context from us. The controller addresses devices itself
 * (Address Device), so the core's dev_init/dev_configure hooks do that
 * work here.
 *
 * The register window must lie below 4 GiB (this kernel has no PAE).
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "usb.h"

/* Capability registers */
#define CAP_LENGTH    0x00
#define CAP_HCIVERSION 0x02
#define CAP_HCSPARAMS1 0x04
#define CAP_HCSPARAMS2 0x08
#define CAP_HCCPARAMS1 0x10
#define CAP_DBOFF     0x14
#define CAP_RTSOFF    0x18

/* Operational registers */
#define OP_USBCMD  0x00
#define OP_USBSTS  0x04
#define OP_CRCR    0x18
#define OP_DCBAAP  0x30
#define OP_CONFIG  0x38
#define OP_PORTSC  0x400

#define CMD_RS    (1u << 0)
#define CMD_HCRST (1u << 1)
#define CMD_INTE  (1u << 2)
#define CMD_HSEE  (1u << 3)
#define STS_HCH   (1u << 0)
#define STS_HSE   (1u << 2)
#define STS_EINT  (1u << 3)
#define STS_PCD   (1u << 4)
#define STS_CNR   (1u << 11)

#define PORT_CCS  (1u << 0)
#define PORT_PED  (1u << 1)
#define PORT_PR   (1u << 4)
#define PORT_PP   (1u << 9)
#define PORT_CSC  (1u << 17)
#define PORT_PEC  (1u << 18)
#define PORT_WRC  (1u << 19)
#define PORT_PRC  (1u << 21)
#define PORT_WPR  (1u << 31)
#define PORT_CHANGES 0x00FE0000u        /* CSC, PEC, WRC, OCC, PRC, PLC, CEC */
#define PORT_KEEP    0x0E00C200u        /* PP, PIC and the wake bits: written back as read */

/* Interrupter 0, in the runtime registers */
#define IR_IMAN   0x20
#define IR_IMOD   0x24
#define IR_ERSTSZ 0x28
#define IR_ERSTBA 0x30
#define IR_ERDP   0x38
#define IMAN_IP   (1u << 0)
#define IMAN_IE   (1u << 1)
#define ERDP_EHB  (1u << 3)

/* TRB types */
#define TRB_NORMAL      1
#define TRB_SETUP       2
#define TRB_DATA        3
#define TRB_STATUS      4
#define TRB_LINK        6
#define TRB_ENABLE_SLOT 9
#define TRB_DISABLE_SLOT 10
#define TRB_ADDRESS_DEV 11
#define TRB_CONFIG_EP   12
#define TRB_EVAL_CTX    13
#define TRB_RESET_EP    14
#define TRB_STOP_EP     15
#define TRB_SET_DEQ     16
#define TRB_EV_TRANSFER 32
#define TRB_EV_COMMAND  33
#define TRB_EV_PORT     34

#define TRB_CYCLE (1u << 0)
#define TRB_TC    (1u << 1)             /* link: toggle cycle */
#define TRB_ISP   (1u << 2)
#define TRB_CH    (1u << 4)
#define TRB_IOC   (1u << 5)
#define TRB_IDT   (1u << 6)
#define TRB_DIR_IN (1u << 16)
#define TRB_TYPE(t) ((uint32_t)(t) << 10)

#define CC_SUCCESS 1
#define CC_BABBLE  3
#define CC_XACT    4
#define CC_STALL   6
#define CC_SHORT   13
#define CC_SPLIT   36

#define RING_TRBS 256                   /* one page; the last is the link back */
#define MAX_XTRBS 8                     /* TRBs of one transfer */
#define CMD_TIMEOUT 1000                /* ms */

struct trb {
    volatile uint32_t p0, p1, status, control;
};

struct ring {
    struct trb *trbs;
    uint32_t enq;
    uint32_t cycle;
};

struct xep {
    struct ring ring;
    struct usb_xfer *xfer;
    uint32_t trb[MAX_XTRBS];            /* the active transfer's TRBs */
    uint32_t len[MAX_XTRBS];
    int ntrbs;
    int last;                           /* index of the TRB that interrupts */
    uint32_t actual;                    /* control: data moved, from a short packet event */
    int short_seen;
    int halted;
};

struct xslot {
    int id;
    uint8_t *out;                       /* device context, the controller's */
    uint8_t *in;                        /* input context */
    struct xep eps[32];                 /* by DCI */
};

struct xhci {
    struct usb_hc hc;
    volatile uint8_t *cap, *op, *rt;
    volatile uint32_t *db;
    int csz;                            /* context size: 32 or 64 */
    int maxslots;
    uint16_t version;
    uint64_t *dcbaa;
    struct ring cmd;
    struct trb *events;
    uint32_t ev_deq, ev_cycle;
    struct xslot *slots[256];
    uint8_t port_usb3[256];
    /* the command in flight */
    volatile uint32_t cmd_trb;
    volatile int cmd_done, cmd_cc, cmd_slot;
    volatile int cmd_busy;
    int dead;
};

static inline uint32_t op_rd(struct xhci *x, int reg)
{
    return *(volatile uint32_t *)(x->op + reg);
}

static inline void op_wr(struct xhci *x, int reg, uint32_t v)
{
    *(volatile uint32_t *)(x->op + reg) = v;
}

static inline void op_wr64(struct xhci *x, int reg, uint32_t lo)
{
    *(volatile uint32_t *)(x->op + reg) = lo;
    *(volatile uint32_t *)(x->op + reg + 4) = 0;
}

static inline uint32_t rt_rd(struct xhci *x, int reg)
{
    return *(volatile uint32_t *)(x->rt + reg);
}

static inline void rt_wr(struct xhci *x, int reg, uint32_t v)
{
    *(volatile uint32_t *)(x->rt + reg) = v;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

/* ---- rings ---- */

static int ring_init(struct ring *r)
{
    r->trbs = usb_dma_alloc(RING_TRBS * sizeof(struct trb));
    if (!r->trbs)
        return -ENOMEM;
    struct trb *link = &r->trbs[RING_TRBS - 1];
    link->p0 = (uint32_t)r->trbs;
    link->control = TRB_TYPE(TRB_LINK) | TRB_TC;
    r->enq = 0;
    r->cycle = 1;
    return 0;
}

static void ring_free(struct ring *r)
{
    usb_dma_free(r->trbs, RING_TRBS * sizeof(struct trb));
    r->trbs = NULL;
}

/* Put a TRB on the ring; returns its address. The cycle bit goes last,
 * so the controller never sees half of one. With `hold`, it is written
 * inverted, for the caller to flip once the whole transfer is there. */
static uint32_t ring_put(struct ring *r, uint32_t p0, uint32_t p1, uint32_t status,
                         uint32_t control, int hold)
{
    struct trb *t = &r->trbs[r->enq];
    t->p0 = p0;
    t->p1 = p1;
    t->status = status;
    barrier();
    t->control = control | (hold ? !r->cycle : r->cycle);
    if (++r->enq == RING_TRBS - 1) {
        struct trb *link = &r->trbs[RING_TRBS - 1];
        link->control = TRB_TYPE(TRB_LINK) | TRB_TC | (control & TRB_CH) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
    return (uint32_t)t;
}

static uint32_t ring_deq_ptr(struct ring *r)
{
    return (uint32_t)&r->trbs[r->enq] | r->cycle;
}

/* ---- contexts ---- */

static uint32_t *slot_ctx(struct xhci *x, uint8_t *ctx)
{
    return (uint32_t *)ctx;
}

static uint32_t *ep_ctx(struct xhci *x, uint8_t *ctx, int dci)
{
    return (uint32_t *)(ctx + dci * x->csz);
}

/* The input context: the control context, then a copy of the layout above. */
static uint32_t *in_ctrl(struct xhci *x, struct xslot *s)
{
    (void)x;
    return (uint32_t *)s->in;
}

static uint8_t *in_dev(struct xhci *x, struct xslot *s)
{
    return s->in + x->csz;
}

static int dci_of(uint8_t addr)
{
    int n = addr & 0xF;
    return n ? n * 2 + ((addr & USB_DIR_IN) ? 1 : 0) : 1;
}

static uint32_t speed_code(int speed)
{
    switch (speed) {
    case USB_SPEED_LOW:  return 2;
    case USB_SPEED_FULL: return 1;
    case USB_SPEED_HIGH: return 3;
    default:             return 4;
    }
}

/* ---- events ---- */

static struct xep *ep_of(struct xhci *x, int slot, int dci)
{
    if (slot <= 0 || slot > x->maxslots || !x->slots[slot] || dci < 1 || dci > 31)
        return NULL;
    return &x->slots[slot]->eps[dci];
}

static void transfer_event(struct xhci *x, struct trb *ev, struct usb_xfer **done)
{
    int slot = ev->control >> 24, dci = (ev->control >> 16) & 0x1F;
    int cc = ev->status >> 24;
    uint32_t residual = ev->status & 0xFFFFFF;
    struct xep *ep = ep_of(x, slot, dci);
    if (!ep || !ep->xfer)
        return;
    int i;
    for (i = 0; i < ep->ntrbs && ep->trb[i] != ev->p0; i++)
        ;
    if (i == ep->ntrbs)
        return;                         /* an old transfer's (some controllers send two) */
    struct usb_xfer *xf = ep->xfer;
    int control = xf->ep->type == USB_EP_CONTROL;

    uint32_t before = 0;
    for (int k = 0; k < i; k++)
        before += ep->len[k];
    int st;
    if (cc == CC_SUCCESS || cc == CC_SHORT) {
        if (control && i != ep->last) {
            /* A short data stage; the status stage still has to finish. */
            ep->actual = before + ep->len[i] - MIN(residual, ep->len[i]);
            ep->short_seen = 1;
            return;
        }
        if (control) {
            xf->actual = ep->short_seen ? ep->actual : xf->len;
        } else {
            xf->actual = before + ep->len[i] - MIN(residual, ep->len[i]);
        }
        st = 0;
    } else {
        if (cc == CC_STALL || cc == CC_BABBLE || cc == CC_XACT || cc == CC_SPLIT)
            ep->halted = 1;
        xf->actual = before;
        st = cc == CC_STALL ? -EPIPE : -EIO;
    }
    ep->xfer = NULL;
    xf->status = st;
    xf->hcnext = *done;
    *done = xf;
}

static void xhci_poll(struct usb_hc *hc)
{
    struct xhci *x = hc->priv;
    if (!x->events)
        return;
    uint32_t sts = op_rd(x, OP_USBSTS);
    if (sts & (STS_EINT | STS_PCD | STS_HSE))
        op_wr(x, OP_USBSTS, sts & (STS_EINT | STS_PCD | STS_HSE));
    if ((sts & STS_HSE) && !x->dead) {
        x->dead = 1;
        klog("xhci: host system error, controller stopped\n");
    }

    struct usb_xfer *done = NULL;
    int any = 0;
    for (;;) {
        struct trb *ev = &x->events[x->ev_deq];
        if ((ev->control & TRB_CYCLE) != x->ev_cycle)
            break;
        barrier();
        switch ((ev->control >> 10) & 0x3F) {
        case TRB_EV_TRANSFER:
            transfer_event(x, ev, &done);
            break;
        case TRB_EV_COMMAND:
            if (ev->p0 == x->cmd_trb && !x->cmd_done) {
                x->cmd_cc = ev->status >> 24;
                x->cmd_slot = ev->control >> 24;
                x->cmd_done = 1;
                wakeup((void *)&x->cmd_done);
            }
            break;
        case TRB_EV_PORT:
            usb_kick();
            break;
        }
        any = 1;
        if (++x->ev_deq == RING_TRBS) {
            x->ev_deq = 0;
            x->ev_cycle ^= 1;
        }
    }
    if (any) {
        *(volatile uint32_t *)(x->rt + IR_ERDP) = (uint32_t)&x->events[x->ev_deq] | ERDP_EHB;
        *(volatile uint32_t *)(x->rt + IR_ERDP + 4) = 0;
    }
    while (done) {
        struct usb_xfer *xf = done;
        done = xf->hcnext;
        usb_xfer_done(xf, xf->status, xf->actual);
    }
}

/* ---- commands ---- */

/* Run a command and wait for its completion. Returns the completion code. */
static int command(struct xhci *x, uint32_t p0, uint32_t status, uint32_t control, int *slot)
{
    if (x->dead)
        return -1;
    while (x->cmd_busy && usb_in_process())
        sleep_on((void *)&x->cmd_busy);
    x->cmd_busy = 1;
    x->cmd_done = 0;
    x->cmd_trb = ring_put(&x->cmd, p0, 0, status, control, 0);
    barrier();
    x->db[0] = 0;

    uint32_t deadline = ticks + CMD_TIMEOUT / (1000 / HZ) + 1;
    for (uint32_t us = 0; !x->cmd_done; us += 20) {
        xhci_poll(&x->hc);
        if (x->cmd_done)
            break;
        if (usb_in_process()) {
            if ((int32_t)(ticks - deadline) >= 0)
                break;
            sleep_on_timeout((void *)&x->cmd_done, ticks + 1);
        } else {
            if (us >= CMD_TIMEOUT * 1000)
                break;
            udelay(20);
        }
    }
    int cc = x->cmd_done ? x->cmd_cc : -1;
    if (slot)
        *slot = x->cmd_slot;
    x->cmd_trb = 0;
    x->cmd_busy = 0;
    wakeup((void *)&x->cmd_busy);
    if (cc != CC_SUCCESS)
        klog("xhci: command %u failed (%d)\n", (control >> 10) & 0x3F, cc);
    return cc;
}

#define EP_RUNNING 1
#define EP_HALTED  2

/*
 * Bring an endpoint to a stop, whether it is running or halted (by what
 * its context says, which may be news), and move its ring past whatever
 * was queued, so it starts afresh with the next transfer.
 */
static void ep_restart(struct xhci *x, struct xslot *s, int dci)
{
    struct xep *ep = &s->eps[dci];
    uint32_t target = (uint32_t)s->id << 24 | (uint32_t)dci << 16;
    int state = ep_ctx(x, s->out, dci)[0] & 7;
    if (state == EP_RUNNING && command(x, 0, 0, TRB_TYPE(TRB_STOP_EP) | target, NULL) != CC_SUCCESS)
        state = ep_ctx(x, s->out, dci)[0] & 7;          /* it halted meanwhile */
    if (state == EP_HALTED)
        command(x, 0, 0, TRB_TYPE(TRB_RESET_EP) | target, NULL);
    command(x, ring_deq_ptr(&ep->ring), 0, TRB_TYPE(TRB_SET_DEQ) | target, NULL);
    ep->halted = 0;
}

/* ---- transfers ---- */

static void note_trb(struct xep *ep, uint32_t addr, uint32_t len)
{
    if (ep->ntrbs < MAX_XTRBS) {
        ep->trb[ep->ntrbs] = addr;
        ep->len[ep->ntrbs] = len;
        ep->ntrbs++;
    }
}

/* Data TRBs for a buffer: none may cross a 64 KiB boundary. last_flags
 * go on the final one. */
static int put_data(struct xep *ep, uint8_t *buf, uint32_t len, uint32_t mps, uint32_t first_type,
                    uint32_t dir, uint32_t last_flags, int *hold_first, uint32_t *first_trb)
{
    uint32_t a = (uint32_t)buf, left = len, done = 0;
    uint32_t type = first_type;
    do {
        uint32_t n = MIN(left, 0x10000u - (a & 0xFFFF));
        left -= n;
        done += n;
        uint32_t packets = (len - done + mps - 1) / mps;
        uint32_t td_size = left ? MIN(packets, 31u) : 0;
        uint32_t ctl = TRB_TYPE(type) | TRB_ISP | dir | (left ? TRB_CH : last_flags);
        if (ep->ntrbs >= MAX_XTRBS - 1)
            return -EINVAL;
        uint32_t t = ring_put(&ep->ring, n ? a : 0, 0, n | td_size << 17, ctl, *hold_first);
        if (*hold_first) {
            *first_trb = t;
            *hold_first = 0;
        }
        note_trb(ep, t, n);
        a += n;
        type = TRB_NORMAL;
        dir = 0;
    } while (left);
    return 0;
}

static int xhci_submit(struct usb_hc *hc, struct usb_xfer *xf)
{
    struct xhci *x = hc->priv;
    struct xslot *s = xf->dev->hcpriv;
    if (!s || x->dead)
        return -ENODEV;
    int dci = dci_of(xf->ep->addr);
    struct xep *ep = &s->eps[dci];
    if (!ep->ring.trbs || xf->ep->type == USB_EP_ISO)
        return -EINVAL;
    if (ep->xfer)
        return -EBUSY;
    if (ep->halted) {
        if (!usb_can_wait())
            return -EIO;                /* commands take a caller who can wait */
        ep_restart(x, s, dci);
    }

    ep->ntrbs = 0;
    ep->short_seen = 0;
    ep->actual = 0;
    int hold = 1;
    uint32_t first = 0;
    uint32_t mps = xf->ep->maxpacket ? xf->ep->maxpacket : 8;
    if (xf->ep->type == USB_EP_CONTROL) {
        int in = (xf->setup[0] & USB_RT_IN) != 0;
        uint32_t trt = !xf->len ? 0 : in ? 3 : 2;
        uint32_t p0 = xf->setup[0] | xf->setup[1] << 8 | xf->setup[2] << 16 |
                      (uint32_t)xf->setup[3] << 24;
        uint32_t p1 = xf->setup[4] | xf->setup[5] << 8 | xf->setup[6] << 16 |
                      (uint32_t)xf->setup[7] << 24;
        first = ring_put(&ep->ring, p0, p1, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | trt << 16, 1);
        hold = 0;
        note_trb(ep, first, 0);
        if (xf->len && put_data(ep, xf->buf, xf->len, mps, TRB_DATA, in ? TRB_DIR_IN : 0, 0,
                                &hold, &first) < 0)
            return -EINVAL;
        uint32_t t = ring_put(&ep->ring, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC |
                              ((in && xf->len) ? 0 : TRB_DIR_IN), 0);
        note_trb(ep, t, 0);
    } else if (put_data(ep, xf->buf, xf->len, mps, TRB_NORMAL, 0, TRB_IOC, &hold, &first) < 0) {
        return -EINVAL;
    }
    ep->last = ep->ntrbs - 1;
    ep->xfer = xf;
    xf->hcpriv = ep;
    barrier();
    ((struct trb *)first)->control ^= TRB_CYCLE;       /* now the controller may start */
    barrier();
    x->db[s->id] = (uint32_t)dci;
    return 0;
}

static void xhci_cancel(struct usb_hc *hc, struct usb_xfer *xf)
{
    struct xhci *x = hc->priv;
    struct xslot *s = xf->dev->hcpriv;
    if (!s)
        return;
    int dci = dci_of(xf->ep->addr);
    struct xep *ep = &s->eps[dci];
    if (ep->xfer != xf)
        return;
    ep->xfer = NULL;                    /* its events are ignored from now on */
    ep_restart(x, s, dci);
}

/* ---- devices ---- */

static void slot_free(struct xhci *x, struct xslot *s)
{
    for (int i = 1; i < 32; i++) {
        if (s->eps[i].xfer) {
            s->eps[i].xfer->status = -ENODEV;
            wakeup(s->eps[i].xfer);
        }
        if (s->eps[i].ring.trbs)
            ring_free(&s->eps[i].ring);
    }
    usb_dma_free(s->out, 2048);
    usb_dma_free(s->in, 4096);
    kfree(s);
}

/* Fill in the input context's endpoint context for an endpoint. */
static void setup_ep_ctx(struct xhci *x, struct xslot *s, struct usb_dev *dev, struct usb_ep *ep)
{
    int dci = dci_of(ep->addr);
    uint32_t *c = ep_ctx(x, in_dev(x, s), dci);
    memset(c, 0, (uint32_t)x->csz);
    int in = (ep->addr & USB_DIR_IN) != 0;
    uint32_t type = ep->type == USB_EP_CONTROL ? 4 :
                    ep->type == USB_EP_BULK ? (in ? 6 : 2) :
                    ep->type == USB_EP_INTR ? (in ? 7 : 3) : (in ? 5 : 1);
    uint32_t mps = ep->maxpacket ? ep->maxpacket : 8;
    uint32_t burst = dev->speed == USB_SPEED_SUPER ? ep->burst :
                     dev->speed == USB_SPEED_HIGH && ep->type == USB_EP_INTR ? ep->mult : 0;
    uint32_t interval = 0;
    if (ep->type == USB_EP_INTR) {
        if (dev->speed >= USB_SPEED_HIGH) {
            interval = ep->interval ? ep->interval - 1u : 0;
        } else {
            /* Milliseconds to 2^n microframes. */
            uint32_t uf = (ep->interval ? ep->interval : 1) * 8u;
            while (interval < 15 && (2u << interval) <= uf)
                interval++;
            interval = MAX(interval, 3u);
        }
        interval = MIN(interval, 15u);
    }
    c[0] = interval << 16;
    c[1] = 3u << 1 | type << 3 | burst << 8 | mps << 16;
    c[2] = ring_deq_ptr(&s->eps[dci].ring);
    c[3] = 0;
    uint32_t esit = ep->type == USB_EP_INTR ? mps * (burst + 1) : 0;
    c[4] = (ep->type == USB_EP_CONTROL ? 8u : ep->type == USB_EP_INTR ? MIN(mps, 1024u) : 3072u) |
           (esit & 0xFFFF) << 16;
}

static int xhci_dev_init(struct usb_hc *hc, struct usb_dev *dev)
{
    struct xhci *x = hc->priv;
    int id = 0;
    if (command(x, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &id) != CC_SUCCESS || id <= 0 ||
        id > x->maxslots)
        return -EIO;
    struct xslot *s = kzalloc(sizeof(*s));
    if (!s) {
        command(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)id << 24, NULL);
        return -ENOMEM;
    }
    s->id = id;
    s->out = usb_dma_alloc(2048);
    s->in = usb_dma_alloc(4096);
    if (!s->out || !s->in || ring_init(&s->eps[1].ring) < 0) {
        command(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)id << 24, NULL);
        slot_free(x, s);
        return -ENOMEM;
    }
    x->slots[id] = s;
    x->dcbaa[id] = (uint32_t)s->out;
    dev->hcpriv = s;

    uint32_t *sc = slot_ctx(x, in_dev(x, s));
    sc[0] = dev->route | speed_code(dev->speed) << 20 | 1u << 27;
    sc[1] = (uint32_t)dev->root_port << 16;
    if (dev->speed < USB_SPEED_HIGH && dev->tt_hub && dev->tt_hub->hcpriv)
        sc[2] = (uint32_t)((struct xslot *)dev->tt_hub->hcpriv)->id | (uint32_t)dev->tt_port << 8;
    setup_ep_ctx(x, s, dev, &dev->ep0);

    in_ctrl(x, s)[0] = 0;
    in_ctrl(x, s)[1] = 0x3;             /* slot and ep0 */
    if (command(x, (uint32_t)s->in, 0, TRB_TYPE(TRB_ADDRESS_DEV) | (uint32_t)id << 24,
                NULL) != CC_SUCCESS)
        return -EIO;                    /* the core frees the slot */
    dev->addr = slot_ctx(x, s->out)[3] & 0xFF;
    return 0;
}

/* ep0's packet size turned out different from the guess. */
static int xhci_dev_update(struct usb_hc *hc, struct usb_dev *dev)
{
    struct xhci *x = hc->priv;
    struct xslot *s = dev->hcpriv;
    uint32_t *c = ep_ctx(x, in_dev(x, s), 1);
    c[1] = (c[1] & 0xFFFF) | (uint32_t)dev->ep0.maxpacket << 16;
    in_ctrl(x, s)[0] = 0;
    in_ctrl(x, s)[1] = 0x2;
    return command(x, (uint32_t)s->in, 0, TRB_TYPE(TRB_EVAL_CTX) | (uint32_t)s->id << 24,
                   NULL) == CC_SUCCESS ? 0 : -EIO;
}

static int xhci_dev_configure(struct usb_hc *hc, struct usb_dev *dev)
{
    struct xhci *x = hc->priv;
    struct xslot *s = dev->hcpriv;
    uint32_t add = 1;                   /* the slot context */
    int maxdci = 1;
    for (int i = 0; i < 32; i++) {
        struct usb_ep *ep = &dev->eps[i];
        if (!ep->maxpacket || ep->type == USB_EP_ISO)
            continue;
        int dci = dci_of(ep->addr);
        if (!s->eps[dci].ring.trbs && ring_init(&s->eps[dci].ring) < 0)
            return -ENOMEM;
        setup_ep_ctx(x, s, dev, ep);
        add |= 1u << dci;
        maxdci = MAX(maxdci, dci);
    }
    if (add == 1)
        return 0;                       /* nothing but ep0 */
    uint32_t *sc = slot_ctx(x, in_dev(x, s));
    sc[0] = (sc[0] & ~(0x1Fu << 27)) | (uint32_t)maxdci << 27;
    in_ctrl(x, s)[0] = 0;
    in_ctrl(x, s)[1] = add;
    return command(x, (uint32_t)s->in, 0, TRB_TYPE(TRB_CONFIG_EP) | (uint32_t)s->id << 24,
                   NULL) == CC_SUCCESS ? 0 : -EIO;
}

static int xhci_dev_hub(struct usb_hc *hc, struct usb_dev *dev, int nports, int ttt, int mtt)
{
    struct xhci *x = hc->priv;
    struct xslot *s = dev->hcpriv;
    uint32_t *sc = slot_ctx(x, in_dev(x, s));
    sc[0] |= 1u << 26 | (mtt ? 1u << 25 : 0);
    sc[1] = (sc[1] & 0x00FFFFFFu) | (uint32_t)nports << 24;
    if (dev->speed == USB_SPEED_HIGH)
        sc[2] = (sc[2] & ~(3u << 16)) | (uint32_t)ttt << 16;
    in_ctrl(x, s)[0] = 0;
    in_ctrl(x, s)[1] = 1;
    /* As Linux does: 0.95 controllers take hub fields by Evaluate Context. */
    uint32_t type = x->version > 0x95 ? TRB_CONFIG_EP : TRB_EVAL_CTX;
    return command(x, (uint32_t)s->in, 0, TRB_TYPE(type) | (uint32_t)s->id << 24,
                   NULL) == CC_SUCCESS ? 0 : -EIO;
}

static void xhci_dev_free(struct usb_hc *hc, struct usb_dev *dev)
{
    struct xhci *x = hc->priv;
    struct xslot *s = dev->hcpriv;
    if (!s)
        return;
    command(x, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | (uint32_t)s->id << 24, NULL);
    x->dcbaa[s->id] = 0;
    x->slots[s->id] = NULL;
    dev->hcpriv = NULL;
    slot_free(x, s);
}

/* After CLEAR_FEATURE(HALT): reset the endpoint (its toggle) and its ring. */
static void xhci_ep_reset(struct usb_hc *hc, struct usb_dev *dev, struct usb_ep *ep)
{
    struct xhci *x = hc->priv;
    struct xslot *s = dev->hcpriv;
    int dci = dci_of(ep->addr);
    if (!s || !s->eps[dci].ring.trbs || s->eps[dci].xfer)
        return;
    if (s->eps[dci].halted || (ep_ctx(x, s->out, dci)[0] & 7) == EP_HALTED)
        ep_restart(x, s, dci);
}

/* ---- root hub ---- */

static volatile uint32_t *portsc(struct xhci *x, int port)
{
    return (volatile uint32_t *)(x->op + OP_PORTSC + 0x10 * (port - 1));
}

static int xhci_port_status(struct usb_hub *h, int port)
{
    struct xhci *x = h->hc->priv;
    volatile uint32_t *r = portsc(x, port);
    uint32_t v = *r;
    if (v & PORT_CHANGES)
        *r = (v & PORT_KEEP) | (v & PORT_CHANGES);
    return ((v & PORT_CCS) ? PORT_CONNECT : 0) | ((v & PORT_PED) ? PORT_ENABLE : 0) |
           ((v & PORT_CSC) ? PORT_CHANGE : 0);
}

static int port_speed(uint32_t v)
{
    switch ((v >> 10) & 0xF) {
    case 1:  return USB_SPEED_FULL;
    case 2:  return USB_SPEED_LOW;
    case 3:  return USB_SPEED_HIGH;
    default: return USB_SPEED_SUPER;
    }
}

static int xhci_port_reset(struct usb_hub *h, int port)
{
    struct xhci *x = h->hc->priv;
    volatile uint32_t *r = portsc(x, port);
    if (!(*r & PORT_CCS))
        return -ENODEV;
    if (x->port_usb3[port]) {
        /* USB 3 links train by themselves; a warm reset if one is stuck. */
        for (int i = 0; i < 20 && !(*r & PORT_PED); i++)
            mdelay(5);
        if (!(*r & PORT_PED)) {
            *r = (*r & PORT_KEEP) | PORT_WPR;
            for (int i = 0; i < 50 && !(*r & (PORT_WRC | PORT_PRC)); i++)
                mdelay(10);
        }
    } else {
        *r = (*r & PORT_KEEP) | PORT_PR;
        for (int i = 0; i < 50 && !(*r & PORT_PRC); i++)
            mdelay(10);
    }
    uint32_t v = *r;
    *r = (v & PORT_KEEP) | (v & (PORT_PRC | PORT_WRC | PORT_PEC));
    if (!(v & PORT_CCS))
        return -ENODEV;
    if (!(v & PORT_PED))
        return -EIO;
    return port_speed(v);
}

static void xhci_port_disable(struct usb_hub *h, int port)
{
    struct xhci *x = h->hc->priv;
    volatile uint32_t *r = portsc(x, port);
    if (!x->port_usb3[port])
        *r = (*r & PORT_KEEP) | PORT_PED;   /* PED is write-1-to-disable */
}

static const struct usb_hc_ops xhci_ops = {
    .submit = xhci_submit,
    .cancel = xhci_cancel,
    .poll = xhci_poll,
    .dev_init = xhci_dev_init,
    .dev_update = xhci_dev_update,
    .dev_configure = xhci_dev_configure,
    .dev_hub = xhci_dev_hub,
    .dev_free = xhci_dev_free,
    .ep_reset = xhci_ep_reset,
};

#define MAX_XHCI 4
static struct xhci *xhcis[MAX_XHCI];
static int nxhci;

static void xhci_irq(struct trapframe *tf)
{
    (void)tf;
    for (int i = 0; i < nxhci; i++) {
        struct xhci *x = xhcis[i];
        if (!(op_rd(x, OP_USBSTS) & STS_EINT) && !(rt_rd(x, IR_IMAN) & IMAN_IP))
            continue;
        op_wr(x, OP_USBSTS, STS_EINT);
        rt_wr(x, IR_IMAN, IMAN_IE | IMAN_IP);
        xhci_poll(&x->hc);
    }
}

/* Walk the extended capabilities: take over from the BIOS, learn which
 * ports are USB 3. */
static void ext_caps(struct xhci *x, uint32_t hcc1)
{
    uint32_t off = (hcc1 >> 16) << 2;
    for (int guard = 0; off && guard < 64; guard++) {
        volatile uint32_t *c = (volatile uint32_t *)(x->cap + off);
        uint32_t v = c[0];
        int id = v & 0xFF;
        if (id == 1) {                  /* USB legacy support */
            if (v & (1u << 16)) {
                c[0] = v | (1u << 24);
                for (int i = 0; i < 100 && (c[0] & (1u << 16)); i++)
                    mdelay(10);
                if (c[0] & (1u << 16)) {
                    klog("xhci: BIOS would not let go; taking the controller anyway\n");
                    c[0] = (c[0] & ~(1u << 16)) | (1u << 24);
                }
            }
            c[1] = (c[1] & ~0x0000E011u) | 0xE0000000u;     /* SMIs off, pending ones cleared */
        } else if (id == 2) {           /* supported protocol */
            int major = v >> 24;
            uint32_t ports = c[2];
            int first = ports & 0xFF, count = (ports >> 8) & 0xFF;
            for (int p = first; p < first + count && p < 256; p++)
                x->port_usb3[p] = major >= 3;
        }
        uint32_t next = (v >> 8) & 0xFF;
        off = next ? off + next * 4 : 0;
    }
}

/* Intel 7 to 9 series: route the shared ports from the EHCI to us. */
static void intel_switch_ports(const struct pci_dev *pci)
{
    static const uint16_t ids[] = { 0x1E31, 0x8C31, 0x9C31, 0x8CB1, 0x9CB1 };
    uint32_t id = pci_read32(pci, 0);
    if ((id & 0xFFFF) != 0x8086)
        return;
    for (uint32_t i = 0; i < ARRAY_SIZE(ids); i++) {
        if ((id >> 16) == ids[i]) {
            pci_write32(pci, 0xD8, pci_read32(pci, 0xDC));     /* USB3_PSSEN from USB3PRM */
            pci_write32(pci, 0xD0, pci_read32(pci, 0xD4));     /* XUSB2PR from XUSB2PRM */
            return;
        }
    }
}

static void xhci_init(const struct pci_dev *pci)
{
    struct xhci *x = kzalloc(sizeof(*x));
    if (!x)
        return;
    x->cap = pci_map_bar(pci, PCI_BAR0);
    if (!x->cap) {
        klog("xhci: registers above 4 GiB or missing; controller not used\n");
        kfree(x);
        return;
    }
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    x->op = x->cap + *(volatile uint8_t *)(x->cap + CAP_LENGTH);
    x->rt = x->cap + (*(volatile uint32_t *)(x->cap + CAP_RTSOFF) & ~0x1Fu);
    x->db = (volatile uint32_t *)(x->cap + (*(volatile uint32_t *)(x->cap + CAP_DBOFF) & ~0x3u));
    x->version = *(volatile uint16_t *)(x->cap + CAP_HCIVERSION);
    uint32_t hcs1 = *(volatile uint32_t *)(x->cap + CAP_HCSPARAMS1);
    uint32_t hcs2 = *(volatile uint32_t *)(x->cap + CAP_HCSPARAMS2);
    uint32_t hcc1 = *(volatile uint32_t *)(x->cap + CAP_HCCPARAMS1);
    x->csz = (hcc1 & (1u << 2)) ? 64 : 32;
    x->maxslots = (int)MIN(hcs1 & 0xFF, 32u);
    int nports = (int)(hcs1 >> 24);

    intel_switch_ports(pci);
    ext_caps(x, hcc1);

    /* Stop and reset it (the BIOS may have left it running). */
    op_wr(x, OP_USBCMD, op_rd(x, OP_USBCMD) & ~(CMD_RS | CMD_INTE));
    for (int i = 0; i < 20 && !(op_rd(x, OP_USBSTS) & STS_HCH); i++)
        mdelay(1);
    op_wr(x, OP_USBCMD, CMD_HCRST);
    for (int i = 0; i < 1000 && (op_rd(x, OP_USBCMD) & CMD_HCRST); i++)
        mdelay(1);
    for (int i = 0; i < 1000 && (op_rd(x, OP_USBSTS) & STS_CNR); i++)
        mdelay(1);
    if (op_rd(x, OP_USBSTS) & STS_CNR) {
        klog("xhci: controller did not come out of reset\n");
        return;
    }

    x->dcbaa = usb_dma_alloc((uint32_t)(x->maxslots + 1) * 8);
    x->events = usb_dma_alloc(RING_TRBS * sizeof(struct trb));
    uint32_t *erst = usb_dma_alloc(64);
    if (!x->dcbaa || !x->events || !erst || ring_init(&x->cmd) < 0) {
        klog("xhci: out of memory\n");
        x->events = NULL;
        return;
    }
    int nscratch = (int)(((hcs2 >> 27) & 0x1F) | ((hcs2 >> 21) & 0x1F) << 5);
    if (nscratch) {
        uint32_t frames = ((uint32_t)nscratch * 8 + PAGE_SIZE - 1) / PAGE_SIZE;
        uint64_t *arr = (uint64_t *)pmm_alloc_contig(frames);
        if (!arr) {
            klog("xhci: out of memory\n");
            x->events = NULL;
            return;
        }
        memset(arr, 0, frames * PAGE_SIZE);
        for (int i = 0; i < nscratch; i++) {
            uint32_t pg = pmm_alloc();
            if (pg)
                memset((void *)pg, 0, PAGE_SIZE);
            arr[i] = pg;
        }
        x->dcbaa[0] = (uint32_t)arr;
    }

    op_wr(x, OP_CONFIG, (op_rd(x, OP_CONFIG) & ~0xFFu) | (uint32_t)x->maxslots);
    op_wr64(x, OP_DCBAAP, (uint32_t)x->dcbaa);
    op_wr64(x, OP_CRCR, (uint32_t)x->cmd.trbs | 1);
    erst[0] = (uint32_t)x->events;
    erst[1] = 0;
    erst[2] = RING_TRBS;
    x->ev_deq = 0;
    x->ev_cycle = 1;
    rt_wr(x, IR_ERSTSZ, 1);
    rt_wr(x, IR_ERDP, (uint32_t)x->events);
    rt_wr(x, IR_ERDP + 4, 0);
    rt_wr(x, IR_ERSTBA, (uint32_t)erst);
    rt_wr(x, IR_ERSTBA + 4, 0);
    rt_wr(x, IR_IMOD, 0);

    struct usb_hc *hc = &x->hc;
    hc->name = "xHCI";
    hc->ops = &xhci_ops;
    hc->priv = x;
    hc->sets_address = 1;
    hc->irq = cmdline_has("usb.poll") ? -1 : pci_irq(pci);
    rt_wr(x, IR_IMAN, IMAN_IP | (hc->irq >= 0 ? IMAN_IE : 0));
    op_wr(x, OP_USBSTS, STS_EINT | STS_PCD | STS_HSE);
    op_wr(x, OP_USBCMD, CMD_RS | CMD_HSEE | (hc->irq >= 0 ? CMD_INTE : 0));
    for (int i = 0; i < 20 && (op_rd(x, OP_USBSTS) & STS_HCH); i++)
        mdelay(1);

    hc->root.nports = MIN(nports, USB_MAX_PORTS);
    hc->root.port_status = xhci_port_status;
    hc->root.port_reset = xhci_port_reset;
    hc->root.port_disable = xhci_port_disable;
    hc->root.priv = x;
    if (hcc1 & (1u << 3)) {             /* port power control */
        for (int p = 1; p <= hc->root.nports; p++)
            *portsc(x, p) = (*portsc(x, p) & PORT_KEEP) | PORT_PP;
        mdelay(20);
    }

    xhcis[nxhci++] = x;
    if (hc->irq >= 0)
        irq_register(hc->irq, xhci_irq);
    usb_register_hc(hc);
    boot_ok("USB: xHCI %x.%02x controller, IRQ %d, %d ports, %d slots", x->version >> 8,
            x->version & 0xFF, hc->irq, nports, x->maxslots);
}

void xhci_probe(void)
{
    struct pci_dev pci;
    uint8_t prog_if;
    for (int i = 0; nxhci < MAX_XHCI && pci_find_class_nth(0x0C03, i, &pci, &prog_if) == 0; i++)
        if (prog_if == 0x30)
            xhci_init(&pci);
}
