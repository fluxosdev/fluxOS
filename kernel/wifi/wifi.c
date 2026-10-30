/*
 * The wireless station: scanning, joining a network (open, or WPA2 with
 * a pre-shared key), staying joined, and the network interface over it.
 * See wifi.h for how it divides the work with a card driver.
 *
 * A connection: find the network (from recent scan results, or scan),
 * pick its strongest access point, send Open System authentication and
 * an association request built here, then run the 4-way handshake
 * (wpa.c) on the EAPOL frames that follow and install the keys it
 * yields. Only then is the link up, which starts DHCP. A lost or
 * refused connection is tried again, sooner at first and then less
 * often, until SYS_wifi WIFI_DISCONNECT; a handshake that keeps failing
 * (most likely a wrong password) is given up after three tries.
 *
 * We join as a plain 802.11a/g station (no HT, no QoS): slower than the
 * card can go, but every access point but the strictest accepts it, and
 * there are no block-ack sessions to keep.
 */
#include "kernel.h"
#include "drivers.h"
#include "lock.h"
#include "mm.h"
#include "net.h"
#include "proc.h"
#include "syscall.h"
#include "ieee80211.h"
#include "wifi.h"

#define MAX_BSS        32
#define BSS_FRESH      (30 * HZ)        /* scan results this young are used without a new scan */
#define BSS_EXPIRE     (120 * HZ)
#define SCAN_TIMEOUT   (15 * HZ)
#define FRAME_TIMEOUT  (HZ * 3 / 10)    /* an auth or assoc response */
#define FRAME_TRIES    3
#define EAPOL_TIMEOUT  (3 * HZ)         /* each handshake message */
#define RXQ_LEN        8
#define RXQ_FRAME      512              /* management and EAPOL frames we keep */
#define LISTEN_INTERVAL 10
#define HANDSHAKE_FAILS_MAX 3

enum mlme_state {
    M_OFF,                              /* no card, or it is starting or failed */
    M_IDLE,
    M_SCAN,                             /* a scan someone asked for */
    M_SCAN_CONNECT,                     /* a scan to find the network to join */
    M_AUTH,
    M_ASSOC,
    M_HANDSHAKE,
    M_CONNECTED,
};

static struct {
    struct wifi_hw *hw;
    int status;                         /* WIFI_ST_* for the starting/failed part */
    enum mlme_state state;
    uint32_t scans;
    struct wifi_bss bss[MAX_BSS];

    /* the network we want, from WIFI_CONNECT */
    int want;
    struct wifi_connect target;
    /* the access point being joined */
    struct wifi_bss cur;
    uint32_t deadline;                  /* for the current step */
    int tries;
    uint32_t retry_at;                  /* reconnect after a failure */
    uint32_t backoff;                   /* seconds */
    int handshake_fails;
    int sent_msg2;                      /* this attempt reached message 2 */
    uint16_t aid;
    struct wpa_sm wpa;
    uint8_t own_ie[WPA_IE_MAX];
    uint32_t own_ie_len;
    char error[64];

    /* requests from SYS_wifi, taken by the thread */
    volatile int req_scan, req_connect, req_disconnect;
    struct wifi_connect req_target;

    /* frames for the thread (under lock) */
    struct { uint8_t data[RXQ_FRAME]; uint32_t len; } rxq[RXQ_LEN];
    uint32_t rxq_head, rxq_tail;
    volatile int events;

    /* what the transmit path needs while connected (read with interrupts off) */
    volatile int link_up;
    volatile int encrypt;
    uint8_t tx_bssid[6];
} w;

static spinlock_t wifi_lock = SPINLOCK_INIT("wifi");

static const uint8_t rates_2ghz[] = { 2, 4, 11, 22, 12, 18, 24, 36, 48, 72, 96, 108 };
static const uint8_t rates_5ghz[] = { 12, 18, 24, 36, 48, 72, 96, 108 };
static const uint8_t snap_hdr[6] = { 0xAA, 0xAA, 0x03, 0x00, 0x00, 0x00 };

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put_le16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

uint32_t wifi_channel_freq(int channel, int band)
{
    if (band == BAND_2GHZ)
        return channel == 14 ? 2484 : 2407 + 5 * (uint32_t)channel;
    return 5000 + 5 * (uint32_t)channel;
}

static void fmt_ssid(char *out, const uint8_t *ssid, uint32_t len)
{
    uint32_t i;
    for (i = 0; i < len && i < WIFI_SSID_MAX; i++)
        out[i] = ssid[i] >= 0x20 && ssid[i] < 0x7F ? (char)ssid[i] : '?';
    out[i] = 0;
}

