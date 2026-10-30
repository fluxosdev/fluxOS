/* AX210 PCIe transport. Protocol reference: Linux v6.12 iwlwifi,
 * iwl-context-info-gen3.h, iwl-fh.h and pcie/{ctxt-info-gen3,tx-gen2,rx}.c.
 * One RX queue, one command queue, two station queues; polling is used so
 * the driver also works when firmware has not routed a legacy PCI IRQ.
 * Only kwifid services RX and sends commands. Data TX is protected by tx_lock.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "pci.h"
#include "proc.h"
#include "iwl.h"
#include "wifi.h"

#define RX_COUNT 512
#define TX_COUNT 128
#define CMD_QUEUE 0
#define CSR_INT 0x008
#define CSR_MASK 0x00c
#define CSR_FH_INT 0x010
#define CSR_RESET 0x020
#define CSR_GP 0x024
#define CSR_GIO 0x03c
#define CSR_HW_IF 0x000
#define NIC_READY 0x00400000
#define NIC_PREPARE 0x08000000
#define NIC_REQ 8
#define INT_ERROR ((1u << 29) | (1u << 25))
#define FW_MAX_DMA (16u << 20)

struct dma_mem { uint8_t *p; uint32_t pages; };
struct rx_desc { uint16_t id, reserved[3]; uint64_t addr; } __attribute__((packed));
struct tfd_tb { uint16_t len; uint64_t addr; } __attribute__((packed));
struct tfd { uint16_t n; struct tfd_tb tb[25]; uint32_t pad; } __attribute__((packed));
struct queue {
    struct dma_mem desc, bc, buffers;
    uint16_t read, write;
    int id, sta, tid, active;
};
/* Fields are in firmware order, including reserved/dummy addresses. */
struct context {
    uint16_t version, size;
    uint32_t config;
    uint64_t info, cr_head, tr_tail, cr_tail, tr_head;
    uint16_t cr_size, tr_size;
    uint64_t mtr, mcr;
    uint16_t mtr_size, mcr_size, mtr_db, mcr_db, mtr_msi, mcr_msi;
    uint8_t opt[4];
    uint16_t flags, info_msi;
    uint64_t scratch;
    uint32_t scratch_size, reserved;
} __attribute__((packed));
struct scratch {
    uint16_t mac, version, size, reserved;
    uint32_t flags, reserved2;
    uint64_t pnvm;
    uint32_t pnvm_size, reserved3;
    uint64_t hwm;
    uint32_t hwm_size, debug;
    uint64_t free_rbd;
    uint32_t reserved4;
    uint64_t reduce_power;
    uint32_t reduce_size, reserved5, step[2];
    uint32_t fseq, analog, reserved6[8];
    uint64_t umac[64], lmac[64], paging[64];
} __attribute__((packed));
_Static_assert(sizeof(struct tfd) == 256, "TFH descriptor");
_Static_assert(sizeof(struct context) == 104, "gen3 context");
_Static_assert(sizeof(struct scratch) == 1660, "gen3 scratch");

static struct {
    struct pci_dev pci;
    int found, powered, running, servicing, rx_fragment;
    struct dma_mem ctx, scratch, info, iml, rbd, used, status;
    struct dma_mem rx[RX_COUNT], sections[3][IWL_FW_MAX_SECS];
    struct dma_mem pnvm[IWL_PNVM_MAX_CHUNKS], pnvm_map;
    struct queue cmd, tx[2];
    uint16_t rx_read, rx_write;
    struct iwl_notif_wait *waits;
    struct { int active, done, error; uint16_t seq; uint8_t cmd, group;
             void *resp; uint32_t size, len; } sync;
} t;
static spinlock_t reg_lock = SPINLOCK_INIT("iwl-reg");
static spinlock_t tx_lock = SPINLOCK_INIT("iwl-tx");

