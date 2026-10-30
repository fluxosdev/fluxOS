/*
 * OHCI host controller driver (USB 1.1 on SiS, ALi, VIA, AMD and
 * NVIDIA chipsets, and companions of EHCI on many of them).
 *
 * The controller walks lists of endpoint descriptors: a control list, a
 * bulk list, and an interrupt tree that hangs off the 32 entries of the
 * HCCA, all of which point at one chain here, so interrupt endpoints are
 * polled every frame. Each endpoint descriptor (ED) has a queue of
 * transfer descriptors ending in an empty one; the controller keeps the
 * data toggles. A transfer is finished when the ED's queue is empty or
 * halted. Descriptors are freed two frames after they are done with.
 *
 * Isochronous endpoints go on the interrupt list too, with isochronous
 * descriptors of up to eight packets each, given the frames they are
 * for; an endpoint's transfers queue up, each one's packets in the
 * frames after the one before's.
 */
#include "kernel.h"
#include "arch.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "usb.h"

#define HC_REVISION   0x00
#define HC_CONTROL    0x04
#define HC_CMDSTATUS  0x08
#define HC_INTSTATUS  0x0C
#define HC_INTENABLE  0x10
#define HC_INTDISABLE 0x14
#define HC_HCCA       0x18
#define HC_CTRLHEAD   0x20
#define HC_CTRLCUR    0x24
#define HC_BULKHEAD   0x28
#define HC_BULKCUR    0x2C
#define HC_FMINTERVAL 0x34
#define HC_FMNUMBER   0x3C
#define HC_PERIODSTART 0x40
#define HC_LSTHRESH   0x44
#define HC_RHDESCA    0x48
#define HC_RHSTATUS   0x50
#define HC_RHPORT     0x54

#define CTL_PLE   (1u << 2)
#define CTL_IE    (1u << 3)
#define CTL_CLE   (1u << 4)
#define CTL_BLE   (1u << 5)
#define CTL_HCFS  (3u << 6)
#define CTL_OPER  (2u << 6)
#define CTL_RESUME (1u << 6)
#define CTL_IR    (1u << 8)
#define CMD_HCR   (1u << 0)
#define CMD_CLF   (1u << 1)
#define CMD_BLF   (1u << 2)
#define CMD_OCR   (1u << 3)
#define INT_WDH   (1u << 1)
#define INT_UE    (1u << 4)
#define INT_MIE   (1u << 31)

#define RH_NPS    (1u << 9)
#define RH_LPSC   (1u << 16)

#define PS_CCS  (1u << 0)
#define PS_PES  (1u << 1)
#define PS_PRS  (1u << 4)
#define PS_LSDA (1u << 9)
#define PS_CSC  (1u << 16)
#define PS_PRSC (1u << 20)
#define PS_CHANGES (0x1Fu << 16)
#define PS_CLEAR_ENABLE (1u << 0)
#define PS_SET_POWER    (1u << 8)

#define ED_DIR_OUT (1u << 11)
#define ED_DIR_IN  (2u << 11)
#define ED_LS   (1u << 13)
#define ED_SKIP (1u << 14)
#define ED_ISO  (1u << 15)
#define ED_HALTED 0x1u
#define ED_CARRY  0x2u

#define TD_R      (1u << 18)            /* a short packet is fine */
#define TD_SETUP  (0u << 19)
#define TD_OUT    (1u << 19)
#define TD_IN     (2u << 19)
#define TD_NOINT  (7u << 21)
#define TD_DATA0  (2u << 24)
#define TD_DATA1  (3u << 24)
#define TD_CC(x)  ((x) >> 28)
#define CC_NOERROR   0
#define CC_STALL     4
#define CC_UNDERRUN  9
#define CC_NOTACCESSED 0xE

#define ISO_QUEUE 8                     /* isochronous transfers in flight on an endpoint */

struct ohci_td {
    volatile uint32_t info, cbp, next, be;
    struct ohci_td *snext;              /* this transfer's next descriptor */
    struct ohci_td *rnext;              /* next retired chain */
    uint32_t start;                     /* the buffer address it started at */
    uint16_t len;
    uint16_t frame;
};

/* An isochronous descriptor: up to eight packets, one a frame, in the two
 * pages of bp0 and be; psw[i] holds packet i's offset, then its result. */
