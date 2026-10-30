#include "kernel-test.h"
#include <sys/mman.h>
#include "../kernel/wifi/iwl_trans.c"
struct iwl_dev iwl;
volatile uint32_t ticks;
static int errors, dispatched;
void klog(const char *fmt, ...) { (void)fmt; }
void sleep_on(void *p) { (void)p; abort(); }
void iwl_mvm_fw_error(int hw) { (void)hw; errors++; iwl.dead=1; }
void iwl_mvm_rx(const struct iwl_rx_packet *p) { (void)p; dispatched++; }
int iwl_fw_cmd_ver(const struct iwl_fw *fw, uint8_t g, uint8_t c) { (void)fw; (void)g; (void)c; return 17; }
static uint8_t regs[8192], descriptors[TX_COUNT*256], buffers[TX_COUNT*PAGE_SIZE], bc[TX_COUNT*2];
static int reset_seen, master_enabled;
void pci_enable(const struct pci_dev *p, uint32_t bits)
{ (void)p; master_enabled |= bits; }
void udelay(uint32_t us)
{
    if (us == 10000 && (iwl_read32(CSR_RESET) & 0x80)) {
        /* Hardware reset loses ownership; normal RAM would hide this. */
        iwl_write32(CSR_HW_IF, 0);
        iwl_write32(0x88, 0);
        iwl_write32(CSR_RESET, 0);
        reset_seen++;
    }
}
/* Pages for a TX queue, low enough for the 32-bit physical addresses the
 * transport keeps; the firmware's reply is staged and delivered while the
 * command waits, where the kernel would sleep. */
