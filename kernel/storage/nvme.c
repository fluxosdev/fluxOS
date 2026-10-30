/*
 * NVMe driver: solid-state disks on PCI Express (class 01h, subclass 08h,
 * programming interface 02h), as in most PCs since about 2015 and on QEMU
 * with "-device nvme".
 *
 * Each controller gets the admin queue pair and one I/O queue pair, and
 * has one command under way at a time: the caller writes a submission
 * entry, rings the doorbell and sleeps until the interrupt (the PCI pin;
 * no MSI) says the completion entry is there, or polls before processes
 * exist, and for good once a completion has gone by without its
 * interrupt (nvme.poll does so from the start). Callers serialise through the VFS lock, as for the other disk
 * drivers, so one bounce buffer and one PRP list serve each controller.
 *
 * Each namespace is a disk, named nvme0n1, nvme0n2, nvme1n1, ... as Linux
 * names them; their partitions are nvme0n1p1 and so on (blkdev.c).
 */
#include "kernel.h"
#include "arch.h"
#include "blkdev.h"
#include "drivers.h"
#include "lock.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "x86.h"

/* Controller registers */
#define REG_CAP    0x00                 /* 64-bit */
#define REG_VS     0x08
#define REG_INTMS  0x0C
#define REG_INTMC  0x10
#define REG_CC     0x14
#define REG_CSTS   0x1C
#define REG_AQA    0x24
#define REG_ASQ    0x28                 /* 64-bit */
#define REG_ACQ    0x30                 /* 64-bit */
#define REG_DB     0x1000               /* doorbells, 4 << CAP.DSTRD bytes apart */

#define CC_EN        (1u << 0)
#define CC_SHN_NORMAL (1u << 14)
#define CC_SHN_MASK  (3u << 14)
#define CC_IOSQES    (6u << 16)         /* 64-byte submission entries */
#define CC_IOCQES    (4u << 20)         /* 16-byte completion entries */

#define CSTS_RDY     (1u << 0)
#define CSTS_CFS     (1u << 1)          /* controller fatal status */
#define CSTS_SHST_MASK (3u << 2)
#define CSTS_SHST_DONE (2u << 2)

#define PCI_CMD_INTX_OFF (1u << 10)

/* Admin commands */
#define ADM_CREATE_SQ 0x01
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY  0x06
#define ADM_SET_FEAT  0x09
#define FEAT_NQUEUES  0x07

/* NVM commands */
#define NVM_FLUSH 0x00
#define NVM_WRITE 0x01
#define NVM_READ  0x02

#define ADMIN_DEPTH 16
#define IO_DEPTH    64                  /* 64 x 64 bytes: one page of submissions */
#define BOUNCE_SIZE 65536u              /* also the most one command moves */
#define CMD_TIMEOUT 10000               /* ms */
#define MAX_CTRLS   4
#define MAX_NS      8                   /* namespaces per controller */

struct nvme_sqe {
    uint32_t cdw0;                      /* opcode, command ID in bits 16-31 */
    uint32_t nsid;
    uint32_t reserved[2];
    uint64_t mptr;
    uint64_t prp1, prp2;
    uint32_t cdw10, cdw11, cdw12, cdw13, cdw14, cdw15;
};

struct nvme_cqe {
    uint32_t result;
    uint32_t reserved;
    uint16_t sq_head, sq_id;
    uint16_t cid;
    uint16_t status;                    /* phase in bit 0, the status above it */
};

struct nvme_queue {
    struct nvme_sqe *sq;
    volatile struct nvme_cqe *cq;
    uint16_t depth, qid;
    uint16_t sq_tail, cq_head;
    uint16_t phase;
};

struct nvme_ctrl;

struct nvme_ns {
    struct nvme_ctrl *c;
    uint32_t nsid;
    uint32_t shift;                     /* log2 of the sector size */
    struct blockdev bd;
};