struct ohci_itd {
    volatile uint32_t info, bp0, next, be;
    volatile uint16_t psw[8];
    struct ohci_itd *snext;             /* this transfer's next descriptor */
    struct ohci_itd *rnext;             /* next retired chain */
    uint16_t frame;
    uint16_t npackets;
};

struct ohci_ed {
    volatile uint32_t info, tailp, headp, next;
    struct ohci_ed *sprev, *snext;      /* in its list */
    struct ohci_td *dummy;              /* the empty descriptor at the tail */
    struct usb_xfer *xfer;
    struct ohci_td *tds;
    int type;
    /* isochronous: the empty descriptor at the tail, the transfers queued
     * (oldest first, each one's descriptors chained from its hcpriv) */
    struct ohci_itd *idummy;
    struct usb_xfer *iso[ISO_QUEUE];
    int iso_head, iso_n;
    uint16_t next_frame;
};

struct ohci {
    struct usb_hc hc;
    volatile uint8_t *regs;
    uint32_t *hcca;
    struct ohci_ed *head[3];            /* interrupt, control, bulk: dummy heads */
    struct ohci_td *reclaim;
    struct ohci_itd *ireclaim;
    int dead;
};

static inline uint32_t rd(struct ohci *o, int reg)
{
    return *(volatile uint32_t *)(o->regs + reg);
}

