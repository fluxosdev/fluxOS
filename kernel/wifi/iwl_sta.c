/* AX210 station operations, firmware API 89 / MLD command interface.
 * A single legacy a/g link, no HT aggregation, 6 GHz, or power saving.
 * Wire definitions: iwl_abi.h; upstream reference: Linux v6.12 iwlwifi mvm.
 */
#include "kernel.h"
#include "drivers.h"
#include "proc.h"
#include "iwl.h"
#include "iwl_abi.h"
#include "wifi.h"
#include "ieee80211.h"

#define ADD 1
#define MODIFY 2
#define REMOVE 3
#define MAC_CONFIG 0x08
#define LINK_CONFIG 0x09
#define STA_CONFIG 0x0a
#define STA_REMOVE 0x0c
#define PHY_CONFIG 0x08
#define SCAN_REQ 0x0d
#define SCAN_ABORT 0x0e
#define SESSION_PROTECT 0x05
#define SEC_KEY 0x18
#define RLC_CONFIG 0x08
#define MGMT_TID 15                     /* the queue for management and non-QoS frames */
#define RX_MPDU 0xc1
#define SCAN_COMPLETE 0x0f
#define SCAN_ITER_COMPLETE 0xb5
/* LINK_CONFIG modify_mask bits */
#define LINK_ACTIVE 0x01
#define LINK_RATES 0x02
#define LINK_PROTECTION 0x04
#define LINK_QOS 0x08
#define LINK_BEACON_TIMING 0x10
#define NVM_VALID 1
#define NVM_ACTIVE 8
#define NVM_RADAR 16

extern uint16_t iwl_chan_flags[110];
extern int iwl_have_chan_flags;
int iwl_chan_index(int channel, int band);
int iwl_chan_from_index(int idx, int *band);
int iwl_mvm_start(char *error, uint32_t size);
void iwl_free_file(uint8_t *buf, uint32_t pages);

static struct wifi_hw hw;
static struct {
    int ready, mac, link, phy, sta, active, scanning, protection, failed;
    int link_phy, link_active;          /* the link is bound to the PHY context, and active */
    int txq;
    uint16_t aid, sequence;
    uint32_t scan_uid, scan_deadline;
    struct wifi_bss bss;
    /* Receive PNs belong to a key and TID, including non-QoS (16).
     * Group and pairwise key 0 must have separate replay state. */
    uint64_t rx_pn[5][17];
    uint8_t key_valid[5];
} st = { .txq = -1 };
static spinlock_t sta_lock = SPINLOCK_INIT("iwl-sta");
static uint16_t rd16(const uint8_t *p) { return p[0] | (uint16_t)p[1] << 8; }
static uint32_t rd32(const uint8_t *p) { return rd16(p) | (uint32_t)rd16(p + 2) << 16; }
static int send(uint8_t g, uint8_t c, const void *p, uint32_t n)
{
    int r = iwl_send_cmd(g, c, p, n, NULL, 0, NULL);
    if (r < 0) snprintf(hw.error, sizeof(hw.error), "command %02x:%02x failed (%d)", g, c, r);
    return r;
}
static int mac_config(int action, int associated)
{
    struct iwl_mac_config_cmd c = { 0 };
    c.action = action; c.mac_type = 5; /* FW_MAC_TYPE_BSS_STA */
    memcpy(c.local_mld_addr, iwl.mac, 6);
    c.filter_flags = (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4);
    c.client.is_assoc = associated; c.client.assoc_id = st.aid;
    return send(GRP_MAC_CONF, MAC_CONFIG, &c, sizeof(c));
}
/* The PHY context is bound (phy_id) only while the link is inactive, and
 * the firmware ignores a change of it once active, so as Linux does: bind
 * with an empty mask, then activate, and the reverse on the way down. */