struct nvme_ctrl {
    int num;
    volatile uint8_t *regs;
    int irq;
    int irq_on;                         /* the handler is registered */
    int intr;                           /* and the controller's interrupts are unmasked */
    uint32_t dstrd;                     /* doorbell stride in bytes */
    uint32_t timeout;                   /* ms to become ready (CAP.TO) */
    uint32_t max_xfer;                  /* bytes in one command */
    int vwc;                            /* there is a volatile write cache to flush */
    struct nvme_queue admin, io;
    uint64_t *prp_list;                 /* one page */
    uint8_t *bounce;
    /* The command under way, and what came back for it (under lock) */
    uint16_t cid;
    volatile int done;
    volatile uint16_t status;
    volatile int irq_seen;              /* the handler took a completion for it */
    spinlock_t lock;
    struct nvme_ns *ns[MAX_NS];
    int nns;
};

static struct nvme_ctrl ctrls[MAX_CTRLS];
static int nctrls;

static inline uint32_t rd32(struct nvme_ctrl *c, int reg)
{
    return *(volatile uint32_t *)(c->regs + reg);
}

static inline void wr32(struct nvme_ctrl *c, int reg, uint32_t v)
{
    *(volatile uint32_t *)(c->regs + reg) = v;
}

/* 64-bit registers as two halves, low first; everything is below 1 GiB. */
static inline void wr64(struct nvme_ctrl *c, int reg, uint32_t v)
{
    wr32(c, reg, v);
    wr32(c, reg + 4, 0);
}

static inline void barrier(void)
{
    __asm__ volatile("" ::: "memory");
}

static int in_process(void)
{
    return current && current->pid > 0;
}

static void sq_doorbell(struct nvme_ctrl *c, struct nvme_queue *q)
{
    wr32(c, REG_DB + 2 * q->qid * c->dstrd, q->sq_tail);
}

static void cq_doorbell(struct nvme_ctrl *c, struct nvme_queue *q)
{
    wr32(c, REG_DB + (2 * q->qid + 1) * c->dstrd, q->cq_head);
}

/* Take whatever completions a queue has, noting the one for the command
 * under way. Lock held. Returns how many there were. */
static int cq_reap(struct nvme_ctrl *c, struct nvme_queue *q)
{
    int n = 0;
    for (;;) {
        volatile struct nvme_cqe *e = &q->cq[q->cq_head];
        uint16_t st = e->status;
        if ((st & 1) != q->phase)
            break;
        barrier();
        if (e->cid == c->cid && !c->done) {
            c->status = st >> 1;
            c->done = 1;
        }
        if (++q->cq_head == q->depth) {
            q->cq_head = 0;
            q->phase ^= 1;
        }
        n++;
    }
    if (n)
        cq_doorbell(c, q);
    return n;
}

static int cmd_done(struct nvme_ctrl *c)
{
    spin_lock(&c->lock);
    cq_reap(c, &c->admin);
    cq_reap(c, &c->io);
    int done = c->done;
    spin_unlock(&c->lock);
    return done;
}

static void nvme_irq(struct trapframe *tf)
{
    (void)tf;
    for (int i = 0; i < nctrls; i++) {
        struct nvme_ctrl *c = &ctrls[i];
        if (!c->irq_on)
            continue;
        spin_lock(&c->lock);
        int n = cq_reap(c, &c->admin) + cq_reap(c, &c->io);
        if (n)
            c->irq_seen = 1;
        spin_unlock(&c->lock);
        if (n)
            wakeup(c);
    }
}

/* ---- the controller ---- */

static int wait_csts(struct nvme_ctrl *c, uint32_t mask, uint32_t want)
{
    for (uint32_t i = 0; i < c->timeout; i++) {
        uint32_t csts = rd32(c, REG_CSTS);
        if ((csts & mask) == want)
            return 0;
        if (csts == 0xFFFFFFFFu)
            return -1;                  /* gone from the bus */
        mdelay(1);
    }
    return -1;
}

static int ctrl_disable(struct nvme_ctrl *c)
{
    if (rd32(c, REG_CC) & CC_EN)
        wr32(c, REG_CC, rd32(c, REG_CC) & ~CC_EN);
    return wait_csts(c, CSTS_RDY, 0);
}

static void queue_reset(struct nvme_queue *q)
{
    memset(q->sq, 0, PAGE_SIZE);
    memset((void *)q->cq, 0, PAGE_SIZE);
    q->sq_tail = q->cq_head = 0;
    q->phase = 1;
}

