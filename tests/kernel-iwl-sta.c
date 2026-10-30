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
int iwl_chan_from_index(int idx, int *band)
{ *band = idx < 14 ? BAND_2GHZ : BAND_5GHZ; return idx < 14 ? idx + 1 : 36; }
int iwl_send_cmd(uint8_t g, uint8_t c, const void *p, uint32_t n,
                 void *resp, uint32_t size, uint32_t *got)
{
    CHECK(g == GRP_LONG && c == SCAN_REQ && n == sizeof(scan_command));
    memcpy(&scan_command, p, n); return 0;
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
    st.key_valid[0]=1; CHECK(tx_frame(f,32,1)==0 && sent==2 && last_len==40);
    CHECK(last_frame[1]&FC_PROTECTED); CHECK(last_frame[27]==0x20);
    CHECK(rd16(last_cmd)==40 && rd16(last_cmd+2)==1);
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
    uint8_t packet[64]={0}; struct iwl_rx_packet *p=(void*)packet;
    p->cmd=0x1c; p->len_n_flags=4+47; p->data[0]=1;
    iwl_sta_rx(p); CHECK(reclaimed==0);
    p->len_n_flags=4+48; p->data[36]=3; p->data[44]=9;
    iwl_sta_rx(p); CHECK(reclaimed==1);
    puts("kernel-iwl-sta: all passed"); return 0;
}