static void set_error(const char *msg)
{
    strlcpy(w.error, msg, sizeof(w.error));
}

void wifi_wake(void)
{
    w.events = 1;
    wakeup(&w);
}

/* ---- scan results ---- */

static const uint8_t *find_ie(const uint8_t *ies, uint32_t len, uint8_t id)
{
    const uint8_t *p = ies, *end = ies + len;
    while (end - p >= 2 && end - p >= 2 + p[1]) {
        if (p[0] == id)
            return p;
        p += 2 + p[1];
    }
    return NULL;
}

/* A beacon or probe response: note (or refresh) the access point. */
static void bss_heard(const uint8_t *f, uint32_t len, int rssi, int rx_channel, int band)
{
    if (len < HDR_LEN + 12)
        return;
    const struct ieee80211_hdr *h = (const void *)f;
    const uint8_t *body = f + HDR_LEN, *ies = body + 12;
    uint32_t ies_len = len - HDR_LEN - 12;
    const uint8_t *ssid = find_ie(ies, ies_len, IE_SSID);
    if (!ssid || ssid[1] > WIFI_SSID_MAX)
        return;
    int channel = rx_channel;
    const uint8_t *ds = find_ie(ies, ies_len, IE_DS_PARAM);
    const uint8_t *ht = find_ie(ies, ies_len, IE_HT_OPER);
    if (ds && ds[1] >= 1)
        channel = ds[2];
    else if (ht && ht[1] >= 1)
        channel = ht[2];
    if (channel != rx_channel && rx_channel)
        return;                         /* heard on a neighbouring channel */

    spin_lock(&wifi_lock);
    struct wifi_bss *b = NULL, *oldest = &w.bss[0];
    for (int i = 0; i < MAX_BSS; i++) {
        if (w.bss[i].seen && !memcmp(w.bss[i].bssid, h->addr3, 6)) {
            b = &w.bss[i];
            break;
        }
        if (!w.bss[i].seen || (int32_t)(w.bss[i].seen - oldest->seen) < 0)
            oldest = &w.bss[i];
    }
    if (!b) {
        b = oldest;
        memset(b, 0, sizeof(*b));
        memcpy(b->bssid, h->addr3, 6);
    }
    /* A hidden network's beacons carry an empty SSID: keep a name a probe response gave. */
    if (ssid[1] && !(ssid[1] == 1 && ssid[2] == 0)) {
        memcpy(b->ssid, ssid + 2, ssid[1]);
        b->ssid_len = ssid[1];
    }
    b->channel = (uint8_t)channel;
    b->band = (uint8_t)band;
    b->rssi = (int8_t)rssi;
    b->beacon_int = le16(body + 8);
    b->capab = le16(body + 10);
    const uint8_t *tim = find_ie(ies, ies_len, IE_TIM);
    b->dtim_period = tim && tim[1] >= 2 && tim[3] ? tim[3] : 1;
    b->nrates = 0;
    for (int id = IE_RATES; id; id = id == IE_RATES ? IE_EXT_RATES : 0) {
        const uint8_t *r = find_ie(ies, ies_len, (uint8_t)id);
        for (int i = 0; r && i < r[1] && b->nrates < WIFI_RATES_MAX; i++)
            b->rates[b->nrates++] = r[2 + i];
    }
    const uint8_t *rsn = find_ie(ies, ies_len, IE_RSN);
    b->rsn_len = rsn && rsn[1] + 2u <= WPA_IE_MAX ? (uint8_t)(rsn[1] + 2) : 0;
    if (b->rsn_len)
        memcpy(b->rsn, rsn, b->rsn_len);
    b->security = (uint8_t)rsn_security(b->rsn_len ? b->rsn : NULL, b->rsn_len,
                                        (b->capab & CAP_PRIVACY) != 0);
    b->seen = ticks ? ticks : 1;
    spin_unlock(&wifi_lock);
}

/* The strongest access point of the wanted network we can join. */
static int pick_bss(struct wifi_bss *out)
{
    static const uint8_t any[6];
    int found = 0;
    spin_lock(&wifi_lock);
    for (int i = 0; i < MAX_BSS; i++) {
        struct wifi_bss *b = &w.bss[i];
        if (!b->seen || (int32_t)(ticks - b->seen) > BSS_FRESH)
            continue;
        if (b->ssid_len != w.target.ssid_len || memcmp(b->ssid, w.target.ssid, b->ssid_len))
            continue;
        if (memcmp(w.target.bssid, any, 6) && memcmp(w.target.bssid, b->bssid, 6))
            continue;
        if (b->security != (w.target.has_psk ? SEC_WPA2_PSK : SEC_OPEN))
            continue;
        if (!found || b->rssi > out->rssi) {
            *out = *b;
            found = 1;
        }
    }
    spin_unlock(&wifi_lock);
    return found;
}