static int ctrl_start(struct nvme_ctrl *c);

/*
 * A command finished, but the handler never ran for it: its interrupt
 * does not reach us (the routing is wrong, as QEMU's firmware leaves it
 * after waking from S3). Decided only once a wait has run out and the
 * completion is there after all (a command that happened to finish before
 * the first look, its interrupt still on its way, must not count), as
 * every command would otherwise take the whole timeout: the controller
 * is polled from now on instead.
 */
static void lost_interrupt(struct nvme_ctrl *c)
{
    klog("nvme%d: a command completed without an interrupt; polling from now on\n", c->num);
    wr32(c, REG_INTMS, 1);
    c->intr = 0;
    c->irq_on = 0;
}

/*
 * Run one command and wait for it. Returns 0, the controller's status
 * (status code type << 8 | status code) if it refused it, or -EIO if
 * it did not answer (the controller is then reset).
 */
static int submit(struct nvme_ctrl *c, struct nvme_queue *q, struct nvme_sqe *cmd,
                  uint32_t timeout_ms)
{
    spin_lock(&c->lock);
    c->cid++;
    c->done = 0;
    c->irq_seen = 0;
    cmd->cdw0 = (cmd->cdw0 & 0xFFFF) | (uint32_t)c->cid << 16;
    q->sq[q->sq_tail] = *cmd;
    if (++q->sq_tail == q->depth)
        q->sq_tail = 0;
    barrier();
    sq_doorbell(c, q);
    spin_unlock(&c->lock);

    int done;
    if (in_process() && c->intr) {
        /* The interrupt wakes us. */
        uint32_t deadline = ticks + timeout_ms / (1000 / HZ) + 1;
        wait_event_timeout(c, (done = cmd_done(c)) != 0, deadline);
        if (done && !c->irq_seen && (int32_t)(ticks - deadline) >= 0)
            lost_interrupt(c);
    } else {
        done = 0;
        for (uint32_t i = 0; !(done = cmd_done(c)) && i < timeout_ms * 50; i++)
            udelay(20);
    }
    barrier();

    if (!done) {
        klog("nvme%d: command %02x timed out; resetting the controller\n", c->num,
             cmd->cdw0 & 0xFF);
        if (q == &c->io)
            ctrl_start(c);
        return -EIO;
    }
    return c->status & 0x7FF;
}

static int create_io_queues(struct nvme_ctrl *c)
{
    struct nvme_sqe cmd = { .cdw0 = ADM_SET_FEAT, .cdw10 = FEAT_NQUEUES, .cdw11 = 0 };
    submit(c, &c->admin, &cmd, CMD_TIMEOUT);    /* one of each; refusals are harmless */

    cmd = (struct nvme_sqe){
        .cdw0 = ADM_CREATE_CQ, .prp1 = (uint32_t)c->io.cq,
        .cdw10 = (uint32_t)(c->io.depth - 1) << 16 | c->io.qid,
        .cdw11 = 1u << 1 | 1u,                  /* interrupts on (vector 0), contiguous */
    };
    if (submit(c, &c->admin, &cmd, CMD_TIMEOUT) != 0)
        return -1;
    cmd = (struct nvme_sqe){
        .cdw0 = ADM_CREATE_SQ, .prp1 = (uint32_t)c->io.sq,
        .cdw10 = (uint32_t)(c->io.depth - 1) << 16 | c->io.qid,
        .cdw11 = (uint32_t)c->io.qid << 16 | 1u,   /* completions to that queue, contiguous */
    };
    if (submit(c, &c->admin, &cmd, CMD_TIMEOUT) != 0)
        return -1;
    return 0;
}

/*
 * Reset and enable the controller with fresh queues: at boot, after a
 * command times out, and after suspend to RAM. Interrupts stay masked in
 * the controller (and commands are polled for) until the queues are up.
 */