static void barrier(void) { __sync_synchronize(); }
static uint16_t get16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }
static void put32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }
static int dma_alloc(struct dma_mem *m, uint32_t size)
{
    if (!size || size > FW_MAX_DMA || m->p) return -EINVAL;
    m->pages = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    m->p = (void *)pmm_alloc_contig(m->pages);
    if (!m->p) { m->pages = 0; return -ENOMEM; }
    memset(m->p, 0, m->pages * PAGE_SIZE);
    return 0;
}
static void dma_free(struct dma_mem *m)
{
    for (uint32_t i = 0; m->p && i < m->pages; i++)
        pmm_free((uint32_t)m->p + i * PAGE_SIZE);
    m->p = NULL; m->pages = 0;
}
uint32_t iwl_read32(uint32_t r) { return *(volatile uint32_t *)(iwl.regs + r); }
void iwl_write32(uint32_t r, uint32_t v) { *(volatile uint32_t *)(iwl.regs + r) = v; }
static void setbits(uint32_t r, uint32_t v) { iwl_write32(r, iwl_read32(r) | v); }
static int poll(uint32_t reg, uint32_t mask, uint32_t value, uint32_t us)
{
    for (uint32_t n = 0; n < us; n += 10) {
        uint32_t v = iwl_read32(reg);
        if (v == ~0u) return -ENODEV;
        if ((v & mask) == value) return 0;
        udelay(10);
    }
    return -ETIMEDOUT;
}
static int nic_get(void)
{
    spin_lock(&reg_lock);
    setbits(CSR_GP, NIC_REQ);
    udelay(2);
    if (poll(CSR_GP, 0x11, 1, 15000) == 0) return 0;
    iwl_write32(CSR_GP, iwl_read32(CSR_GP) & ~NIC_REQ);
    spin_unlock(&reg_lock);
    return -EIO;
}
static void nic_put(void)
{
    iwl_write32(CSR_GP, iwl_read32(CSR_GP) & ~NIC_REQ);
    spin_unlock(&reg_lock);
}
uint32_t iwl_read_prph(uint32_t addr)
{
    if (nic_get() < 0) return ~0u;
    iwl_write32(0x448, addr | (3u << 24));
    uint32_t v = iwl_read32(0x450);
    nic_put(); return v;
}
void iwl_write_prph(uint32_t addr, uint32_t val)
{
    if (nic_get() < 0) return;
    iwl_write32(0x444, addr | (3u << 24));
    iwl_write32(0x44c, val);
    nic_put();
}
int iwl_read_mem(uint32_t addr, uint32_t *buf, uint32_t n)
{
    if ((addr & 3) || n > 1024 || nic_get() < 0) return -EIO;
    iwl_write32(0x40c, addr);
    for (uint32_t i = 0; i < n; i++) buf[i] = iwl_read32(0x41c);
    nic_put(); return 0;
}
int iwl_trans_rfkill(void) { return !(iwl_read32(CSR_GP) & (1u << 27)); }