static int link_config(int action, int active, uint32_t mask)
{
    struct iwl_link_config_cmd c = { 0 };
    c.action = action;
    c.phy_id = st.phy ? 0 : 0xffffffff;
    memcpy(c.local_link_addr, iwl.mac, 6);
    c.active = active;
    if (action == MODIFY) c.modify_mask = mask;
    c.cck_rates = st.bss.band == BAND_2GHZ ? 0xf : 0;
    c.ofdm_rates = 0x15; /* mandatory OFDM 6, 12, 24 Mbps */
    c.cck_short_preamble = !!(st.bss.capab & CAP_SHORT_PREAMBLE);
    c.short_slot = !!(st.bss.capab & CAP_SHORT_SLOT);
    for (int i = 0; i < 5; i++) {
        c.ac[i].cw_min = 15; c.ac[i].cw_max = 1023;
        c.ac[i].aifsn = 3; c.ac[i].fifos_mask = 1u << (i == 4 ? 3 : i);
    }
    c.bi = st.bss.beacon_int ? st.bss.beacon_int : 100;
    c.dtim_interval = c.bi * (st.bss.dtim_period ? st.bss.dtim_period : 1);
    return send(GRP_MAC_CONF, LINK_CONFIG, &c, sizeof(c));
}
static int phy_config(int action)
{
    struct iwl_phy_context_cmd c = { 0 };
    c.action = action; c.ci.channel = st.bss.channel;
    c.ci.band = st.bss.band == BAND_2GHZ ? 1 : 0;
    /* API 4 uses a separate RX chain command (RLC_CONFIG v2: phy_id, then
     * rx_chain_info); rxchain_info here is reserved. As Linux, after every
     * add or modify: valid antennas, one static and one MIMO chain. */
    int r = send(GRP_LONG, PHY_CONFIG, &c, sizeof(c));
    if (!r && action != REMOVE && iwl_fw_cmd_ver(&iwl.fw, GRP_DATA_PATH, RLC_CONFIG) >= 2) {
        uint32_t rlc[8] = { 0 };
        rlc[1] = (uint32_t)iwl.rx_ant << 1 | 1u << 10 | 1u << 12;
        r = send(GRP_DATA_PATH, RLC_CONFIG, rlc, sizeof(rlc));
    }
    return r;
}
static int sta_config(void)
{
    struct iwl_sta_cfg_cmd c = { 0 };
    memcpy(c.peer_mld_address, st.bss.bssid, 6);
    memcpy(c.peer_link_address, st.bss.bssid, 6);
    c.assoc_id = st.aid; c.ack_enabled = 1; c.sp_length = 128;
    return send(GRP_MAC_CONF, STA_CONFIG, &c, sizeof(c));
}
static int protect(int action)
{
    struct iwl_mvm_session_prot_cmd c = { 0 };
    c.action = action; c.duration_tu = 10000;
    struct iwl_notif_wait wait;
    uint8_t notification[16];
    if (action == ADD) iwl_notif_arm(&wait, GRP_MAC_CONF, 0xfb, notification, sizeof(notification));
    int r = send(GRP_MAC_CONF, SESSION_PROTECT, &c, sizeof(c));
    if (action != ADD) return r;
    if (r < 0) { iwl_notif_cancel(&wait); return r; }
    /* Accepted: the firmware holds it until told otherwise, whatever the
     * notification says, so leave() must cancel it even if we give up. */
    st.protection = 1;
    r = iwl_notif_wait(&wait, 1000);
    if (r < 0 || wait.len != 16 || !rd32(notification + 4) || !rd32(notification + 8)) return -EIO;
    return 0;
}
static void leave(void)
{
    spin_lock(&sta_lock);
    st.active = 0;
    spin_unlock(&sta_lock);
    if (st.protection && !iwl.dead) protect(REMOVE);
    st.protection = 0;
    if (st.txq >= 0) iwl_trans_free_txq(st.txq);
    st.txq = -1;
    if (st.sta && !iwl.dead) { uint32_t id = 0; send(GRP_MAC_CONF, STA_REMOVE, &id, 4); }
    st.sta = 0;
    if (st.link_active && !iwl.dead) link_config(MODIFY, 0, LINK_ACTIVE);
    st.link_active = 0;
    if (st.phy && !iwl.dead) phy_config(REMOVE);
    st.phy = 0;
    if (st.link_phy && !iwl.dead) link_config(MODIFY, 0, 0);    /* unbind */
    st.link_phy = 0;
    if (st.mac && !iwl.dead) mac_config(MODIFY, 0);
    st.aid = 0;
    memset(st.key_valid, 0, sizeof(st.key_valid));
    memset(st.rx_pn, 0, sizeof(st.rx_pn));
}
static int scan(void)
{
    if (!st.ready || iwl.dead || iwl_trans_rfkill()) return -ENETDOWN;
    if (st.scanning || st.active) return -EBUSY; /* initially scan only while idle */
    static struct iwl_scan_req_umac_v17 c;
    memset(&c, 0, sizeof(c));
    c.uid = st.scan_uid; c.ooc_priority = 6;
    struct iwl_scan_general_params_v11 *g = &c.scan_params.general_params;
    g->flags = (1u << 1) | (1u << 2) | (1u << 6) | (1u << 11);
    g->active_dwell[0] = g->active_dwell[1] = 10;
    g->passive_dwell[0] = g->passive_dwell[1] = 110;
    g->scan_priority = 6;
    for (int i = 0; i < 51; i++) {
        int band, ch = iwl_chan_from_index(i, &band);
        if (ch < 0 || !(iwl_chan_flags[i] & NVM_VALID) ||
            (band == BAND_2GHZ ? !iwl.band_2ghz : !iwl.band_5ghz)) continue;
        unsigned j = c.scan_params.channel_params.count++;
        struct iwl_scan_channel_cfg_umac *cc = &c.scan_params.channel_params.channel_config[j];
        cc->channel_num = ch;
        /* Scan v17 uses PHY_BAND_24=1 / PHY_BAND_5=0, unlike
         * the station layer's BAND_2GHZ=0 / BAND_5GHZ=1. */
        cc->flags = (uint32_t)(band == BAND_2GHZ ? 1 : 0) << 30;
        cc->v5.iter_count = 1;
    }
    if (!c.scan_params.channel_params.count) return -ENODEV;
    c.scan_params.periodic_params.schedule[0].iter_count = 1;
    /* Even passive scans supply a valid probe template; FORCE_PASSIVE
     * prohibits its transmission on channels lacking active permission. */
    struct iwl_scan_probe_req *p = &c.scan_params.probe_params.preq;
    p->mac_header.len = 26;
    p->buf[0] = FC_PROBE_REQ;
    memset(p->buf + 4, 0xff, 6); memcpy(p->buf + 10, iwl.mac, 6); memset(p->buf + 16, 0xff, 6);
    p->band_data[0].offset = 26; p->band_data[0].len = 6;
    memcpy(p->buf + 26, (uint8_t[]){1,4,0x82,0x84,0x8b,0x96}, 6);
    p->band_data[1].offset = 32; p->band_data[1].len = 5;
    memcpy(p->buf + 32, (uint8_t[]){1,3,0x8c,0x98,0xb0}, 5);
    st.scanning = 1; st.scan_deadline = ticks + 15 * HZ;
    int r = send(GRP_LONG, SCAN_REQ, &c, sizeof(c));
    if (r < 0) st.scanning = 0;
    return r;
}
/* A station may join an AP on a passive-only (no initiated radiation) or
 * radar channel, as Linux does: the scan heard the AP there, and the
 * firmware holds transmission on such a channel until it receives on it.
 * In the world domain that is all of 5 GHz and 2.4 GHz channels 12-13. */