static int ctrl_start(struct nvme_ctrl *c)
{
    c->intr = 0;
    if (ctrl_disable(c) < 0)
        return -1;
    spin_lock(&c->lock);
    queue_reset(&c->admin);
    queue_reset(&c->io);
    c->done = 0;
    spin_unlock(&c->lock);

    wr32(c, REG_AQA, (uint32_t)(ADMIN_DEPTH - 1) << 16 | (ADMIN_DEPTH - 1));
    wr64(c, REG_ASQ, (uint32_t)c->admin.sq);
    wr64(c, REG_ACQ, (uint32_t)c->admin.cq);
    wr32(c, REG_INTMS, 1);
    wr32(c, REG_CC, CC_IOCQES | CC_IOSQES | CC_EN);    /* NVM commands, 4 KiB pages */
    if (wait_csts(c, CSTS_RDY | CSTS_CFS, CSTS_RDY) < 0)
        return -1;
    if (create_io_queues(c) < 0)
        return -1;
    if (c->irq_on) {
        wr32(c, REG_INTMC, 1);
        c->intr = 1;
    }
    return 0;
}

/* ---- disks ---- */

/*
 * Describe [buf, buf + len) to the controller as a PRP pair: the first
 * page, then the second or a list of the rest. Kernel memory below
 * 1 GiB is identity-mapped, so contiguous here is contiguous to it.
 */
static void build_prps(struct nvme_ctrl *c, struct nvme_sqe *cmd, uint32_t addr, uint32_t len)
{
    cmd->prp1 = addr;
    uint32_t first = PAGE_SIZE - (addr & (PAGE_SIZE - 1));
    if (len <= first)
        return;
    uint32_t next = addr + first, rest = len - first;
    if (rest <= PAGE_SIZE) {
        cmd->prp2 = next;
        return;
    }
    for (uint32_t i = 0; i * PAGE_SIZE < rest; i++)
        c->prp_list[i] = next + i * PAGE_SIZE;
    cmd->prp2 = (uint32_t)c->prp_list;
}

static int ns_rw(struct nvme_ns *ns, uint32_t lba, uint32_t count, void *buf, int write)
{
    struct nvme_ctrl *c = ns->c;
    uint32_t len = count << ns->shift;
    uint32_t addr = (uint32_t)buf;
    void *mem = buf;
    if (addr < 0x100000 || addr + len > KERNEL_SPACE_END || addr + len < addr || (addr & 3)) {
        mem = c->bounce;
        if (write)
            memcpy(mem, buf, len);
    }
    struct nvme_sqe cmd = {
        .cdw0 = write ? NVM_WRITE : NVM_READ, .nsid = ns->nsid,
        .cdw10 = lba, .cdw11 = 0, .cdw12 = count - 1,
    };
    build_prps(c, &cmd, (uint32_t)mem, len);
    int r = submit(c, &c->io, &cmd, CMD_TIMEOUT);
    if (r != 0) {
        if (r > 0)
            klog("%s: %s of sector %u failed, status %03x\n", ns->bd.name,
                 write ? "write" : "read", lba, r);
        return -EIO;
    }
    if (!write && mem != buf)
        memcpy(buf, mem, len);
    return 0;
}

static int nvme_read(struct blockdev *bd, uint32_t lba, uint32_t count, void *buf)
{
    struct nvme_ns *ns = bd->priv;
    uint32_t most = ns->c->max_xfer >> ns->shift;
    uint8_t *out = buf;
    while (count) {
        uint32_t n = MIN(count, most);
        if (ns_rw(ns, lba, n, out, 0) < 0)
            return -EIO;
        lba += n;
        count -= n;
        out += n << ns->shift;
    }
    return 0;
}

static int nvme_write(struct blockdev *bd, uint32_t lba, uint32_t count, const void *buf)
{
    struct nvme_ns *ns = bd->priv;
    uint32_t most = ns->c->max_xfer >> ns->shift;
    const uint8_t *in = buf;
    while (count) {
        uint32_t n = MIN(count, most);
        if (ns_rw(ns, lba, n, (void *)in, 1) < 0)
            return -EIO;
        lba += n;
        count -= n;
        in += n << ns->shift;
    }
    return 0;
}

/* Without a volatile write cache, everything written is already safe. */
static int nvme_flush(struct blockdev *bd)
{
    struct nvme_ns *ns = bd->priv;
    if (!ns->c->vwc)
        return 0;
    struct nvme_sqe cmd = { .cdw0 = NVM_FLUSH, .nsid = ns->nsid };
    return submit(ns->c, &ns->c->io, &cmd, 30000) == 0 ? 0 : -EIO;
}