/* Why the wanted network cannot be joined, if it was heard at all. */
static const char *why_not_joinable(void)
{
    const char *why = "network not found";
    spin_lock(&wifi_lock);
    for (int i = 0; i < MAX_BSS; i++) {
        struct wifi_bss *b = &w.bss[i];
        if (!b->seen || b->ssid_len != w.target.ssid_len ||
            memcmp(b->ssid, w.target.ssid, b->ssid_len))
            continue;
        if (b->security == SEC_OTHER)
            why = "network uses security we do not support (WPA3, Enterprise, WEP...)";
        else if (b->security == SEC_WPA2_PSK && !w.target.has_psk)
            why = "network needs a password";
        else if (b->security == SEC_OPEN && w.target.has_psk)
            why = "network is open: connect without a password";
    }
    spin_unlock(&wifi_lock);
    return why;
}

/* ---- frames we send ---- */

static uint32_t mgmt_hdr(uint8_t *f, uint8_t subtype, const uint8_t *bssid)
{
    memset(f, 0, HDR_LEN);
    struct ieee80211_hdr *h = (void *)f;
    h->fc[0] = FC_TYPE_MGMT | subtype;
    memcpy(h->addr1, bssid, 6);
    memcpy(h->addr2, w.hw->mac, 6);
    memcpy(h->addr3, bssid, 6);
    return HDR_LEN;
}

static int send_auth(void)
{
    uint8_t f[HDR_LEN + 6];
    uint32_t n = mgmt_hdr(f, FC_AUTH, w.cur.bssid);
    put_le16(f + n, 0);                 /* Open System */
    put_le16(f + n + 2, 1);             /* transaction 1 */
    put_le16(f + n + 4, 0);
    return w.hw->tx_mgmt(f, n + 6);
}

static int send_deauth(uint16_t reason)
{
    uint8_t f[HDR_LEN + 2];
    uint32_t n = mgmt_hdr(f, FC_DEAUTH, w.cur.bssid);
    put_le16(f + n, reason);
    return w.hw->tx_mgmt(f, n + 2);
}

static int network_requires(uint8_t rate)
{
    for (int i = 0; i < w.cur.nrates; i++)
        if ((w.cur.rates[i] & 0x7F) == rate)
            return w.cur.rates[i] & RATE_BASIC;
    return 0;
}

static int send_assoc(void)
{
    uint8_t f[HDR_LEN + 4 + 2 + WIFI_SSID_MAX + 2 + 8 + 2 + 8 + WPA_IE_MAX];
    uint32_t n = mgmt_hdr(f, FC_ASSOC_REQ, w.cur.bssid);
    uint16_t capab = CAP_ESS | (w.cur.capab & CAP_SHORT_PREAMBLE);
    if (w.cur.band == BAND_2GHZ)
        capab |= w.cur.capab & CAP_SHORT_SLOT;
    put_le16(f + n, capab);
    put_le16(f + n + 2, LISTEN_INTERVAL);
    n += 4;
    f[n++] = IE_SSID;
    f[n++] = w.cur.ssid_len;
    memcpy(f + n, w.cur.ssid, w.cur.ssid_len);
    n += w.cur.ssid_len;
    const uint8_t *rates = w.cur.band == BAND_2GHZ ? rates_2ghz : rates_5ghz;
    uint32_t nrates = w.cur.band == BAND_2GHZ ? sizeof(rates_2ghz) : sizeof(rates_5ghz);
    for (uint32_t start = 0; start < nrates; start += 8) {
        uint32_t cnt = MIN(nrates - start, 8u);
        f[n++] = start ? IE_EXT_RATES : IE_RATES;
        f[n++] = (uint8_t)cnt;
        for (uint32_t i = 0; i < cnt; i++) {
            uint8_t r = rates[start + i];
            f[n++] = r | (network_requires(r) ? RATE_BASIC : 0);
        }
    }
    if (w.target.has_psk) {
        memcpy(f + n, w.own_ie, w.own_ie_len);
        n += w.own_ie_len;
    }
    return w.hw->tx_mgmt(f, n);
}

