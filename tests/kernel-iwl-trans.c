#include "kernel-test.h"
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
static void setup(void)
{
    memset(&t,0,sizeof(t)); memset(&iwl,0,sizeof(iwl));
    memset(regs,0,sizeof(regs)); iwl.regs=regs;
    t.running=1; t.cmd.desc.p=descriptors; t.cmd.buffers.p=buffers; t.cmd.bc.p=bc;
    errors=dispatched=0;
}
int main(void)
{
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
    puts("kernel-iwl-trans: all passed"); return 0;
}