static int channel_joinable(int channel, int band)
{
    int idx = iwl_chan_index(channel, band);
    return iwl_have_chan_flags && idx >= 0 && (iwl_chan_flags[idx] & NVM_VALID);
}
static int join(const struct wifi_bss *b)
{
    if (!st.ready || iwl.dead || st.scanning || iwl_trans_rfkill()) return -ENETDOWN;
    if (!channel_joinable(b->channel, b->band)) {
        strlcpy(hw.error, "channel not available in this regulatory domain", sizeof(hw.error));
        return -EINVAL;
    }
    if (st.phy || st.sta) leave();
    st.bss = *b; st.aid = 0;
    int r = phy_config(ADD);
    if (r < 0) goto fail;
    st.phy = 1;
    r = link_config(MODIFY, 0, 0);                              /* bind */
    if (r < 0) goto fail;
    st.link_phy = 1;
    r = link_config(MODIFY, 1, LINK_ACTIVE | LINK_RATES);
    if (r < 0) goto fail;
    st.link_active = 1;
    r = sta_config();
    if (r < 0) goto fail;
    st.sta = 1;
    r = iwl_trans_alloc_txq(0, MGMT_TID);
    if (r < 0) goto fail;
    st.txq = r;
    r = protect(ADD);
    if (r < 0) goto fail;
    st.active = 1;
    return 0;
fail:
    leave(); return r;
}
static int associated(const struct wifi_bss *b, uint16_t aid)
{
    if (!st.active) return -ENETDOWN;
    st.bss = *b; st.aid = aid;
    int r = sta_config();
    if (!r) r = link_config(MODIFY, 1, LINK_RATES | LINK_PROTECTION | LINK_QOS | LINK_BEACON_TIMING);
    if (!r) r = mac_config(MODIFY, 1);
    return r;
}
static int authorized(void)
{
    if (!st.active || iwl.dead) return -ENETDOWN;
    if (st.protection) {
        int r = protect(REMOVE);
        if (r < 0) return r;
        st.protection = 0;
    }
    return 0;
}
static int tx_frame(const uint8_t *frame, uint32_t len, int encrypt)
{
    if (len < HDR_LEN || len > 1800) return -EMSGSIZE;
    uint8_t f[1816], cmd[28] = { 0 };
    spin_lock(&sta_lock);
    if (!st.active || (encrypt && !st.key_valid[0])) { spin_unlock(&sta_lock); return -ENETDOWN; }
    memcpy(f, frame, len);
    uint16_t sequence = (st.sequence++ & 0xfff) << 4;
    memcpy(f + 22, &sequence, 2);
    /* New TX API (AX210): the firmware inserts the CCMP header and MIC
     * itself, from the key's own PN counter, so the frame goes without
     * IV space and the header size excludes it. Only the Protected bit
     * tells it to encrypt; ENCRYPT_DIS (flag bit 1) sends in the clear. */
    if (encrypt) f[1] |= FC_PROTECTED;
    uint16_t n = len, flags = 1 | (encrypt ? 0 : 2); /* explicit rate, optional encryption */
    memcpy(cmd, &n, 2); memcpy(cmd + 2, &flags, 2);
    uint32_t offload = (HDR_LEN / 2) << 8;
    memcpy(cmd + 4, &offload, 4);
    uint32_t ant = (iwl.tx_ant & 1) ? 1 : 2;
    uint32_t rate = (ant << 14) | (st.bss.band == BAND_5GHZ ? 1u << 8 : 0);
    memcpy(cmd + 16, &rate, 4); /* new rate encoding: 1 Mbps CCK / 6 Mbps OFDM */
    int r = iwl_trans_tx(st.txq, cmd, f, HDR_LEN, n);
    spin_unlock(&sta_lock); return r;
}
static int tx_mgmt(const uint8_t *f, uint32_t n) { return tx_frame(f, n, 0); }
static int tx_data(const uint8_t *f, uint32_t n, int encrypt) { return tx_frame(f, n, encrypt); }
static int set_key(int pairwise, int idx, const uint8_t *key, uint32_t len, const uint8_t rsc[8])
{
    if (!st.active || len != 16 || idx < 0 || idx > 3 || (pairwise && idx)) return -EINVAL;
    struct iwl_sec_key_cmd c = { 0 };
    c.action = ADD; c.u.add.sta_mask = 1; c.u.add.key_id = idx;
    c.u.add.key_flags = 2 | (pairwise ? 0 : 0x48); /* CCM, multicast RX only */
    memcpy(c.u.add.key, key, 16);
    uint64_t pn = 0;
    if (rsc) for (int i = 0; i < 6; i++) pn |= (uint64_t)rsc[i] << (8 * i);
    c.u.add.rx_seq = pn;
    int slot = pairwise ? 0 : idx + 1;
    /* Remove a group key before rekeying the same ID. WPA rejects duplicate
     * PTK installation; firmware must not reset an existing TX counter. */
    if (st.key_valid[slot]) {
        struct iwl_sec_key_cmd rm = { 0 };
        if (pairwise) return -EINVAL;
        rm.action = REMOVE; rm.u.remove.sta_mask = 1; rm.u.remove.key_id = idx;
        rm.u.remove.key_flags = c.u.add.key_flags;
        int r = send(GRP_DATA_PATH, SEC_KEY, &rm, sizeof(rm));
        if (r < 0) return r;
        st.key_valid[slot] = 0;
    }
    int r = send(GRP_DATA_PATH, SEC_KEY, &c, sizeof(c));
    memset(&c, 0, sizeof(c));
    if (!r) {
        for (int i = 0; i < 17; i++) st.rx_pn[slot][i] = pn;
        st.key_valid[slot] = 1;
    }
    return r;
}
static void rx_frame(const uint8_t *d, uint32_t size)
{
    /* AX210 RX descriptor v3/5 is 64 bytes. */
    if (size < 64 + HDR_LEN) return;
    uint32_t n = rd16(d), status = rd32(d + 12);
    if (n > size - 64 || n < HDR_LEN || (status & 3) != 3 || (d[3] & 0x40)) return;
    const uint8_t *f = d + 64;
    if ((f[1] & FC_MOREFRAG) || (rd16(f + 22) & 15)) return;
    uint32_t hlen = HDR_LEN;
    unsigned tid = 16;
    if ((f[0] & FC_TYPE_MASK) == FC_TYPE_DATA && (f[0] & FC_QOS_BIT)) {
        if (n < HDR_LEN + 2) return;
        tid = f[HDR_LEN] & 15; hlen += 2;
        if (f[1] & FC_ORDER) hlen += 4;
    }
    unsigned pad = (d[3] & 0x20) ? 2 : 0, tail = (d[2] & 0xf0) >> 3;
    unsigned crypt = 0;
    if (f[1] & FC_PROTECTED) {
        if ((status & 0x700) != 0x200 || !(status & 0x40) || n < hlen + 8) return;
        const uint8_t *iv = f + hlen;
        if (!(iv[3] & 0x20) || memcmp(f + 10, st.bss.bssid, 6)) return;
        int keyidx = iv[3] >> 6;
        int slot = (f[4] & 1) ? keyidx + 1 : 0;
        if (!st.key_valid[slot] || (!(f[4] & 1) && keyidx)) return;
        uint64_t pn = iv[0] | (uint64_t)iv[1] << 8 | (uint64_t)iv[4] << 16 |
                      (uint64_t)iv[5] << 24 | (uint64_t)iv[6] << 32 | (uint64_t)iv[7] << 40;
        if (pn <= st.rx_pn[slot][tid]) return;
        if (n < hlen + 8 + pad + tail) return;
        st.rx_pn[slot][tid] = pn; crypt = 8;
    }
    if (n < hlen + crypt + pad + tail) return;
    uint32_t body = n - hlen - crypt - pad - tail;
    uint8_t clean[2048];
    if (hlen + body > sizeof(clean)) return;
    memcpy(clean, f, hlen); memcpy(clean + hlen, f + hlen + crypt + pad, body);
    int a = d[40] ? d[40] : 127, b = d[41] ? d[41] : 127;
    int channel = d[42], band = channel > 14 ? BAND_5GHZ : BAND_2GHZ;
    wifi_rx(clean, hlen + body, -MIN(a, b), channel, band, crypt != 0);
}
void iwl_sta_rx(const struct iwl_rx_packet *p)
{
    const uint8_t *d = p->data; uint32_t n = iwl_rx_payload_len(p);
    if (p->group > 1) return;
    switch (p->cmd) {
    case RX_MPDU: rx_frame(d, n); break;
    case SCAN_COMPLETE:
        if (st.scanning && n >= 16 && rd32(d) == st.scan_uid) {
            st.scanning = 0; wifi_scan_done();
        }
        break;
    case 0x1c: /* TX_RSP v7: fixed fields, status[frame_count], then SCD SSN */
        if (n >= 48 && d[0] == 1) iwl_trans_reclaim(rd16(d + 36), rd32(d + 44));
        break;
    default: break;
    }
}
static void service(void)
{
    iwl_trans_service();
    if (iwl.dead) {
        if (!st.failed) {
            st.failed = 1; st.active = 0;
            wifi_hw_failed("AX210 stopped; see dmesg");
            iwl_trans_stop();
        }
        return;
    }
    if (st.scanning && (int32_t)(ticks - st.scan_deadline) >= 0) {
        uint32_t abort[2] = { st.scan_uid, 0 };
        send(GRP_LONG, SCAN_ABORT, abort, sizeof(abort));
        st.scanning = 0; wifi_scan_done();
    }
}
static int start(void)
{
    int r = iwl_mvm_start(hw.error, sizeof(hw.error));
    if (r < 0) goto fail;
    if (!iwl_fw_has_capa(&iwl.fw, 110) || iwl_fw_cmd_ver(&iwl.fw, 1, SCAN_REQ) != 17 ||
        iwl_fw_cmd_ver(&iwl.fw, 1, PHY_CONFIG) != 4 ||
        iwl_fw_cmd_ver(&iwl.fw, GRP_DATA_PATH, 0x17) != 3) {
        strlcpy(hw.error, "unsupported AX210 firmware command versions", sizeof(hw.error));
        r = -EINVAL; goto fail;
    }
    memcpy(hw.mac, iwl.mac, 6);
    snprintf(hw.firmware, sizeof(hw.firmware), "%u.%08x.%u", iwl.fw.major, iwl.fw.minor, iwl.fw.local);
    r = mac_config(ADD, 0);
    if (r < 0) goto fail;
    st.mac = 1;
    r = link_config(ADD, 0, 0);
    if (r < 0) goto fail;
    st.link = 1; st.ready = 1;
    return 0;
fail:
    if (!hw.error[0]) snprintf(hw.error, sizeof(hw.error), "AX210 startup failed (%d)", r);
    iwl_trans_stop();
    iwl_free_file(iwl.fw_file, iwl.fw_file_pages);
    iwl.fw_file = NULL; iwl.fw_file_pages = 0;
    return r;
}
int iwl_probe(void)
{
    if (iwl_trans_probe() < 0) return -ENODEV;
    hw.name = "Intel AX210"; hw.start = start; hw.service = service;
    hw.scan = scan; hw.join = join; hw.associated = associated;
    hw.authorized = authorized; hw.leave = leave;
    hw.tx_mgmt = tx_mgmt; hw.tx_data = tx_data; hw.set_key = set_key;
    wifi_register(&hw); return 0;
}