int iwl_trans_probe(void)
{
    if (t.found) return 0;
    klog("iwl: probing PCI for Intel AX210 (8086:2725)\n");
    if (pci_find_device(0x8086, 0x2725, &t.pci) < 0) {
        klog("iwl: no 8086:2725 found; check the pci device list in dmesg\n");
        pci_log_network_diagnostics(NULL);
        return -ENODEV;
    }
    /* Wake it first: a function left in D3hot may come back with its
     * BARs blank, and pci_set_d0 puts them back. */
    int was = pci_set_d0(&t.pci);
    if (was > 0)
        klog("iwl: AX210 was in D%d, now D0\n", was);
    klog("iwl: found 8086:2725 at %02x:%02x.%u, BAR0 %08x BAR1 %08x\n",
         t.pci.bus, t.pci.dev, t.pci.fn,
         pci_read32(&t.pci, PCI_BAR0), pci_read32(&t.pci, PCI_BAR1));
    /* Disable MSI/MSI-X before using polling. */
    if (pci_read16(&t.pci, 6) & 0x10) {
        uint8_t cap = pci_read8(&t.pci, 0x34) & 0xfc;
        for (int i = 0; cap >= 0x40 && i < 48; i++) {
            uint8_t id = pci_read8(&t.pci, cap);
            if (id == 5) {
                pci_write16(&t.pci, cap + 2, pci_read16(&t.pci, cap + 2) & ~1);
            } else if (id == 0x11) {
                pci_write16(&t.pci, cap + 2, pci_read16(&t.pci, cap + 2) & ~0x8000);
            }
            cap = pci_read8(&t.pci, cap + 1) & 0xfc;
        }
    }
    /* The root port (and any switch) must forward MMIO down and DMA up. */
    pci_enable_upstream(&t.pci, PCI_CMD_MEM | PCI_CMD_MASTER);
    iwl.regs = pci_map_bar(&t.pci, PCI_BAR0);
    if (!iwl.regs) {
        klog("iwl: cannot map AX210 BAR0 (unassigned, above 4 GiB, or MMIO mapping failed)\n");
        pci_log_network_diagnostics(&t.pci);
        return -ENODEV;
    }
    pci_enable(&t.pci, PCI_CMD_MEM | 0x400); /* INTx disabled; poll CSR/RX */
    iwl_write32(CSR_MASK, 0);
    iwl.hw_rev = iwl_read32(CSR_HW_REV);
    iwl.hw_rf_id = iwl_read32(CSR_HW_RF_ID);
    if (iwl.hw_rev == ~0u) {
        klog("iwl: AX210 BAR mapped but register reads return ffffffff\n");
        pci_log_network_diagnostics(&t.pci);
        return -ENODEV;
    }
    iwl.irq = -1;
    t.found = 1;
    klog("iwl: AX210 at %02x:%02x.%u, rev %08x RF %08x (polling)\n",
         t.pci.bus, t.pci.dev, t.pci.fn, iwl.hw_rev, iwl.hw_rf_id);
    return 0;
}
static int hw_ready(void)
{
    setbits(CSR_HW_IF, NIC_READY);
    int r = poll(CSR_HW_IF, NIC_READY, NIC_READY, 50);
    if (!r) setbits(0x88, 1u << 5); /* OS_ALIVE */
    return r;
}
static int prepare_card(void)
{
    int r = hw_ready();
    if (!r || r == -ENODEV) return r;
    /* Match iwl_pcie_prepare_card_hw: wake the management side when
     * NIC ownership is not immediately available. */
    setbits(0x250, 0x80000000);
    udelay(1000);
    for (int attempt = 0; attempt < 10; attempt++) {
        setbits(CSR_HW_IF, NIC_PREPARE);
        for (int us = 0; us < 150000; us += 200) {
            r = hw_ready();
            if (!r || r == -ENODEV) return r;
            udelay(200);
        }
        udelay(25000);
    }
    klog("iwl: NIC ownership timed out, HW_IF %08x\n", iwl_read32(CSR_HW_IF));
    return r;
}
int iwl_trans_power_up(void)
{
    if (!t.found) return -ENODEV;
    int r = prepare_card();
    if (r < 0) return r;
    setbits(CSR_RESET, 0x80);
    udelay(10000);
    /* Software reset releases ownership; retake it before APM init. */
    r = prepare_card();
    if (r < 0) return r;
    setbits(0x100, 0x00800000);
    setbits(0x240, 0xffff0000);
    setbits(0, 0x00080000);
    setbits(CSR_GIO, 2); /* L0S_DISABLED: required by iwl_pcie_apm_config */
    setbits(CSR_GP, 4); /* INIT_DONE */
    if (poll(CSR_GP, 1, 1, 25000) < 0) return -EIO;
    iwl_write32(CSR_MASK, 0);
    iwl_write32(CSR_INT, ~0u);
    iwl_write32(CSR_FH_INT, ~0u);
    pci_enable(&t.pci, PCI_CMD_MASTER);
    t.powered = 1; iwl.dead = 0;
    return 0;
}
static int queue_alloc(struct queue *q)
{
    if (dma_alloc(&q->desc, TX_COUNT * sizeof(struct tfd)) < 0 ||
        dma_alloc(&q->bc, TX_COUNT * 2) < 0 ||
        dma_alloc(&q->buffers, TX_COUNT * PAGE_SIZE) < 0) return -ENOMEM;
    return 0;
}
static void queue_free(struct queue *q)
{
    dma_free(&q->desc); dma_free(&q->bc); dma_free(&q->buffers);
    memset(q, 0, sizeof(*q));
}
int iwl_trans_start_fw(void)
{
    if (!t.powered || iwl.fw.api_ver != 89) return -EINVAL;
    if (dma_alloc(&t.ctx, sizeof(struct context)) < 0 ||
        dma_alloc(&t.scratch, sizeof(struct scratch)) < 0 || dma_alloc(&t.info, PAGE_SIZE) < 0 ||
        dma_alloc(&t.rbd, RX_COUNT * 16) < 0 || dma_alloc(&t.used, RX_COUNT * 32) < 0 ||
        dma_alloc(&t.status, PAGE_SIZE) < 0 || dma_alloc(&t.iml, iwl.fw.iml_len) < 0 ||
        queue_alloc(&t.cmd) < 0) return -ENOMEM;
    memcpy(t.iml.p, iwl.fw.iml, iwl.fw.iml_len);
    struct scratch *s = (void *)t.scratch.p;
    struct context *c = (void *)t.ctx.p;
    s->mac = (uint16_t)iwl.hw_rev; s->size = sizeof(*s) / 4;
    s->flags = (1u << 16) | (1u << 17) | 0xc0000;
    s->free_rbd = (uint32_t)t.rbd.p;
    struct iwl_fw_sec_list *lists[] = { &iwl.fw.umac, &iwl.fw.lmac, &iwl.fw.paging };
    for (int part = 0; part < 3; part++) {
        for (uint32_t i = 0; i < lists[part]->n; i++) {
            struct iwl_fw_sec *sec = &lists[part]->sec[i];
            if (dma_alloc(&t.sections[part][i], sec->len) < 0) return -ENOMEM;
            memcpy(t.sections[part][i].p, sec->data, sec->len);
            /* All three address maps have the same packed 64-entry format. */
            uint64_t address = (uint32_t)t.sections[part][i].p;
            memcpy((uint8_t *)s + 124 + part * 512 + i * 8, &address, 8);
        }
    }
    for (int i = 0; i < RX_COUNT; i++) {
        if (dma_alloc(&t.rx[i], PAGE_SIZE) < 0) return -ENOMEM;
        ((struct rx_desc *)t.rbd.p)[i].addr = (uint32_t)t.rx[i].p;
        ((struct rx_desc *)t.rbd.p)[i].id = i + 1;
    }
    c->info = (uint32_t)t.info.p; c->scratch = (uint32_t)t.scratch.p;
    c->scratch_size = sizeof(*s); c->cr_head = (uint32_t)t.status.p;
    c->tr_tail = (uint32_t)t.info.p + PAGE_SIZE / 2;
    c->cr_tail = (uint32_t)t.info.p + 3 * PAGE_SIZE / 4;
    c->mtr = (uint32_t)t.cmd.desc.p; c->mcr = (uint32_t)t.used.p;
    c->mtr_size = 4; /* log2(128) - 3 */ c->mcr_size = 9; /* log2(512) */
    t.cmd.id = CMD_QUEUE; t.cmd.active = 1;
    t.rx_read = 0; t.rx_write = RX_COUNT - 8; t.rx_fragment = 0;
    barrier();
    iwl_write32(0x118, (uint32_t)t.ctx.p); iwl_write32(0x11c, 0);
    iwl_write32(0x120, (uint32_t)t.iml.p); iwl_write32(0x124, 0);
    iwl_write32(0x128, iwl.fw.iml_len);
    setbits(0, 2);
    t.running = 1;
    iwl_write_prph(0xd05c44, 1); /* UREG_CPU_INIT_RUN */
    iwl_write32(0x1c80, t.rx_write);
    return 0;
}
void iwl_trans_fw_alive(void)
{
    dma_free(&t.iml);
    /* Firmware's virtual paging image must remain resident until stop. */
    for (int p = 0; p < 2; p++)
        for (int i = 0; i < IWL_FW_MAX_SECS; i++) dma_free(&t.sections[p][i]);
}
void iwl_trans_stop(void)
{
    if (!t.found) return;
    int was_running = t.running;
    spin_lock(&tx_lock);
    t.running = 0;
    iwl_write32(CSR_MASK, 0);
    setbits(CSR_RESET, 0x200);
    int stopped = poll(CSR_RESET, 0x100, 0x100, 10000) == 0 || !was_running;
    pci_write16(&t.pci, PCI_COMMAND, pci_read16(&t.pci, PCI_COMMAND) & ~PCI_CMD_MASTER);
    iwl_write32(CSR_RESET, 0x80); udelay(10000);
    /* Complete D0A -> D0U, as in iwl_pcie_gen2_apm_stop. */
    iwl_write32(CSR_GP, iwl_read32(CSR_GP) & ~4u);
    spin_unlock(&tx_lock);
    t.powered = 0; t.waits = NULL; t.sync.active = 0;
    /* Keep DMA memory quarantined if the device never acknowledged stop. */
    if (!stopped) { klog("iwl: DMA stop unconfirmed; retaining buffers\n"); return; }
    dma_free(&t.ctx); dma_free(&t.scratch); dma_free(&t.info); dma_free(&t.iml);
    dma_free(&t.rbd); dma_free(&t.used); dma_free(&t.status); dma_free(&t.pnvm_map);
    for (int i = 0; i < RX_COUNT; i++) dma_free(&t.rx[i]);
    for (int p = 0; p < 3; p++)
        for (int i = 0; i < IWL_FW_MAX_SECS; i++) dma_free(&t.sections[p][i]);
    for (int i = 0; i < IWL_PNVM_MAX_CHUNKS; i++) dma_free(&t.pnvm[i]);
    queue_free(&t.cmd); queue_free(&t.tx[0]); queue_free(&t.tx[1]);
}
int iwl_trans_set_pnvm(const uint8_t *const chunks[], const uint32_t lens[], int n)
{
    if (!t.scratch.p || n < 0 || n > IWL_PNVM_MAX_CHUNKS) return -EINVAL;
    struct scratch *s = (void *)t.scratch.p;
    if (s->pnvm_size) return -EBUSY;
    if (!n) return 0;
    uint32_t total = 0;
    for (int i = 0; i < n; i++) {
        if (!lens[i] || lens[i] > FW_MAX_DMA - total) return -EINVAL;
        total += lens[i];
    }
    if (iwl_fw_has_capa(&iwl.fw, 32)) {
        if (dma_alloc(&t.pnvm_map, 64 * 8) < 0) return -ENOMEM;
        for (int i = 0; i < n; i++) {
            if (dma_alloc(&t.pnvm[i], lens[i]) < 0) return -ENOMEM;
            memcpy(t.pnvm[i].p, chunks[i], lens[i]);
            ((uint64_t *)t.pnvm_map.p)[i] = (uint32_t)t.pnvm[i].p;
        }
        s->pnvm = (uint32_t)t.pnvm_map.p;
    } else {
        if (n != 2) return -EINVAL;
        if (dma_alloc(&t.pnvm[0], total) < 0) return -ENOMEM;
        memcpy(t.pnvm[0].p, chunks[0], lens[0]);
        memcpy(t.pnvm[0].p + lens[0], chunks[1], lens[1]);
        s->pnvm = (uint32_t)t.pnvm[0].p;
    }
    s->pnvm_size = total; barrier(); return 0;
}
void iwl_notif_arm(struct iwl_notif_wait *w, uint8_t group, uint8_t cmd, void *buf, uint32_t size)
{
    memset(w, 0, sizeof(*w)); w->group = group; w->cmd = cmd;
    w->any_legacy = group <= 1; w->buf = buf; w->size = size;
    w->next = t.waits; t.waits = w;
}
void iwl_notif_cancel(struct iwl_notif_wait *w)
{
    struct iwl_notif_wait **p = &t.waits;
    while (*p && *p != w) p = &(*p)->next;
    if (*p) *p = w->next;
}
int iwl_notif_wait(struct iwl_notif_wait *w, uint32_t ms)
{
    uint32_t end = ticks + (ms * HZ + 999) / 1000;
    while (!w->done && !iwl.dead && (int32_t)(ticks - end) < 0) {
        iwl_trans_service();
        if (!w->done && !iwl.dead) sleep_ticks(1);
    }
    iwl_notif_cancel(w);
    return iwl.dead ? -EIO : w->done ? 0 : -ETIMEDOUT;
}
/* Buffer 0 must be 20 bytes and cannot cross a cache line. Each slot is
 * page aligned; the second TB contains the rest, at most one page. */