/* ---- probing ---- */

static int identify(struct nvme_ctrl *c, uint32_t cns, uint32_t nsid, void *page)
{
    struct nvme_sqe cmd = { .cdw0 = ADM_IDENTIFY, .nsid = nsid, .prp1 = (uint32_t)page,
                            .cdw10 = cns };
    return submit(c, &c->admin, &cmd, CMD_TIMEOUT);
}

static void ident_string(const uint8_t *s, int len, char *out, int size)
{
    int k = 0;
    for (int i = 0; i < len && k < size - 1; i++)
        out[k++] = (char)s[i];
    out[k] = 0;
    while (k > 0 && (out[k - 1] == ' ' || out[k - 1] == 0))
        out[--k] = 0;
}

static void ns_probe(struct nvme_ctrl *c, uint32_t nsid, uint8_t *id, const char *model)
{
    if (identify(c, 0, nsid, id) != 0)
        return;
    uint32_t nsze_lo = *(uint32_t *)(id + 0), nsze_hi = *(uint32_t *)(id + 4);
    if (!nsze_lo && !nsze_hi)
        return;                         /* not an active namespace */
    uint32_t lbaf = *(uint32_t *)(id + 128 + 4 * (id[26] & 0x0F));
    uint32_t ms = lbaf & 0xFFFF, shift = (lbaf >> 16) & 0xFF;
    if (ms || shift < 9 || (1u << shift) > BLK_MAX_SECTOR || (1u << shift) > c->max_xfer) {
        boot_warn("nvme%dn%u: %u-byte sectors%s: not supported", c->num, nsid,
                  shift < 32 ? 1u << shift : 0, ms ? " with metadata" : "");
        return;
    }

    struct nvme_ns *ns = kzalloc(sizeof(*ns));
    if (!ns)
        return;
    ns->c = c;
    ns->nsid = nsid;
    ns->shift = shift;
    struct blockdev *bd = &ns->bd;
    snprintf(bd->name, sizeof(bd->name), "nvme%dn%u", c->num, nsid);
    strlcpy(bd->model, model, sizeof(bd->model));
    bd->type = BLK_DISK;
    bd->sector_size = 1u << shift;
    bd->nsectors = nsze_hi ? 0xFFFFFFFFu : nsze_lo;
    bd->priv = ns;
    bd->read = nvme_read;
    bd->write = nvme_write;
    bd->flush = nvme_flush;
    if (blk_register(bd) < 0) {
        kfree(ns);
        return;
    }
    c->ns[c->nns++] = ns;
    boot_ok("%s: NVMe %s, %u MiB", bd->name, bd->model,
            (uint32_t)(((uint64_t)bd->nsectors << shift) >> 20));
    blk_scan_partitions(bd);
}