/* An 802.11 data frame to the AP carrying an Ethernet payload. */
static int send_data(const uint8_t *dst, const uint8_t *src, uint16_t type, const uint8_t *payload,
                     uint32_t len, int encrypt)
{
    uint8_t f[HDR_LEN + LLC_SNAP_LEN + ETH_MTU];
    if (len > ETH_MTU)
        return -EMSGSIZE;
    struct ieee80211_hdr *h = (void *)f;
    memset(h, 0, HDR_LEN);
    h->fc[0] = FC_TYPE_DATA;
    h->fc[1] = FC_TODS;
    memcpy(h->addr1, w.tx_bssid, 6);
    memcpy(h->addr2, src, 6);
    memcpy(h->addr3, dst, 6);
    memcpy(f + HDR_LEN, snap_hdr, 6);
    f[HDR_LEN + 6] = (uint8_t)(type >> 8);
    f[HDR_LEN + 7] = (uint8_t)type;
    memcpy(f + HDR_LEN + LLC_SNAP_LEN, payload, len);
    return w.hw->tx_data(f, HDR_LEN + LLC_SNAP_LEN + len, encrypt);
}

static int send_eapol(const uint8_t *frame, uint32_t len)
{
    return send_data(w.cur.bssid, w.hw->mac, ETH_P_EAPOL, frame, len, w.encrypt);
}

/* The network stack's transmit hook: an Ethernet frame, net_lock held. */
static int netdev_transmit(const void *frame, uint32_t len)
{
    const uint8_t *e = frame;
    if (!w.link_up)
        return -ENETDOWN;
    if (len < ETH_HLEN)
        return -EINVAL;
    return send_data(e, e + 6, (uint16_t)(e[12] << 8 | e[13]), e + ETH_HLEN, len - ETH_HLEN,
                     w.encrypt);
}

/* ---- frames we receive ---- */

static void queue_frame(const uint8_t *f, uint32_t len)
{
    spin_lock(&wifi_lock);
    if (w.rxq_head - w.rxq_tail < RXQ_LEN) {
        uint32_t n = MIN(len, (uint32_t)RXQ_FRAME);
        memcpy(w.rxq[w.rxq_head % RXQ_LEN].data, f, n);
        w.rxq[w.rxq_head % RXQ_LEN].len = n;
        w.rxq_head++;
    }
    spin_unlock(&wifi_lock);
    wifi_wake();
}

/* EAPOL frames are wrapped in a fake 802.11 header-less record: tag them. */
#define EAPOL_TAG 0xFF

void wifi_rx(const uint8_t *f, uint32_t len, int rssi, int channel, int band, int decrypted)
{
    if (len < HDR_LEN || !w.hw)
        return;
    const struct ieee80211_hdr *h = (const void *)f;
    uint8_t type = h->fc[0] & FC_TYPE_MASK, stype = h->fc[0] & FC_STYPE_MASK;

    if (type == FC_TYPE_MGMT) {
        if (stype == FC_BEACON || stype == FC_PROBE_RESP) {
            bss_heard(f, len, rssi, channel, band);
            return;
        }
        if (memcmp(h->addr1, w.hw->mac, 6))
            return;                     /* not addressed to us */
        if (stype == FC_AUTH || stype == FC_ASSOC_RESP || stype == FC_REASSOC_RESP ||
            stype == FC_DEAUTH || stype == FC_DISASSOC)
            queue_frame(f, len);
        return;
    }
    if (type != FC_TYPE_DATA || (stype & FC_NULLFUNC_BIT))
        return;
    if ((h->fc[1] & (FC_TODS | FC_FROMDS)) != FC_FROMDS || !w.cur.seen ||
        memcmp(h->addr2, w.cur.bssid, 6))
        return;                         /* only from our access point */
    if (!(h->addr1[0] & 1) && memcmp(h->addr1, w.hw->mac, 6))
        return;
    uint32_t hlen = HDR_LEN;
    if (stype & FC_QOS_BIT)
        hlen += QOS_CTL_LEN + ((h->fc[1] & FC_ORDER) ? HT_CTL_LEN : 0);
    int prot = (h->fc[1] & FC_PROTECTED) != 0;
    if (prot && !decrypted)
        return;                         /* we cannot decrypt it ourselves */
    if (len < hlen + LLC_SNAP_LEN || memcmp(f + hlen, snap_hdr, 3))
        return;
    uint16_t etype = (uint16_t)(f[hlen + 6] << 8 | f[hlen + 7]);
    const uint8_t *payload = f + hlen + LLC_SNAP_LEN;
    uint32_t plen = len - hlen - LLC_SNAP_LEN;

    if (etype == ETH_P_EAPOL) {
        uint8_t tagged[RXQ_FRAME];
        uint32_t n = MIN(plen, (uint32_t)RXQ_FRAME - 1);
        tagged[0] = EAPOL_TAG;
        memcpy(tagged + 1, payload, n);
        queue_frame(tagged, n + 1);
        return;
    }
    if (!w.link_up || (w.encrypt && !prot) || plen > ETH_MTU)
        return;                         /* once keys are in, only protected data */
    uint8_t e[ETH_FRAME_MAX];
    memcpy(e, h->addr1, 6);             /* DA */
    memcpy(e + 6, h->addr3, 6);         /* SA */
    e[12] = (uint8_t)(etype >> 8);
    e[13] = (uint8_t)etype;
    memcpy(e + ETH_HLEN, payload, plen);
    spin_lock(&net_lock);
    net_receive(e, ETH_HLEN + plen);
    spin_unlock(&net_lock);
}