static inline void wr(struct ohci *o, int reg, uint32_t v)
{
    *(volatile uint32_t *)(o->regs + reg) = v;
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static uint16_t frame_now(struct ohci *o)
{
    return (uint16_t)rd(o, HC_FMNUMBER);
}

static void wait_frames(struct ohci *o)
{
    uint16_t f = frame_now(o);
    for (int i = 0; i < 60 && (uint16_t)(frame_now(o) - f) < 2; i++)
        udelay(50);
}

static void free_chain(struct ohci_td *td)
{
    while (td) {
        struct ohci_td *n = td->snext;
        usb_dma_free(td, sizeof(*td));
        td = n;
    }
}

static void retire(struct ohci *o, struct ohci_td *tds)
{
    if (!tds)
        return;
    tds->frame = frame_now(o);
    tds->rnext = o->reclaim;
    o->reclaim = tds;
}

static void free_ichain(struct ohci_itd *td)
{
    while (td) {
        struct ohci_itd *n = td->snext;
        usb_dma_free(td, sizeof(*td));
        td = n;
    }
}

static void iretire(struct ohci *o, struct ohci_itd *tds)
{
    if (!tds)
        return;
    tds->frame = frame_now(o);
    tds->rnext = o->ireclaim;
    o->ireclaim = tds;
}

static void reclaim(struct ohci *o)
{
    uint16_t now = frame_now(o);
    struct ohci_td **pp = &o->reclaim;
    while (*pp) {
        struct ohci_td *td = *pp;
        if ((uint16_t)(now - td->frame) >= 2 || o->dead) {
            *pp = td->rnext;
            free_chain(td);
        } else {
            pp = &td->rnext;
        }
    }
    struct ohci_itd **ip = &o->ireclaim;
    while (*ip) {
        struct ohci_itd *td = *ip;
        if ((uint16_t)(now - td->frame) >= 2 || o->dead) {
            *ip = td->rnext;
            free_ichain(td);
        } else {
            ip = &td->rnext;
        }
    }
}

/* ---- endpoint descriptors ---- */

static int list_of(int type)
{
    return type == USB_EP_INTR || type == USB_EP_ISO ? 0 : type == USB_EP_CONTROL ? 1 : 2;
}

static struct ohci_ed *ed_new(int type)
{
    struct ohci_ed *ed = usb_dma_alloc(sizeof(*ed));
    if (!ed)
        return NULL;
    ed->info = ED_SKIP;
    ed->type = type;
    return ed;
}

static void ed_link(struct ohci *o, struct ohci_ed *ed)
{
    struct ohci_ed *h = o->head[list_of(ed->type)];
    ed->next = h->next;
    ed->snext = h->snext;
    ed->sprev = h;
    if (h->snext)
        h->snext->sprev = ed;
    h->snext = ed;
    barrier();
    h->next = (uint32_t)ed;
}

static void ed_unlink(struct ohci_ed *ed)
{
    ed->sprev->next = ed->next;
    ed->sprev->snext = ed->snext;
    if (ed->snext)
        ed->snext->sprev = ed->sprev;
}

static struct ohci_ed *ep_ed(struct ohci *o, struct usb_dev *dev, struct usb_ep *ep)
{
    if (ep->hcpriv)
        return ep->hcpriv;
    int iso = ep->type == USB_EP_ISO;
    struct ohci_ed *ed = ed_new(ep->type);
    struct ohci_td *dummy = iso ? NULL : usb_dma_alloc(sizeof(struct ohci_td));
    struct ohci_itd *idummy = iso ? usb_dma_alloc(sizeof(struct ohci_itd)) : NULL;
    if (!ed || (!dummy && !idummy)) {
        usb_dma_free(ed, sizeof(*ed));
        usb_dma_free(dummy, sizeof(*dummy));
        usb_dma_free(idummy, sizeof(*idummy));
        return NULL;
    }
    ed->info = (uint32_t)dev->addr | (uint32_t)(ep->addr & 0xF) << 7 |
               (dev->speed == USB_SPEED_LOW ? ED_LS : 0) |
               (uint32_t)(ep->maxpacket ? ep->maxpacket : 8) << 16;
    if (iso)                            /* isochronous descriptors carry no direction */
        ed->info |= ED_ISO | ((ep->addr & USB_DIR_IN) ? ED_DIR_IN : ED_DIR_OUT);
    ed->dummy = dummy;
    ed->idummy = idummy;
    ed->tailp = iso ? (uint32_t)idummy : (uint32_t)dummy;
    ed->headp = ed->tailp;
    ed_link(o, ed);
    ep->hcpriv = ed;
    return ed;
}

/* ---- isochronous transfers ---- */

/* OUT packets lie one after another in the buffer; IN packets maxpacket
 * apart. A descriptor takes up to eight packets, as many as lie within
 * two pages. */
static int iso_submit(struct ohci *o, struct ohci_ed *ed, struct usb_xfer *x)
{
    struct usb_ep *ep = x->ep;
    uint32_t mps = ep->maxpacket ? ep->maxpacket : 8;
    int in = (ep->addr & USB_DIR_IN) != 0;
    if (!x->npackets || x->npackets > USB_ISO_MAX_PACKETS || !x->iso_len || ep->interval > 1)
        return -EINVAL;
    if (ed->iso_n >= ISO_QUEUE)
        return -EBUSY;
    uint32_t addr[USB_ISO_MAX_PACKETS], a = (uint32_t)x->buf;
    for (uint32_t i = 0; i < x->npackets; i++) {
        if (in)
            a = (uint32_t)x->buf + i * mps;
        if (x->iso_len[i] > mps || a + x->iso_len[i] > (uint32_t)x->buf + x->len)
            return -EINVAL;
        addr[i] = a;
        if (!in)
            a += x->iso_len[i];
    }
    uint16_t now = frame_now(o), start = ed->next_frame;
    int16_t ahead = (int16_t)(start - now);
    if (!ed->iso_n || ahead < 2 || ahead > 1000)
        start = (uint16_t)(now + 2);    /* idle, late, or absurd: soon */

    /* The descriptor starting at packet p takes packets up to a page past p's. */
    struct ohci_itd *tds[USB_ISO_MAX_PACKETS + 1];
    int n = 0;
    uint32_t first[USB_ISO_MAX_PACKETS], count[USB_ISO_MAX_PACKETS];
    for (uint32_t p = 0; p < x->npackets; n++) {
        uint32_t page = addr[p] & ~0xFFFu, k = 0;
        while (p + k < x->npackets && k < 8) {
            uint32_t slot = in ? mps : (x->iso_len[p + k] ? x->iso_len[p + k] : 1);
            if (((addr[p + k] + slot - 1) & ~0xFFFu) > page + 0x1000)
                break;
            k++;
        }
        if (!k)
            return -EINVAL;             /* a packet over two page ends */
        first[n] = p;
        count[n] = k;
        p += k;
    }
    tds[0] = ed->idummy;                /* the transfer starts in the empty tail descriptor */
    for (int i = 1; i <= n; i++) {
        if (!(tds[i] = usb_dma_alloc(sizeof(struct ohci_itd)))) {
            for (int k = 1; k < i; k++)
                usb_dma_free(tds[k], sizeof(struct ohci_itd));
            return -ENOMEM;
        }
    }
    for (int i = 0; i < n; i++) {
        struct ohci_itd *td = tds[i];
        uint32_t p = first[i], k = count[i], last = p + k - 1;
        uint32_t page = addr[p] & ~0xFFFu;
        uint32_t end = in ? addr[last] + mps - 1 : addr[last] + (x->iso_len[last] ? x->iso_len[last] - 1 : 0);
        td->info = (uint32_t)((start + p) & 0xFFFF) | (k - 1) << 24 | TD_NOINT |
                   (uint32_t)CC_NOTACCESSED << 28;
        td->bp0 = page;
        td->be = end;
        for (uint32_t j = 0; j < 8; j++)
            td->psw[j] = j < k ? (uint16_t)(0xE000 | ((addr[p + j] - page) & 0x1FFF)) : 0;
        td->npackets = (uint16_t)k;
        td->snext = i + 1 < n ? tds[i + 1] : NULL;
        td->next = (uint32_t)tds[i + 1];
    }
    tds[n - 1]->info &= ~TD_NOINT;
    ed->idummy = tds[n];
    int q = (ed->iso_head + ed->iso_n) % ISO_QUEUE;
    ed->iso[q] = x;
    ed->iso_n++;
    ed->next_frame = (uint16_t)(start + x->npackets);
    x->hcpriv = tds[0];
    barrier();
    ed->tailp = (uint32_t)tds[n];
    return 0;
}

/* Is the oldest queued transfer done? Each descriptor's condition code is
 * written when it retires; a packet's own is in its status word. */
static int iso_check(struct usb_xfer *x)
{
    int in = (x->ep->addr & USB_DIR_IN) != 0;
    for (struct ohci_itd *td = x->hcpriv; td; td = td->snext)
        if (TD_CC(td->info) >= CC_NOTACCESSED)
            return USB_PENDING;
    uint32_t got = 0, i = 0;
    for (struct ohci_itd *td = x->hcpriv; td; td = td->snext) {
        for (uint32_t j = 0; j < td->npackets; j++, i++) {
            uint16_t psw = td->psw[j];
            int cc = psw >> 12;
            uint32_t len = x->iso_len[i];
            if (TD_CC(td->info) != CC_NOERROR || (cc != CC_NOERROR && cc != CC_UNDERRUN)) {
                if (in)
                    x->iso_len[i] = 0;
                continue;
            }
            uint32_t n = in ? MIN((uint32_t)(psw & 0x7FF), len) : len;
            if (in)
                x->iso_len[i] = (uint16_t)n;
            got += n;
        }
    }
    x->actual = got;
    return 0;
}

static void iso_poll(struct ohci *o, struct ohci_ed *ed, struct usb_xfer **done)
{
    while (ed->iso_n) {
        struct usb_xfer *x = ed->iso[ed->iso_head];
        if (iso_check(x) == USB_PENDING)
            break;
        iretire(o, x->hcpriv);
        x->hcpriv = NULL;
        ed->iso[ed->iso_head] = NULL;
        ed->iso_head = (ed->iso_head + 1) % ISO_QUEUE;
        ed->iso_n--;
        x->status = 0;
        x->hcnext = *done;
        *done = x;
    }
}

/* Every transfer queued on the endpoint (its descriptors chained together
 * for the caller to free once the controller is past them). */
static struct ohci_itd *iso_drop(struct ohci_ed *ed, int status)
{
    struct ohci_itd *chains = NULL;
    while (ed->iso_n) {
        struct usb_xfer *x = ed->iso[ed->iso_head];
        ed->iso[ed->iso_head] = NULL;
        ed->iso_head = (ed->iso_head + 1) % ISO_QUEUE;
        ed->iso_n--;
        struct ohci_itd *first = x->hcpriv, *last = first;
        while (last && last->snext)
            last = last->snext;
        if (first) {
            last->snext = chains;
            chains = first;
        }
        x->hcpriv = NULL;
        if (x->status == USB_PENDING) {
            x->status = status;
            if (status == -ENODEV)
                wakeup(x);
        }
    }
    return chains;
}

/* ---- transfers ---- */

static void td_fill(struct ohci_td *td, uint32_t info, void *buf, uint32_t len)
{
    td->info = info | TD_NOINT | (uint32_t)CC_NOTACCESSED << 28;
    td->cbp = len ? (uint32_t)buf : 0;
    td->be = len ? (uint32_t)buf + len - 1 : 0;
    td->start = (uint32_t)buf;
    td->len = (uint16_t)len;
}

/* A descriptor covers at most 8 KiB and one page crossing. */
static uint32_t chunk_at(uint32_t addr, uint32_t left)
{
    return MIN(left, 0x2000u - (addr & 0xFFF));
}

static int ohci_submit(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ohci *o = hc->priv;
    struct usb_ep *ep = x->ep;
    struct ohci_ed *ed = ep_ed(o, x->dev, ep);
    if (!ed)
        return -ENOMEM;
    if (ep->type == USB_EP_ISO)
        return iso_submit(o, ed, x);
    if (ed->xfer)
        return -EBUSY;

    int control = ep->type == USB_EP_CONTROL;
    int in = control ? (x->setup[0] & USB_RT_IN) != 0 : (ep->addr & USB_DIR_IN) != 0;
    int ndata = 0;
    uint32_t a = (uint32_t)x->buf, left = x->len;
    do {
        uint32_t c = chunk_at(a, left);
        ndata++;
        a += c;
        left -= c;
    } while (left);
    if (control && !x->len)
        ndata = 0;                      /* no data stage */
    int n = ndata + (control ? 2 : 0);
    if (n > 18)
        return -EINVAL;

    /* The transfer starts in the ED's empty tail descriptor; a fresh
     * empty one becomes the tail. */
    struct ohci_td *td[20];
    td[0] = ed->dummy;
    for (int i = 1; i <= n; i++) {
        if (!(td[i] = usb_dma_alloc(sizeof(struct ohci_td)))) {
            for (int k = 1; k < i; k++)
                usb_dma_free(td[k], sizeof(struct ohci_td));
            return -ENOMEM;
        }
    }

    int i = 0;
    if (control)
        td_fill(td[i++], TD_SETUP | TD_DATA0, x->setup, 8);
    uint8_t *p = x->buf;
    left = x->len;
    for (int k = 0; k < ndata; k++) {
        uint32_t c = chunk_at((uint32_t)p, left);
        uint32_t info = in ? TD_IN : TD_OUT;
        if (control && k == 0)
            info |= TD_DATA1;
        if (in && k + 1 == ndata)
            info |= TD_R;               /* the last may be short; a short one before halts the ED */
        td_fill(td[i++], info, p, c);
        p += c;
        left -= c;
    }
    if (control)
        td_fill(td[i++], ((in && x->len) ? TD_OUT : TD_IN) | TD_DATA1, NULL, 0);
    td[n - 1]->info &= ~TD_NOINT;       /* interrupt when the last one is done */

    for (int k = 0; k < n; k++) {
        td[k]->snext = k + 1 < n ? td[k + 1] : NULL;
        td[k]->next = (uint32_t)td[k + 1];
    }
    ed->tds = td[0];
    ed->dummy = td[n];
    ed->xfer = x;
    x->hcpriv = ed;
    barrier();
    ed->tailp = (uint32_t)td[n];
    if (control)
        wr(o, HC_CMDSTATUS, CMD_CLF);
    else if (ep->type == USB_EP_BULK)
        wr(o, HC_CMDSTATUS, CMD_BLF);
    return 0;
}

/*
 * Is the ED's transfer done? Either every descriptor was retired (the
 * queue is down to the dummy), or the ED halted on an error, which is
 * also how a short packet before the last descriptor ends a transfer.
 */
static int check(struct ohci_ed *ed, uint32_t *actual)
{
    uint32_t head = ed->headp;
    int halted = head & ED_HALTED;
    if (!halted && (head & ~0xFu) != ed->tailp)
        return USB_PENDING;
    uint32_t got = 0;
    int st = 0, control = ed->type == USB_EP_CONTROL;
    for (struct ohci_td *td = ed->tds; td; td = td->snext) {
        uint32_t cc = TD_CC(td->info);
        if (cc >= CC_NOTACCESSED)
            break;
        uint32_t n = td->cbp ? td->cbp - td->start : td->len;
        int is_data = (td->info & (3u << 19)) != TD_SETUP && td->len;
        if (is_data)
            got += n;
        if (cc == CC_NOERROR)
            continue;
        if (cc == CC_UNDERRUN && !control) {
            st = 0;
            break;
        }
        st = cc == CC_STALL ? -EPIPE : -EIO;
        break;
    }
    if (halted)
        ed->headp = ed->tailp | (st ? 0 : (head & ED_CARRY));
    *actual = got;
    return st;
}

static void ohci_poll(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;
    uint32_t sts = rd(o, HC_INTSTATUS);
    if (sts & (INT_WDH | INT_UE))
        wr(o, HC_INTSTATUS, sts & (INT_WDH | INT_UE));
    if ((sts & INT_UE) && !o->dead) {
        o->dead = 1;
        klog("ohci: unrecoverable controller error\n");
    }
    reclaim(o);

    struct usb_xfer *done = NULL;
    for (int l = 0; l < 3; l++) {
        for (struct ohci_ed *ed = o->head[l]->snext; ed; ed = ed->snext) {
            if (ed->type == USB_EP_ISO) {
                iso_poll(o, ed, &done);
                continue;
            }
            if (!ed->xfer)
                continue;
            uint32_t actual = 0;
            int st = check(ed, &actual);
            if (st == USB_PENDING)
                continue;
            struct usb_xfer *x = ed->xfer;
            retire(o, ed->tds);
            ed->tds = NULL;
            ed->xfer = NULL;
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

static void ohci_cancel(struct usb_hc *hc, struct usb_xfer *x)
{
    struct ohci *o = hc->priv;
    struct ohci_ed *ed = x->ep->hcpriv;
    if (!ed)
        return;
    if (ed->type == USB_EP_ISO) {       /* cancelling one cancels the queue */
        if (!ed->iso_n)
            return;
        ed->info |= ED_SKIP;
        wait_frames(o);
        struct ohci_itd *chains = iso_drop(ed, -ETIMEDOUT);
        ed->headp = ed->tailp;
        free_ichain(chains);
        barrier();
        ed->info &= ~ED_SKIP;
        return;
    }
    if (ed->xfer != x)
        return;
    ed->info |= ED_SKIP;
    wait_frames(o);
    ed->headp = ed->tailp | (ed->headp & ED_CARRY);
    free_chain(ed->tds);
    ed->tds = NULL;
    ed->xfer = NULL;
    barrier();
    ed->info &= ~ED_SKIP;
}

static void ohci_dev_free(struct usb_hc *hc, struct usb_dev *dev)
{
    struct ohci *o = hc->priv;
    struct usb_ep *eps[33];
    int any = 0;
    eps[0] = &dev->ep0;
    for (int i = 0; i < 32; i++)
        eps[i + 1] = &dev->eps[i];
    for (int i = 0; i < 33; i++) {
        struct ohci_ed *ed = eps[i]->hcpriv;
        if (ed) {
            ed->info |= ED_SKIP;
            any = 1;
        }
    }
    if (!any)
        return;
    wait_frames(o);
    for (int i = 0; i < 33; i++)
        if (eps[i]->hcpriv)
            ed_unlink(eps[i]->hcpriv);
    wait_frames(o);
    for (int i = 0; i < 33; i++) {
        struct ohci_ed *ed = eps[i]->hcpriv;
        if (!ed)
            continue;
        if (ed->xfer) {
            ed->xfer->status = -ENODEV;
            wakeup(ed->xfer);
        }
        free_ichain(iso_drop(ed, -ENODEV));
        free_chain(ed->tds);
        usb_dma_free(ed->dummy, sizeof(struct ohci_td));
        usb_dma_free(ed->idummy, sizeof(struct ohci_itd));
        usb_dma_free(ed, sizeof(*ed));
        eps[i]->hcpriv = NULL;
    }
}

/* After CLEAR_FEATURE(HALT) the endpoint starts with DATA0 again. */
static void ohci_ep_reset(struct usb_hc *hc, struct usb_dev *dev, struct usb_ep *ep)
{
    (void)hc; (void)dev;
    struct ohci_ed *ed = ep->hcpriv;
    if (ed && !ed->xfer)
        ed->headp &= ~(ED_CARRY | ED_HALTED);
}

/* ---- root hub ---- */

static int port_reg(int port)
{
    return HC_RHPORT + 4 * (port - 1);
}

static int ohci_port_status(struct usb_hub *h, int port)
{
    struct ohci *o = h->hc->priv;
    uint32_t v = rd(o, port_reg(port));
    if (v & PS_CHANGES)
        wr(o, port_reg(port), v & PS_CHANGES);
    return ((v & PS_CCS) ? PORT_CONNECT : 0) | ((v & PS_PES) ? PORT_ENABLE : 0) |
           ((v & PS_CSC) ? PORT_CHANGE : 0);
}

static int ohci_port_reset(struct usb_hub *h, int port)
{
    struct ohci *o = h->hc->priv;
    wr(o, port_reg(port), PS_PRS);
    for (int i = 0; i < 50 && !(rd(o, port_reg(port)) & PS_PRSC); i++)
        usb_mdelay(2);
    wr(o, port_reg(port), PS_PRSC);
    uint32_t v = rd(o, port_reg(port));
    if (!(v & PS_CCS))
        return -ENODEV;
    if (!(v & PS_PES))
        return -EIO;
    return (v & PS_LSDA) ? USB_SPEED_LOW : USB_SPEED_FULL;
}

static void ohci_port_disable(struct usb_hub *h, int port)
{
    struct ohci *o = h->hc->priv;
    wr(o, port_reg(port), PS_CLEAR_ENABLE);
}

static void ohci_suspend(struct usb_hc *hc);
static void ohci_resume(struct usb_hc *hc);

static const struct usb_hc_ops ohci_ops = {
    .submit = ohci_submit,
    .cancel = ohci_cancel,
    .poll = ohci_poll,
    .dev_free = ohci_dev_free,
    .ep_reset = ohci_ep_reset,
    .suspend = ohci_suspend,
    .resume = ohci_resume,
};

#define MAX_OHCI 8
static struct ohci *ohcis[MAX_OHCI];
static int nohci;

static void ohci_irq(struct trapframe *tf)
{
    (void)tf;
    spin_lock(&usb_lock);
    for (int i = 0; i < nohci; i++) {
        struct ohci *o = ohcis[i];
        uint32_t sts = rd(o, HC_INTSTATUS) & rd(o, HC_INTENABLE);
        if (sts & ~INT_MIE) {
            wr(o, HC_INTSTATUS, sts & ~INT_MIE);
            ohci_poll(&o->hc);
        }
    }
    spin_unlock(&usb_lock);
}

/* Take the controller from SMM or from the BIOS driver. */
static void take_ownership(struct ohci *o)
{
    uint32_t ctl = rd(o, HC_CONTROL);
    if (ctl & CTL_IR) {
        wr(o, HC_INTENABLE, INT_MIE | (1u << 30));     /* ownership change interrupt */
        wr(o, HC_CMDSTATUS, CMD_OCR);
        for (int i = 0; i < 200 && (rd(o, HC_CONTROL) & CTL_IR); i++)
            usb_mdelay(5);
        if (rd(o, HC_CONTROL) & CTL_IR)
            klog("ohci: SMM would not let go; resetting anyway\n");
    } else if ((ctl & CTL_HCFS) != 0 && (ctl & CTL_HCFS) != CTL_OPER) {
        wr(o, HC_CONTROL, (ctl & ~CTL_HCFS) | CTL_RESUME);
        usb_mdelay(20);
    }
}

/* From SMM or the BIOS, reset, the schedule in, running, the ports powered. */
static void ohci_setup(struct ohci *o)
{
    struct usb_hc *hc = &o->hc;
    take_ownership(o);

    uint32_t fi = rd(o, HC_FMINTERVAL) & 0x3FFF;
    if (fi < 0x2E00 || fi > 0x2F00)
        fi = 0x2EDF;
    wr(o, HC_INTDISABLE, 0xFFFFFFFFu);
    wr(o, HC_CONTROL, 0);               /* USB reset on every port */
    usb_mdelay(50);
    wr(o, HC_CMDSTATUS, CMD_HCR);
    for (int i = 0; i < 100 && (rd(o, HC_CMDSTATUS) & CMD_HCR); i++)
        udelay(10);
    /* Now in USBSUSPEND: it must be made operational within 2 ms. */
    uint32_t fsmps = (fi - 210) * 6 / 7;
    uint32_t fit = (rd(o, HC_FMINTERVAL) & 0x80000000u) ^ 0x80000000u;    /* toggled: new value */
    wr(o, HC_FMINTERVAL, fit | fsmps << 16 | fi);
    wr(o, HC_PERIODSTART, fi * 9 / 10);
    wr(o, HC_LSTHRESH, 0x628);
    for (int i = 0; i < 32; i++)
        o->hcca[i] = (uint32_t)o->head[0];
    wr(o, HC_HCCA, (uint32_t)o->hcca);
    wr(o, HC_CTRLHEAD, (uint32_t)o->head[1]);
    wr(o, HC_BULKHEAD, (uint32_t)o->head[2]);
    wr(o, HC_CTRLCUR, 0);
    wr(o, HC_BULKCUR, 0);
    wr(o, HC_INTSTATUS, 0xFFFFFFFFu);
    if (hc->irq >= 0)
        wr(o, HC_INTENABLE, INT_MIE | INT_WDH | INT_UE);
    wr(o, HC_CONTROL, CTL_PLE | CTL_CLE | CTL_BLE | CTL_OPER | 3);

    uint32_t rha = rd(o, HC_RHDESCA);
    hc->root.nports = (int)MIN(rha & 0xFF, (uint32_t)USB_MAX_PORTS);
    if (!(rha & RH_NPS)) {
        wr(o, HC_RHSTATUS, RH_LPSC);    /* global power, then each port's */
        for (int p = 1; p <= hc->root.nports; p++)
            wr(o, port_reg(p), PS_SET_POWER);
    }
    usb_mdelay(MAX((rha >> 24) * 2, 20u));
    o->dead = 0;
}

static void ohci_suspend(struct usb_hc *hc)
{
    struct ohci *o = hc->priv;
    wr(o, HC_INTDISABLE, 0xFFFFFFFFu);
    wr(o, HC_CONTROL, 0);
}

static void ohci_resume(struct usb_hc *hc)
{
    ohci_setup(hc->priv);
}

static void ohci_init(const struct pci_dev *pci)
{
    struct ohci *o = kzalloc(sizeof(*o));
    if (!o)
        return;
    o->regs = pci_map_bar(pci, PCI_BAR0);
    o->hcca = usb_dma_alloc(256);
    for (int l = 0; l < 3; l++)
        o->head[l] = ed_new(l == 0 ? USB_EP_INTR : l == 1 ? USB_EP_CONTROL : USB_EP_BULK);
    if (!o->regs || !o->hcca || !o->head[0] || !o->head[1] || !o->head[2]) {
        klog("ohci: cannot set up the controller\n");
        return;
    }
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);

    struct usb_hc *hc = &o->hc;
    hc->name = "OHCI";
    hc->ops = &ohci_ops;
    hc->priv = o;
    hc->irq = cmdline_has("usb.poll") ? -1 : pci_irq(pci);
    hc->root.port_status = ohci_port_status;
    hc->root.port_reset = ohci_port_reset;
    hc->root.port_disable = ohci_port_disable;
    hc->root.priv = o;
    ohci_setup(o);

    ohcis[nohci++] = o;
    if (hc->irq >= 0)
        irq_register(hc->irq, ohci_irq);
    usb_register_hc(hc);
    boot_ok("USB: OHCI %u.%u controller, IRQ %d, %d ports", (rd(o, HC_REVISION) >> 4) & 0xF,
            rd(o, HC_REVISION) & 0xF, hc->irq, hc->root.nports);
}

void ohci_probe(void)
{
    struct pci_dev pci;
    uint8_t prog_if;
    for (int i = 0; nohci < MAX_OHCI && pci_find_class_nth(0x0C03, i, &pci, &prog_if) == 0; i++)
        if (prog_if == 0x10)
            ohci_init(&pci);
}
