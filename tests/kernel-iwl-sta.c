#include "kernel-test.h"
#include "../kernel/wifi/iwl_sta.c"
struct iwl_dev iwl;
volatile uint32_t ticks;
uint16_t iwl_chan_flags[110];
int iwl_have_chan_flags;
static int received, reclaimed, sent;
static uint8_t last_frame[2048], last_cmd[28];
static uint32_t last_len;
static struct iwl_scan_req_umac_v17 scan_command;
int iwl_trans_rfkill(void) { return 0; }
int iwl_chan_index(int channel, int band)
{ return band == BAND_2GHZ ? channel - 1 : channel == 36 ? 14 : channel == 52 ? 18 : -1; }
int iwl_chan_from_index(int idx, int *band)
{ *band = idx < 14 ? BAND_2GHZ : BAND_5GHZ; return idx < 14 ? idx + 1 : 36; }
/* Commands other than scans are logged as group:cmd, and LINK_CONFIG
 * also as its phy_id, active flag and modify mask. */
static struct { uint8_t g, c; uint32_t phy, active, mask; } cmds[16];
static int ncmds;
int iwl_send_cmd(uint8_t g, uint8_t c, const void *p, uint32_t n,
                 void *resp, uint32_t size, uint32_t *got)
{
    if (g == GRP_LONG && c == SCAN_REQ) {
        CHECK(n == sizeof(scan_command));
        memcpy(&scan_command, p, n); return 0;
    }
    CHECK(ncmds < 16);
    cmds[ncmds].g = g; cmds[ncmds].c = c;
    if (g == GRP_MAC_CONF && c == LINK_CONFIG) {
        const struct iwl_link_config_cmd *l = p;
        cmds[ncmds].phy = l->phy_id; cmds[ncmds].active = l->active;
        cmds[ncmds].mask = l->modify_mask;
    }
    ncmds++; return 0;
}
int iwl_fw_cmd_ver(const struct iwl_fw *fw, uint8_t g, uint8_t c) { (void)fw; return 3; }
static int queue_tid = -1;
int iwl_trans_alloc_txq(int sta_id, int tid) { queue_tid = tid; return 1; }
void iwl_trans_free_txq(int txq) {}
static struct iwl_notif_wait *armed;
void iwl_notif_arm(struct iwl_notif_wait *w, uint8_t group, uint8_t cmd, void *buf, uint32_t size)
{ w->buf = buf; w->size = size; armed = w; }
int iwl_notif_wait(struct iwl_notif_wait *w, uint32_t timeout_ms)
{   /* session protection started: id and status nonzero */
    memset(w->buf, 0, 16); w->buf[4] = w->buf[8] = 1; w->len = 16; return 0;
}
void iwl_notif_cancel(struct iwl_notif_wait *w) {}
static int is_link(int i, uint32_t phy, uint32_t active, uint32_t mask)
{
    return cmds[i].g == GRP_MAC_CONF && cmds[i].c == LINK_CONFIG && cmds[i].phy == phy &&
           cmds[i].active == active && cmds[i].mask == mask;
}
static void join_test(void)
{
    struct wifi_bss b = { 0 };
    b.channel = 52; b.band = BAND_5GHZ;
    ncmds = 0;
    CHECK(join(&b) == 0);
    /* PHY context and its RX chains, then bind the link, then activate. */
    CHECK(ncmds >= 5 && cmds[0].g == GRP_LONG && cmds[0].c == PHY_CONFIG);
    CHECK(cmds[1].g == GRP_DATA_PATH && cmds[1].c == RLC_CONFIG);
    CHECK(is_link(2, 0, 0, 0));
    CHECK(is_link(3, 0, 1, LINK_ACTIVE | LINK_RATES));
    CHECK(queue_tid == MGMT_TID);     /* the firmware asserts on TID 16 */
    ncmds = 0;
    leave();
    /* Deactivate, remove the PHY context, then unbind the link. */
    int deact = -1, phy_rm = -1, unbind = -1;
    for (int i = 0; i < ncmds; i++) {
        if (is_link(i, 0, 0, LINK_ACTIVE)) deact = i;
        if (cmds[i].g == GRP_LONG && cmds[i].c == PHY_CONFIG) phy_rm = i;
        if (is_link(i, 0xffffffff, 0, 0)) unbind = i;
    }
    CHECK(deact >= 0 && phy_rm > deact && unbind > phy_rm);
    CHECK(!st.link_phy && !st.link_active && !st.phy);
}
void klog(const char *fmt, ...) { (void)fmt; }
void sleep_on(void *p) { (void)p; abort(); }
void wifi_rx(const uint8_t *f, uint32_t n, int rssi, int ch, int band, int dec)
{ received++; CHECK(n >= 24); CHECK(ch == 1); CHECK(rssi == -40); memcpy(last_frame,f,n); last_len=n; }
void wifi_scan_done(void) {}
void iwl_trans_reclaim(int q, uint16_t ssn) { CHECK(q == 3 && ssn == 9); reclaimed++; }
int iwl_trans_tx(int q, const void *c, const uint8_t *f, uint32_t hlen, uint32_t n)
{ CHECK(q == 1); memcpy(last_cmd,c,28); memcpy(last_frame,f,n); last_len=n; sent++; return 0; }
static void rx_test(void)
{
    uint8_t buf[128] = {0};
    buf[0]=32; buf[12]=3; buf[40]=40; buf[41]=50; buf[42]=1;
    uint8_t *f=buf+64; f[0]=8;
    received=0;
    rx_frame(buf,95); CHECK(received==0); /* MPDU extends past payload */
    rx_frame(buf,96); CHECK(received==1 && last_len==32);
    buf[12]=1; rx_frame(buf,96); CHECK(received==1); /* overrun */
    buf[12]=3; f[1]=FC_MOREFRAG; rx_frame(buf,96); CHECK(received==1);
    f[1]=FC_PROTECTED; buf[0]=40; buf[13]=2; buf[12]=0x43;
    memcpy(st.bss.bssid,"ABCDEF",6); memcpy(f+10,"ABCDEF",6);
    f[24]=1; f[27]=0x20;
    st.key_valid[0]=1;
    rx_frame(buf,104); CHECK(received==2 && last_len==32);
    rx_frame(buf,104); CHECK(received==2); /* replay */
    f[24]=2; buf[12]=3; rx_frame(buf,104); CHECK(received==2); /* bad MIC */
    buf[12]=0x43; rx_frame(buf,104); CHECK(received==3);
    f[24]=3; buf[0]=26; rx_frame(buf,90); CHECK(received==3); /* short IV */
}
static void tx_test(void)
{
    uint8_t f[32]={8};
    st.active=1; st.txq=1; iwl.tx_ant=1;
    CHECK(tx_frame(f,23,0)==-EMSGSIZE);
    CHECK(tx_frame(f,32,0)==0 && sent==1 && last_len==32);
    CHECK(rd16(last_cmd+2)==3);
    st.key_valid[0]=0; CHECK(tx_frame(f,32,1)==-ENETDOWN);
    /* New TX API: the firmware inserts the CCMP header itself. An encrypted
     * frame must go out at its own length with the body right after the
     * MAC header, only the Protected bit set, and MH_SIZE of 24 bytes. */
    f[HDR_LEN]=0x5a;
    st.key_valid[0]=1; CHECK(tx_frame(f,32,1)==0 && sent==2 && last_len==32);
    CHECK(last_frame[1]&FC_PROTECTED); CHECK(last_frame[HDR_LEN]==0x5a);
    CHECK(rd16(last_cmd)==32 && rd16(last_cmd+2)==1);
    CHECK(rd32(last_cmd+4)==((HDR_LEN/2)<<8));
}
int main(void)
{
    st.ready=1; iwl.band_2ghz=iwl.band_5ghz=1;
    iwl_chan_flags[0]=iwl_chan_flags[14]=NVM_VALID;
    CHECK(scan()==0);
    CHECK(scan_command.scan_params.channel_params.count==2);
    CHECK(scan_command.scan_params.channel_params.channel_config[0].channel_num==1);
    CHECK(scan_command.scan_params.channel_params.channel_config[0].flags==(1u<<30));
    CHECK(scan_command.scan_params.channel_params.channel_config[1].channel_num==36);
    CHECK(scan_command.scan_params.channel_params.channel_config[1].flags==0);
    CHECK(scan()==-EBUSY);
    st.scanning=0;
    rx_test(); tx_test();
    /* Passive-only and radar channels can be joined; invalid ones cannot. */
    iwl_have_chan_flags=1; iwl_chan_flags[18]=NVM_VALID|NVM_RADAR;
    CHECK(channel_joinable(1,BAND_2GHZ) && channel_joinable(36,BAND_5GHZ));
    CHECK(channel_joinable(52,BAND_5GHZ));
    CHECK(!channel_joinable(2,BAND_2GHZ) && !channel_joinable(165,BAND_5GHZ));
    st.mac=st.link=1; join_test();
    iwl_have_chan_flags=0; CHECK(!channel_joinable(1,BAND_2GHZ));
    uint8_t packet[64]={0}; struct iwl_rx_packet *p=(void*)packet;
    p->cmd=0x1c; p->len_n_flags=4+47; p->data[0]=1;
    iwl_sta_rx(p); CHECK(reclaimed==0);
    p->len_n_flags=4+48; p->data[36]=3; p->data[44]=9;
    iwl_sta_rx(p); CHECK(reclaimed==1);
    puts("kernel-iwl-sta: all passed"); return 0;
}