void wifi_scan_done(void)
{
    w.scans++;
    if (w.state == M_SCAN)
        w.state = M_IDLE;
    else if (w.state == M_SCAN_CONNECT)
        w.deadline = ticks;             /* look at the results now */
    wifi_wake();
}

void wifi_hw_failed(const char *why)
{
    klog("wifi: %s\n", why);
    strlcpy(w.error, why, sizeof(w.error));
    w.link_up = 0;
    w.status = WIFI_ST_FAILED;
    w.state = M_OFF;
    spin_lock(&net_lock);
    net_link_changed(0);
    spin_unlock(&net_lock);
}

/* ---- the connection ---- */

static void link_down(void)
{
    if (!w.link_up)
        return;
    w.link_up = 0;
    spin_lock(&net_lock);
    net_link_changed(0);
    spin_unlock(&net_lock);
}

/* Give up this attempt (or connection); try again later if still wanted. */
static void fail(const char *why)
{
    /* A synchronous driver call may already have reported a fatal error.
     * Do not turn M_OFF back into M_IDLE and retry a crashed device. */
    if (w.state == M_OFF)
        return;
    char ssid[WIFI_SSID_MAX + 1];
    fmt_ssid(ssid, (const uint8_t *)w.target.ssid, w.target.ssid_len);
    klog("wifi: %s: %s\n", ssid, why);
    set_error(why);
    link_down();
    w.encrypt = 0;
    if (w.state >= M_AUTH)
        w.hw->leave();
    memset(&w.cur, 0, sizeof(w.cur));
    w.state = M_IDLE;
    if (!w.want)
        return;
    if (w.handshake_fails >= HANDSHAKE_FAILS_MAX) {
        klog("wifi: %s: giving up (wrong password?)\n", ssid);
        set_error("key handshake failed repeatedly: wrong password?");
        w.want = 0;
        return;
    }
    w.backoff = w.backoff ? MIN(w.backoff * 2, 60u) : 2;
    w.retry_at = ticks + w.backoff * HZ;
}

static void start_join(void)
{
    char ssid[WIFI_SSID_MAX + 1];
    fmt_ssid(ssid, w.cur.ssid, w.cur.ssid_len);
    klog("wifi: joining %s (%02x:%02x:%02x:%02x:%02x:%02x) on channel %u, %d dBm\n", ssid,
         w.cur.bssid[0], w.cur.bssid[1], w.cur.bssid[2], w.cur.bssid[3], w.cur.bssid[4],
         w.cur.bssid[5], w.cur.channel, w.cur.rssi);
    memcpy(w.tx_bssid, w.cur.bssid, 6);
    w.encrypt = 0;
    w.sent_msg2 = 0;
    if (w.hw->join(&w.cur) < 0) {
        fail(w.hw->error[0] ? w.hw->error : "the card would not join the network");
        return;
    }
    w.state = M_AUTH;
    w.tries = 0;
    w.deadline = ticks;                 /* send the first request now */
}

/* Look for the wanted network: use what recent scans heard, or scan. */
static void connect_step(int scanned)
{
    if (pick_bss(&w.cur)) {
        start_join();
        return;
    }
    if (scanned) {
        fail(why_not_joinable());
        return;
    }
    if (w.hw->scan() < 0) {
        fail("scan failed");
        return;
    }
    w.state = M_SCAN_CONNECT;
    w.deadline = ticks + SCAN_TIMEOUT;
}

static void connected(void)
{
    char ssid[WIFI_SSID_MAX + 1];
    fmt_ssid(ssid, w.cur.ssid, w.cur.ssid_len);
    if (w.hw->authorized() < 0) {
        fail("the card would not open the connection");
        return;
    }
    klog("wifi: connected to %s\n", ssid);
    w.state = M_CONNECTED;
    w.error[0] = 0;
    w.backoff = 0;
    w.handshake_fails = 0;
    w.link_up = 1;
    spin_lock(&net_lock);
    net_link_changed(1);
    spin_unlock(&net_lock);
}