static void describe(struct queue *q, uint32_t len)
{
    unsigned idx = q->write & (TX_COUNT - 1);
    struct tfd *d = (void *)(q->desc.p + idx * sizeof(*d));
    memset(d, 0, sizeof(*d));
    d->n = len > 20 ? 2 : 1;
    d->tb[0].addr = (uint32_t)q->buffers.p + idx * PAGE_SIZE;
    d->tb[0].len = MIN(len, 20u);
    if (len > 20) { d->tb[1].addr = d->tb[0].addr + 20; d->tb[1].len = len - 20; }
    ((uint16_t *)q->bc.p)[idx] = len;
    barrier(); q->write++;
    iwl_write32(0x460, (uint32_t)q->id << 16 | q->write);
}
static int command(uint8_t group, uint8_t cmd, const void *data, uint32_t len, int sync)
{
    if (!t.running || iwl.dead) return -ENODEV;
    if (len > PAGE_SIZE - 8 || (!data && len)) return -EMSGSIZE;
    struct queue *q = &t.cmd;
    if ((uint16_t)(q->write - q->read) >= TX_COUNT - 1) return -EAGAIN;
    /* The firmware sequence carries the low 8 bits of the hardware
     * write pointer, not the index in our smaller DMA allocation. */
    uint16_t seq = (CMD_QUEUE << 8) | (q->write & 0xff);
    uint8_t *p = q->buffers.p + (q->write & (TX_COUNT - 1)) * PAGE_SIZE;
    memset(p, 0, 8); p[0] = cmd; p[1] = group; memcpy(p + 2, &seq, 2);
    uint16_t size = len; memcpy(p + 4, &size, 2);
    /* Wide header version is the command-ID version (zero here), not
     * the payload API version advertised by TLV_CMD_VERSIONS. */
    p[7] = 0;
    if (len) memcpy(p + 8, data, len);
    if (sync) { t.sync.seq = seq; t.sync.cmd = cmd; t.sync.group = group; }
    describe(q, len + 8); return 0;
}
int iwl_send_cmd_async(uint8_t group, uint8_t cmd, const void *data, uint32_t len)
{
    return command(group, cmd, data, len, 0);
}
int iwl_send_cmd(uint8_t group, uint8_t cmd, const void *data, uint32_t len,
                 void *resp, uint32_t resp_size, uint32_t *resp_len)
{
    if (resp_len) *resp_len = 0;
    if (t.sync.active || t.servicing) return -EBUSY;
    memset(&t.sync, 0, sizeof(t.sync)); t.sync.active = 1;
    t.sync.resp = resp; t.sync.size = resp_size;
    int r = command(group, cmd, data, len, 1);
    uint32_t end = ticks + 2 * HZ;
    while (!r && !t.sync.done && !iwl.dead && (int32_t)(ticks - end) < 0) {
        iwl_trans_service();
        if (!t.sync.done && !iwl.dead) sleep_ticks(1);
    }
    if (!r) r = iwl.dead ? -EIO : !t.sync.done ? -ETIMEDOUT : t.sync.error;
    if (resp_len) *resp_len = t.sync.len;
    t.sync.active = 0;
    if (r == -ETIMEDOUT) {
        /* A late response must not complete a reused command slot. */
        klog("iwl: command %02x:%02x timed out\n", group, cmd);
        iwl.dead = 1;
    }
    return r;
}
static void packet(const struct iwl_rx_packet *p)
{
    uint32_t len = iwl_rx_payload_len(p);
    int matched = 0;
    for (struct iwl_notif_wait *w = t.waits; w; w = w->next) {
        if (w->done || p->cmd != w->cmd ||
            !(p->group == w->group || (w->any_legacy && p->group <= 1))) continue;
        w->len = len;
        if (w->buf) memcpy(w->buf, p->data, MIN(len, w->size));
        w->done = 1; matched = 1;
    }
    if (!(p->sequence & SEQ_RX_FRAME) && (p->sequence >> 8) == CMD_QUEUE) {
        uint16_t expected = t.cmd.read & 0xff;
        if ((p->sequence & 0xff) != expected || t.cmd.read == t.cmd.write) {
            iwl_mvm_fw_error(0); return;
        }
        t.cmd.read++;
        if (t.sync.active && p->sequence == t.sync.seq) {
            t.sync.len = len;
            if (p->cmd != t.sync.cmd || (p->group != t.sync.group && !(p->group == 0 && t.sync.group == 1)))
                t.sync.error = -EIO;
            else if (t.sync.resp && len > t.sync.size) t.sync.error = -EMSGSIZE;
            else if (t.sync.resp) memcpy(t.sync.resp, p->data, len);
            t.sync.done = 1;
        }
        return;
    }
    if (!matched) iwl_mvm_rx(p);
}
void iwl_trans_service(void)
{
    if (!t.running || t.servicing || iwl.dead) return;
    t.servicing = 1;
    uint32_t cause = iwl_read32(CSR_INT);
    if (cause == ~0u || (cause & INT_ERROR)) {
        iwl_mvm_fw_error(cause & (1u << 29)); goto out;
    }
    iwl_write32(CSR_INT, cause); iwl_write32(CSR_FH_INT, iwl_read32(CSR_FH_INT));
    uint16_t closed = *(volatile uint16_t *)t.status.p & (RX_COUNT - 1);
    barrier();
    while (t.rx_read != closed && !iwl.dead) {
        unsigned slot = t.rx_read & (RX_COUNT - 1);
        uint8_t *completion = t.used.p + slot * 32;
        unsigned id = get16(completion + 4);
        if (!id || id > RX_COUNT) { iwl_mvm_fw_error(1); break; }
        uint8_t *buf = t.rx[id - 1].p;
        /* Fragmented MPDUs exceed our negotiated legacy MTU. Recycle
         * the buffer without interpreting a continuation as a header. */
        int fragmented = completion[6] & 1;
        int discard = fragmented || t.rx_fragment;
        t.rx_fragment = fragmented;
        for (uint32_t off = 0; !discard && off + 8 <= PAGE_SIZE && !iwl.dead;) {
            uint32_t raw = get32(buf + off), n = raw & RX_LEN_MASK;
            if (raw == 0xffffffff || !raw) break;
            if (n < 4 || n > PAGE_SIZE - off - 4) { iwl_mvm_fw_error(1); break; }
            packet((void *)(buf + off));
            off += (n + 4 + 63) & ~63u;
        }
        memset(buf, 0, PAGE_SIZE);
        ((struct rx_desc *)t.rbd.p)[t.rx_write].addr = (uint32_t)buf;
        ((struct rx_desc *)t.rbd.p)[t.rx_write].id = id;
        t.rx_write = (t.rx_write + 1) & (RX_COUNT - 1);
        t.rx_read = (t.rx_read + 1) & (RX_COUNT - 1);
    }
    barrier(); iwl_write32(0x1c80, t.rx_write & ~7u);
out:
    t.servicing = 0;
}
int iwl_trans_alloc_txq(int sta, int tid)
{
    if (sta < 0 || sta >= 32 || tid < 0 || tid > 15) return -EINVAL;   /* 15: management */
    int h;
    for (h = 0; h < 2 && t.tx[h].active; h++);
    if (h == 2) return -ENOSPC;
    struct queue *q = &t.tx[h];
    if (queue_alloc(q) < 0) { queue_free(q); return -ENOMEM; }
    /* TX_QUEUE_CFG_RSP: queue number, flags, write pointer, reserved; 16 bits each. */
    uint8_t cmd[36] = { 0 }, rsp[8]; uint32_t got = 0;
    put32(cmd + 4, 1u << sta); cmd[8] = tid;
    put32(cmd + 16, 4); put32(cmd + 20, (uint32_t)q->bc.p); put32(cmd + 28, (uint32_t)q->desc.p);
    int r = iwl_send_cmd(GRP_DATA_PATH, 0x17, cmd, sizeof(cmd), rsp, sizeof(rsp), &got);
    if (r < 0) {
        /* The firmware may own these addresses: after a timeout, and after
         * a reply it did send but we could not read. */
        if (r == -EMSGSIZE || r == -EIO) iwl.dead = 1;
        if (!iwl.dead) queue_free(q);
        return r;
    }
    unsigned id = get16(rsp);
    if (got != sizeof(rsp) || id >= 512 || id == CMD_QUEUE ||
        (t.tx[1-h].active && t.tx[1-h].id == (int)id)) { iwl.dead = 1; return -EIO; }
    q->id = id; q->read = q->write = get16(rsp + 4); q->sta = sta; q->tid = tid;
    q->active = 1;
    return h;
}
void iwl_trans_free_txq(int h)
{
    if (h < 0 || h >= 2 || !t.tx[h].active) return;
    struct queue *q = &t.tx[h];
    spin_lock(&tx_lock); q->active = 0; spin_unlock(&tx_lock);
    uint8_t cmd[36] = { 0 }; put32(cmd, 1); put32(cmd + 4, 1u << q->sta); put32(cmd + 8, q->tid);
    if (iwl_send_cmd(GRP_DATA_PATH, 0x17, cmd, sizeof(cmd), NULL, 0, NULL) == 0)
        queue_free(q);
    else iwl.dead = 1; /* quarantine until reset */
}
int iwl_trans_tx(int h, const void *cmd, const uint8_t *f, uint32_t hlen, uint32_t len)
{
    if (h < 0 || h >= 2 || hlen > len || len > PAGE_SIZE - 32) return -EINVAL;
    spin_lock(&tx_lock);
    struct queue *q = &t.tx[h];
    int r = 0;
    if (!t.running || iwl.dead || !q->active) r = -ENETDOWN;
    else if ((uint16_t)(q->write - q->read) >= TX_COUNT - 1) r = -EAGAIN;
    else {
        uint8_t *p = q->buffers.p + (q->write & (TX_COUNT - 1)) * PAGE_SIZE;
        uint16_t seq = (q->id << 8) | (q->write & (TX_COUNT - 1));
        p[0] = 0x1c; p[1] = 0; memcpy(p + 2, &seq, 2);
        memcpy(p + 4, cmd, 28); memcpy(p + 32, f, len);
        unsigned idx = q->write & (TX_COUNT - 1);
        struct tfd *d = (void *)(q->desc.p + idx * sizeof(*d));
        memset(d, 0, sizeof(*d));
        d->n = len > hlen ? 3 : 2;
        d->tb[0].addr = (uint32_t)p; d->tb[0].len = 20;
        d->tb[1].addr = (uint32_t)p + 20; d->tb[1].len = 12 + hlen;
        if (len > hlen) {
            d->tb[2].addr = (uint32_t)p + 32 + hlen;
            d->tb[2].len = len - hlen;
        }
        ((uint16_t *)q->bc.p)[idx] = len;
        barrier(); q->write++;
        iwl_write32(0x460, (uint32_t)q->id << 16 | q->write);
    }
    spin_unlock(&tx_lock); return r;
}
void iwl_trans_reclaim(int id, uint16_t ssn)
{
    spin_lock(&tx_lock);
    for (int h = 0; h < 2; h++) {
        struct queue *q = &t.tx[h];
        if (!q->active || q->id != id) continue;
        if ((uint16_t)(ssn - q->read) <= (uint16_t)(q->write - q->read)) q->read = ssn;
        else { iwl.dead = 1; klog("iwl: invalid TX completion\n"); }
    }
    spin_unlock(&tx_lock);
}
void iwl_mvm_tx_done(int queue, int index) { iwl_trans_reclaim(queue, index); }

/* The ring pointers at the moment of a crash. The SCD sequence the firmware
 * reports wraps at 12 bits, while read/write here are free-running 16-bit
 * counters, so a large (write - read) in the log means the two numbering
 * spaces have drifted and reclaim is no longer freeing descriptors. */
void iwl_trans_dump_queues(void)
{
    klog("iwl: cmd queue read %u write %u (in flight %u)\n", t.cmd.read, t.cmd.write,
         (uint16_t)(t.cmd.write - t.cmd.read));
    for (int h = 0; h < 2; h++) {
        struct queue *q = &t.tx[h];
        if (q->active)
            klog("iwl: tx queue %d (fw id %d sta %d tid %d) read %u write %u (in flight %u)\n",
                 h, q->id, q->sta, q->tid, q->read, q->write,
                 (uint16_t)(q->write - q->read));
    }
}