static int pages_freed;
uint32_t pmm_alloc_contig(uint32_t frames)
{
    void *p = mmap(NULL, frames * PAGE_SIZE, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_32BIT, -1, 0);
    CHECK(p != MAP_FAILED && (uintptr_t)p <= UINT32_MAX);
    return (uint32_t)(uintptr_t)p;
}
void pmm_free(uint32_t phys) { CHECK(munmap((void *)(uintptr_t)phys, PAGE_SIZE) == 0); pages_freed++; }
static uint8_t reply[16]; static uint32_t reply_len;
int sleep_ticks(uint32_t n)
{
    uint8_t buf[32] = {0}; struct iwl_rx_packet *p = (void *)buf;
    p->cmd = t.sync.cmd; p->group = t.sync.group; p->sequence = t.sync.seq;
    p->len_n_flags = 4 + reply_len; memcpy(p->data, reply, reply_len);
    packet(p); return 0;
}
static void setup(void)
{
    memset(&t,0,sizeof(t)); memset(&iwl,0,sizeof(iwl));
    memset(regs,0,sizeof(regs)); iwl.regs=regs;
    t.running=1; t.cmd.desc.p=descriptors; t.cmd.buffers.p=buffers; t.cmd.bc.p=bc;
    errors=dispatched=reset_seen=master_enabled=0;
}
int main(void)
{
    setup(); t.found=1; iwl_write32(CSR_GP, 1);
    CHECK(iwl_trans_power_up()==0 && reset_seen==1);
    CHECK(iwl_read32(CSR_HW_IF)&NIC_READY);
    CHECK(iwl_read32(0x88)&(1u<<5));
    CHECK(iwl_read32(CSR_GIO)&2);
    CHECK(master_enabled&PCI_CMD_MASTER);
    setup(); t.found=1; iwl_write32(CSR_HW_IF, ~0u);
    CHECK(iwl_trans_power_up()==-ENODEV && !master_enabled);
    setup();
    uint8_t payload[3]={1,2,3};
    CHECK(command(1,0xd,payload,3,0)==0);
    CHECK(buffers[0]==0xd && buffers[1]==1 && buffers[7]==0);
    CHECK(get16(buffers+4)==3 && !memcmp(buffers+8,payload,3));
    CHECK(((struct tfd*)descriptors)->n==1);
    CHECK(iwl_read32(0x460)==1);
    t.cmd.write=TX_COUNT-1;
    CHECK(command(1,0xd,payload,3,0)==-EAGAIN);
    setup();
    t.cmd.write=128; t.cmd.read=127;
    t.sync.active=1; t.sync.seq=127; t.sync.cmd=0xd; t.sync.group=1;
    uint8_t packet_buf[32]={0}; struct iwl_rx_packet *p=(void*)packet_buf;
    p->cmd=0xd; p->group=1; p->sequence=127; p->len_n_flags=4;
    packet(p); CHECK(t.sync.done && t.cmd.read==128 && !errors);
    packet(p); CHECK(errors==1); /* duplicate completion cannot free another slot */
    /* Slot 0 at hardware pointer 128 must retain sequence 128. Exercise
     * both the 8-bit sequence rollover and the 16-bit hardware rollover. */
    for (unsigned pos=127; pos<65538; pos++) {
        setup(); t.cmd.read=t.cmd.write=(uint16_t)pos;
        t.sync.active=1;
        CHECK(command(1,0xd,payload,3,1)==0);
        uint8_t *header=buffers+(pos&(TX_COUNT-1))*PAGE_SIZE;
        CHECK(get16(header+2)==(pos&255));
        CHECK(iwl_read32(0x460)==(uint16_t)(pos+1));
        p->cmd=0xd; p->group=1; p->sequence=pos&255; p->len_n_flags=4;
        packet(p);
        CHECK(t.sync.done && !t.sync.error && !errors);
        CHECK(t.cmd.read==t.cmd.write);
    }
    setup();
    struct iwl_notif_wait wait; uint8_t out[2]={0};
    iwl_notif_arm(&wait,0,1,out,sizeof(out));
    p->cmd=1; p->group=1; p->sequence=SEQ_RX_FRAME; p->len_n_flags=8;
    memcpy(p->data,"abcd",4); packet(p);
    CHECK(wait.done && wait.len==4 && !memcmp(out,"ab",2) && !dispatched);
    iwl_notif_cancel(&wait); CHECK(!t.waits);
    setup();
    t.sync.active=1; t.sync.seq=0; t.sync.cmd=2; t.sync.group=1; t.cmd.write=1;
    p->sequence=0; p->cmd=3; packet(p);
    CHECK(t.sync.done && t.sync.error==-EIO);
    setup(); t.tx[0].active=1; t.tx[0].id=3; t.tx[0].read=65534; t.tx[0].write=2;
    iwl_trans_reclaim(3,1); CHECK(t.tx[0].read==1 && !iwl.dead);
    iwl_trans_reclaim(3,10); CHECK(iwl.dead && t.tx[0].read==1);
    /* The firmware's SSN wraps at 12 bits: frame 4096 completes as SSN 0. */
    setup(); t.tx[0].active=1; t.tx[0].id=3; t.tx[0].read=4094; t.tx[0].write=4098;
    iwl_trans_reclaim(3,4095); CHECK(t.tx[0].read==4095 && !iwl.dead);
    iwl_trans_reclaim(3,1); CHECK(t.tx[0].read==4097 && !iwl.dead);
    iwl_trans_reclaim(3,4); CHECK(iwl.dead && t.tx[0].read==4097);
    /* Multi-buffer RX must discard the final (unflagged) continuation,
     * including when it arrives in a later poll. */
    setup();
    static uint8_t used[RX_COUNT*32], rbd[RX_COUNT*16], rxbuf[PAGE_SIZE];
    uint16_t closed=1;
    t.used.p=used; t.rbd.p=rbd; t.status.p=(void*)&closed; t.rx[0].p=rxbuf;
    used[4]=1; used[6]=1;
    iwl_trans_service(); CHECK(t.rx_fragment && !errors);
    /* Simulate write-one-to-clear CSRs between polls. */
    iwl_write32(CSR_INT,0); iwl_write32(CSR_FH_INT,0);
    used[32+4]=1; closed=2;
    struct iwl_rx_packet *rxp=(void*)rxbuf;
    /* Tail bytes can accidentally resemble a valid notification. */
    rxp->len_n_flags=4; rxp->cmd=0x55; rxp->sequence=SEQ_RX_FRAME;
    iwl_trans_service(); CHECK(!t.rx_fragment && !errors && !dispatched);
    iwl_write32(CSR_INT,0); iwl_write32(CSR_FH_INT,0);
    used[64+4]=1; closed=3;
    rxp->len_n_flags=4; rxp->cmd=0x55; rxp->sequence=SEQ_RX_FRAME;
    iwl_trans_service(); CHECK(dispatched==1 && !errors);
    /* A TX queue: the firmware's 8-byte reply carries the queue number and,
     * after the flags, the write pointer. */
    setup(); t.used.p=used; t.rbd.p=rbd; t.status.p=(void*)&closed; closed=0;
    pages_freed=0; memset(reply,0,sizeof(reply)); reply[0]=7; reply[4]=5; reply_len=8;
    CHECK(iwl_trans_alloc_txq(0,15)==0);
    CHECK(t.tx[0].active && t.tx[0].id==7 && t.tx[0].read==5 && t.tx[0].write==5 && t.tx[0].tid==15);
    CHECK(!iwl.dead && !pages_freed);
    CHECK(iwl_trans_alloc_txq(0,16)==-EINVAL);
    /* A reply we cannot read means the firmware may own the queue: quarantine, never free. */
    setup(); t.used.p=used; t.rbd.p=rbd; t.status.p=(void*)&closed; closed=0;
    pages_freed=0; reply_len=12;
    CHECK(iwl_trans_alloc_txq(0,15)<0 && iwl.dead && !pages_freed);
    puts("kernel-iwl-trans: all passed"); return 0;
}