static void handle_eapol(const uint8_t *frame, uint32_t len)
{
    if (w.state != M_HANDSHAKE && w.state != M_CONNECTED)
        return;
    struct wpa_result res;
    int r = wpa_rx(&w.wpa, frame, len, &res);
    if (r < 0) {
        klog("wifi: EAPOL frame dropped: %s\n", wpa_strerror(r));
        return;
    }
    if (res.reply_len) {
        if (send_eapol(res.reply, res.reply_len) < 0)
            klog("wifi: could not send an EAPOL reply\n");
        if (w.wpa.state == WPA_WAIT_MSG3) {
            w.sent_msg2 = 1;            /* message 2 */
            w.deadline = ticks + EAPOL_TIMEOUT;
        }
    }
    if (res.install_ptk) {
        if (w.hw->set_key(1, 0, WPA_TK(&w.wpa), WPA_TK_LEN, NULL) < 0) {
            fail("could not install the pairwise key");
            return;
        }
        w.encrypt = 1;
    }
    if (res.install_gtk && w.hw->set_key(0, res.gtk_idx, res.gtk, res.gtk_len, res.gtk_rsc) < 0) {
        fail("could not install the group key");
        return;
    }
    if (res.install_gtk && w.state == M_CONNECTED)
        klog("wifi: new group key %d\n", res.gtk_idx);
    if (res.connected)
        connected();
}

static void handle_mgmt(const uint8_t *f, uint32_t len)
{
    const struct ieee80211_hdr *h = (const void *)f;
    uint8_t stype = h->fc[0] & FC_STYPE_MASK;
    const uint8_t *body = f + HDR_LEN;
    uint32_t blen = len - HDR_LEN;
    if (!w.cur.seen || memcmp(h->addr2, w.cur.bssid, 6))
        return;
    if ((stype == FC_DEAUTH || stype == FC_DISASSOC) && blen >= 2) {
        if (w.state < M_AUTH)
            return;
        char why[64];
        snprintf(why, sizeof(why), "%s by the access point (reason %u)",
                 stype == FC_DEAUTH ? "deauthenticated" : "disassociated", le16(body));
        if (w.state == M_HANDSHAKE && w.sent_msg2)
            w.handshake_fails++;        /* thrown out after our message 2: the password */
        fail(why);
        return;
    }
    if (stype == FC_AUTH && w.state == M_AUTH && blen >= 6) {
        if (le16(body) != 0 || le16(body + 2) != 2)
            return;
        uint16_t status = le16(body + 4);
        if (status != STATUS_SUCCESS) {
            char why[48];
            snprintf(why, sizeof(why), "authentication refused (status %u)", status);
            fail(why);
            return;
        }
        w.state = M_ASSOC;
        w.tries = 0;
        w.deadline = ticks;
        return;
    }
    if ((stype == FC_ASSOC_RESP || stype == FC_REASSOC_RESP) && w.state == M_ASSOC && blen >= 6) {
        uint16_t status = le16(body + 2);
        if (status != STATUS_SUCCESS) {
            char why[48];
            snprintf(why, sizeof(why), "association refused (status %u)", status);
            fail(why);
            return;
        }
        w.aid = le16(body + 4) & 0x3FFF;
        klog("wifi: associated, AID %u\n", w.aid);
        if (w.hw->associated(&w.cur, w.aid) < 0) {
            fail("the card would not take the association");
            return;
        }
        if (!w.target.has_psk) {
            connected();
            return;
        }
        wpa_start(&w.wpa, w.target.psk, w.cur.bssid, w.hw->mac, w.own_ie, w.own_ie_len,
                  w.cur.rsn, w.cur.rsn_len);
        w.state = M_HANDSHAKE;
        w.deadline = ticks + EAPOL_TIMEOUT;
    }
}

/* Timeouts and retries of the current step. */
static void mlme_timer(void)
{
    if ((int32_t)(ticks - w.deadline) < 0)
        return;
    switch (w.state) {
    case M_SCAN:
        w.state = M_IDLE;
        break;
    case M_SCAN_CONNECT:
        connect_step(1);
        break;
    case M_AUTH:
    case M_ASSOC:
        if (w.tries++ >= FRAME_TRIES) {
            fail(w.state == M_AUTH ? "no answer to authentication" : "no answer to association");
            break;
        }
        if ((w.state == M_AUTH ? send_auth() : send_assoc()) < 0)
            klog("wifi: could not send a management frame\n");
        w.deadline = ticks + FRAME_TIMEOUT;
        break;
    case M_HANDSHAKE:
        if (w.sent_msg2)
            w.handshake_fails++;
        fail(w.sent_msg2 ? "no reply to the key handshake (wrong password?)"
                         : "the access point did not start the key handshake");
        break;
    default:
        break;
    }
}