static void ctrl_init(const struct pci_dev *pci)
{
    struct nvme_ctrl *c = &ctrls[nctrls];
    c->num = nctrls;
    c->lock = (spinlock_t)SPINLOCK_INIT("nvme");
    c->regs = pci_map_bar(pci, PCI_BAR0);
    if (!c->regs) {
        boot_warn("NVMe: controller registers out of reach");
        return;
    }
    pci_enable(pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    c->irq = cmdline_has("nvme.poll") ? -1 : pci_irq(pci);
    uint16_t cmdreg = pci_read16(pci, PCI_COMMAND);
    pci_write16(pci, PCI_COMMAND, c->irq >= 0 ? cmdreg & ~PCI_CMD_INTX_OFF
                                              : cmdreg | PCI_CMD_INTX_OFF);

    uint32_t cap_lo = rd32(c, REG_CAP), cap_hi = rd32(c, REG_CAP + 4);
    uint32_t mqes = (cap_lo & 0xFFFF) + 1;
    c->timeout = MAX(((cap_lo >> 24) & 0xFF) * 500u, 500u);
    c->dstrd = 4u << (cap_hi & 0xF);
    if (!(cap_hi & (1u << 5)) || ((cap_hi >> 16) & 0xF) != 0) {
        boot_warn("NVMe: controller without the NVM command set or 4 KiB pages");
        return;
    }

    /* Five pages: two queues of each kind, and the PRP list. */
    uint32_t pages[5] = { 0 };
    for (int i = 0; i < 5; i++)
        if (!(pages[i] = pmm_alloc()))
            goto nomem;
    c->bounce = (uint8_t *)pmm_alloc_contig(BOUNCE_SIZE / PAGE_SIZE);
    if (!c->bounce)
        goto nomem;
    c->admin = (struct nvme_queue){ .sq = (void *)pages[0], .cq = (void *)pages[1],
                                    .depth = ADMIN_DEPTH, .qid = 0 };
    c->io = (struct nvme_queue){ .sq = (void *)pages[2], .cq = (void *)pages[3],
                                 .depth = (uint16_t)MIN(mqes, (uint32_t)IO_DEPTH), .qid = 1 };
    c->prp_list = (uint64_t *)pages[4];
    c->max_xfer = BOUNCE_SIZE;
    nctrls++;

    if (ctrl_start(c) < 0) {
        boot_warn("nvme%d: controller did not start (status %08x)", c->num, rd32(c, REG_CSTS));
        return;
    }

    uint8_t *id = (uint8_t *)pmm_alloc();
    if (!id)
        return;
    char model[44];
    if (identify(c, 1, 0, id) != 0) {
        boot_warn("nvme%d: controller does not answer IDENTIFY", c->num);
        pmm_free((uint32_t)id);
        return;
    }
    ident_string(id + 24, 40, model, sizeof(model));
    uint8_t mdts = id[77];
    if (mdts && mdts < 4)
        c->max_xfer = PAGE_SIZE << mdts;
    c->vwc = id[525] & 1;
    uint32_t nn = *(uint32_t *)(id + 516);
    uint32_t vs = rd32(c, REG_VS);
    boot_ok("NVMe %u.%u controller: %s, %u namespace%s, IRQ %d%s", vs >> 16, (vs >> 8) & 0xFF,
            model, nn, nn == 1 ? "" : "s", c->irq, c->irq < 0 ? " (polled)" : "");
    for (uint32_t nsid = 1; nsid <= nn && c->nns < MAX_NS; nsid++)
        ns_probe(c, nsid, id, model);
    pmm_free((uint32_t)id);

    if (c->irq >= 0) {
        irq_register(c->irq, nvme_irq);
        c->irq_on = 1;
        wr32(c, REG_INTMC, 1);
        c->intr = 1;
    }
    return;

nomem:
    for (int i = 0; i < 5; i++)
        if (pages[i])
            pmm_free(pages[i]);
    boot_warn("NVMe: out of memory");
}

void nvme_init(void)
{
    struct pci_dev pci;
    uint8_t prog_if;
    for (int i = 0; nctrls < MAX_CTRLS && pci_find_class_nth(0x0108, i, &pci, &prog_if) == 0; i++) {
        if (prog_if != 0x02)
            continue;                   /* not NVMe (a vendor-specific interface) */
        ctrl_init(&pci);
    }
}

/* ---- suspend to RAM ---- */

/* The VFS lock is held, so no command is under way. The drives write
 * back their caches and are told the power is going, which lets them
 * save their own tables, as on a proper power-off. */
void nvme_suspend(void)
{
    for (int i = 0; i < nctrls; i++) {
        struct nvme_ctrl *c = &ctrls[i];
        if (!(rd32(c, REG_CSTS) & CSTS_RDY))
            continue;
        for (int n = 0; n < c->nns; n++)
            nvme_flush(&c->ns[n]->bd);
        wr32(c, REG_INTMS, 1);
        c->intr = 0;
        wr32(c, REG_CC, (rd32(c, REG_CC) & ~CC_SHN_MASK) | CC_SHN_NORMAL);
        if (wait_csts(c, CSTS_SHST_MASK, CSTS_SHST_DONE) < 0)
            klog("nvme%d: did not finish shutting down\n", c->num);
    }
}

/* The controller lost its state with the power: queues set up again. */
void nvme_resume(void)
{
    for (int i = 0; i < nctrls; i++) {
        struct nvme_ctrl *c = &ctrls[i];
        if (ctrl_start(c) < 0)
            klog("nvme%d: the controller did not come back\n", c->num);
    }
}