static void take_requests(void)
{
    if (w.req_disconnect) {
        w.req_disconnect = 0;
        w.want = 0;
        if (w.state >= M_AUTH) {
            if (w.state >= M_ASSOC)
                send_deauth(REASON_DEAUTH_LEAVING);
            klog("wifi: disconnecting\n");
            link_down();
            w.encrypt = 0;
            w.hw->leave();
            memset(&w.cur, 0, sizeof(w.cur));
        }
        w.state = M_IDLE;
        w.error[0] = 0;
    }
    if (w.req_connect) {
        spin_lock(&wifi_lock);
        w.target = w.req_target;
        w.req_connect = 0;
        spin_unlock(&wifi_lock);
        if (w.state >= M_AUTH) {
            if (w.state >= M_ASSOC)
                send_deauth(REASON_DEAUTH_LEAVING);
            link_down();
            w.encrypt = 0;
            w.hw->leave();
            memset(&w.cur, 0, sizeof(w.cur));
        }
        w.want = 1;
        w.backoff = 0;
        w.handshake_fails = 0;
        w.error[0] = 0;
        w.state = M_IDLE;
        w.retry_at = ticks;
    }
    if (w.req_scan && (w.state == M_IDLE || w.state == M_CONNECTED)) {
        w.req_scan = 0;
        if (w.hw->scan() < 0) {
            klog("wifi: scan failed\n");
        } else if (w.state == M_IDLE) {
            w.state = M_SCAN;
            w.deadline = ticks + SCAN_TIMEOUT;
        }
    }
}

static void kwifid(void)
{
    w.status = WIFI_ST_STARTING;
    if (w.hw->start() < 0) {
        w.status = WIFI_ST_FAILED;
        strlcpy(w.error, w.hw->error[0] ? w.hw->error : "the card did not start", sizeof(w.error));
        klog("wifi: %s\n", w.error);
        goto stopped;
    }
    klog("wifi: %s ready, firmware %s, address %02x:%02x:%02x:%02x:%02x:%02x\n", w.hw->name,
         w.hw->firmware, w.hw->mac[0], w.hw->mac[1], w.hw->mac[2], w.hw->mac[3], w.hw->mac[4],
         w.hw->mac[5]);
    spin_lock(&net_lock);
    memcpy(netif.dev.mac, w.hw->mac, 6);
    spin_unlock(&net_lock);
    w.own_ie_len = rsn_build(w.own_ie);
    w.state = M_IDLE;
    w.status = WIFI_ST_IDLE;

    for (;;) {
        wait_event_timeout(&w, w.events, ticks + HZ / 10);
        w.events = 0;
        if (w.state == M_OFF)
            break;                      /* the firmware died */
        w.hw->service();
        if (w.state == M_OFF)
            break;                      /* service reported a fatal device failure */
        for (;;) {
            uint8_t f[RXQ_FRAME];
            uint32_t len;
            spin_lock(&wifi_lock);
            if (w.rxq_tail == w.rxq_head) {
                spin_unlock(&wifi_lock);
                break;
            }
            len = w.rxq[w.rxq_tail % RXQ_LEN].len;
            memcpy(f, w.rxq[w.rxq_tail % RXQ_LEN].data, len);
            w.rxq_tail++;
            spin_unlock(&wifi_lock);
            if (f[0] == EAPOL_TAG)
                handle_eapol(f + 1, len - 1);
            else
                handle_mgmt(f, len);
        }
        take_requests();
        if (w.state == M_OFF)
            break;
        if (w.state == M_IDLE && w.want && (int32_t)(ticks - w.retry_at) >= 0)
            connect_step(0);
        if (w.state == M_OFF)
            break;
        mlme_timer();
    }
stopped:
    /* proc_spawn_kthread enters via iret, without a C return address.
     * Keep a failed device's worker asleep rather than returning into
     * arbitrary stack data. A future recovery request can wake it. */
    for (;;)
        sleep_on(&w);
}

/* ---- registration and SYS_wifi ---- */

void wifi_register(struct wifi_hw *hw)
{
    w.hw = hw;
    w.status = WIFI_ST_STARTING;
    struct netdev dev = { .transmit = netdev_transmit };
    strlcpy(dev.name, "wlan0", sizeof(dev.name));
    strlcpy(dev.driver, hw->name, sizeof(dev.driver));
    net_register(&dev);
}

void wifi_start(void)
{
    if (w.hw)
        proc_spawn_kthread("kwifid", kwifid);
}

static int status_of(void)
{
    if (w.state == M_OFF)
        return w.status;
    switch (w.state) {
    case M_SCAN:
        return WIFI_ST_SCANNING;
    case M_IDLE:
        return w.want ? WIFI_ST_CONNECTING : WIFI_ST_IDLE;
    case M_CONNECTED:
        return WIFI_ST_CONNECTED;
    default:
        return WIFI_ST_CONNECTING;
    }
}

int wifi_syscall(uint32_t op, uint32_t arg)
{
    switch (op) {
    case WIFI_STATUS: {
        struct wifi_status st;
        memset(&st, 0, sizeof(st));
        if (!w.hw)
            return copy_to_user((void *)arg, &st, sizeof(st));
        st.state = (uint32_t)status_of();
        st.scans = w.scans;
        strlcpy(st.driver, w.hw->name, sizeof(st.driver));
        strlcpy(st.firmware, w.hw->firmware, sizeof(st.firmware));
        memcpy(st.mac, w.hw->mac, 6);
        spin_lock(&wifi_lock);
        const struct wifi_bss *b = &w.cur;
        if (b->seen) {
            memcpy(st.bssid, b->bssid, 6);
            fmt_ssid(st.ssid, b->ssid, b->ssid_len);
            st.channel = b->channel;
            st.freq = (uint16_t)wifi_channel_freq(b->channel, b->band);
            st.security = b->security;
            for (int i = 0; i < MAX_BSS; i++)
                if (w.bss[i].seen && !memcmp(w.bss[i].bssid, b->bssid, 6))
                    st.rssi = w.bss[i].rssi;
        } else if (w.want) {
            fmt_ssid(st.ssid, (const uint8_t *)w.target.ssid, w.target.ssid_len);
        }
        strlcpy(st.error, w.error, sizeof(st.error));
        spin_unlock(&wifi_lock);
        return copy_to_user((void *)arg, &st, sizeof(st));
    }
    case WIFI_SCAN:
        if (!w.hw || w.status == WIFI_ST_FAILED)
            return -ENODEV;
        w.req_scan = 1;
        wifi_wake();
        return 0;
    case WIFI_BSS: {
        struct wifi_bss_info bi;
        if (copy_from_user(&bi, (const void *)arg, sizeof(bi)) < 0)
            return -EFAULT;
        uint32_t want = bi.index, n = 0;
        int found = 0;
        spin_lock(&wifi_lock);
        for (int i = 0; i < MAX_BSS && !found; i++) {
            const struct wifi_bss *b = &w.bss[i];
            if (!b->seen || (int32_t)(ticks - b->seen) > BSS_EXPIRE || n++ != want)
                continue;
            memset(&bi, 0, sizeof(bi));
            bi.index = want;
            memcpy(bi.bssid, b->bssid, 6);
            bi.ssid_len = b->ssid_len;
            fmt_ssid(bi.ssid, b->ssid, b->ssid_len);
            bi.channel = b->channel;
            bi.freq = (uint16_t)wifi_channel_freq(b->channel, b->band);
            bi.rssi = b->rssi;
            bi.security = b->security;
            bi.age_ms = (ticks - b->seen) * (1000 / HZ);
            found = 1;
        }
        spin_unlock(&wifi_lock);
        if (!found)
            return -ENOENT;
        return copy_to_user((void *)arg, &bi, sizeof(bi));
    }
    case WIFI_CONNECT: {
        struct wifi_connect c;
        if (!w.hw || w.status == WIFI_ST_FAILED)
            return -ENODEV;                 /* while starting, it waits for the card */
        if (copy_from_user(&c, (const void *)arg, sizeof(c)) < 0)
            return -EFAULT;
        if (!c.ssid_len || c.ssid_len > WIFI_SSID_MAX)
            return -EINVAL;
        spin_lock(&wifi_lock);
        w.req_target = c;
        w.req_connect = 1;
        spin_unlock(&wifi_lock);
        wifi_wake();
        return 0;
    }
    case WIFI_DISCONNECT:
        if (!w.hw)
            return -ENODEV;
        w.req_disconnect = 1;
        wifi_wake();
        return 0;
    }
    return -EINVAL;
}
