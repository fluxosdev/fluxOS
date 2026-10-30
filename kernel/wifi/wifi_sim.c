/*
 * A simulated wireless card, for trying the wireless station and the
 * desktop's Wireless Networks where there is no card to try them with
 * (QEMU has none). Only with the kernel option wifi.sim, and only when no
 * real card was found.
 *
 * It is software standing in for a card and its access points: scans
 * hear the networks below (beacons, after a second and a half, as a real
 * scan takes), authentication and association are answered, and the WPA2
 * networks run the access point's side of the 4-way handshake with the
 * password "correct horse", so a wrong password fails as it would. Data
 * frames other than EAPOL go nowhere: a connection is made, but no DHCP
 * server answers on it.
 */
#include "kernel.h"
#include "drivers.h"
#include "lock.h"
#include <flux/aes.h>
#include <flux/sha1.h>
#include "ieee80211.h"
#include "wifi.h"

#define SIM_PASSWORD "correct horse"
#define SCAN_TIME    (HZ * 3 / 2)
#define REPLY_DELAY  2                  /* ticks before an answer arrives */
#define QLEN         8
#define QFRAME       320

static const struct sim_net {
    const char *ssid;                   /* "" for one that hides its name */
    uint8_t bssid[6];
    uint8_t channel, band;
    int8_t rssi;
    uint8_t security;                   /* SEC_* */
} nets[] = {
    { "fluxOS Home", { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x01 }, 6, BAND_2GHZ, -48, SEC_WPA2_PSK },
    { "fluxOS Home", { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x02 }, 44, BAND_5GHZ, -63, SEC_WPA2_PSK },
    { "Coffee Shop", { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x03 }, 11, BAND_2GHZ, -71, SEC_OPEN },
    { "Office", { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x04 }, 1, BAND_2GHZ, -60, SEC_OTHER },
    { "Neighbour 5G", { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x05 }, 149, BAND_5GHZ, -82, SEC_WPA2_PSK },
    { "", { 0x02, 0x5E, 0x00, 0x00, 0x00, 0x06 }, 1, BAND_2GHZ, -77, SEC_WPA2_PSK },
};
#define NNETS ((int)(sizeof(nets) / sizeof(nets[0])))

static struct wifi_hw sim;
static spinlock_t sim_lock = SPINLOCK_INIT("wifi-sim");

static struct {
    uint32_t scan_due;                  /* 0: no scan under way */
    const struct sim_net *joined;
    uint8_t pmk[WPA_PMK_LEN], anonce[WPA_NONCE_LEN], ptk[WPA_PTK_LEN];
    uint64_t replay;
    struct { uint8_t f[QFRAME]; uint32_t len, due; int rssi, channel, band; } q[QLEN];
    uint32_t qhead, qtail;
} s;

/* ---- frames from the air, delivered by service() ---- */

static void deliver_later(const uint8_t *f, uint32_t len, const struct sim_net *n)
{
    spin_lock(&sim_lock);
    if (s.qhead - s.qtail < QLEN && len <= QFRAME) {
        uint32_t i = s.qhead++ % QLEN;
        memcpy(s.q[i].f, f, len);
        s.q[i].len = len;
        s.q[i].due = ticks + REPLY_DELAY;
        s.q[i].rssi = n->rssi;
        s.q[i].channel = n->channel;
        s.q[i].band = n->band;
    }
    spin_unlock(&sim_lock);
}

static uint32_t hdr(uint8_t *f, uint8_t fc0, uint8_t fc1, const uint8_t *a1, const uint8_t *a2,
                    const uint8_t *a3)
{
    memset(f, 0, HDR_LEN);
    struct ieee80211_hdr *h = (void *)f;
    h->fc[0] = fc0;
    h->fc[1] = fc1;
    memcpy(h->addr1, a1, 6);
    memcpy(h->addr2, a2, 6);
    memcpy(h->addr3, a3, 6);
    return HDR_LEN;
}

static uint32_t put_rsn(uint8_t *p, int security)
{
    uint32_t n = rsn_build(p);
    if (security == SEC_OTHER) {
        p[19] = 8;                      /* AKM SAE: WPA3 only */
        p[20] = RSN_CAP_MFPC | RSN_CAP_MFPR;
    }
    return n;
}

static void beacon(const struct sim_net *n)
{
    static const uint8_t bcast[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    static const uint8_t rates_2ghz[] = { 0x82, 0x84, 0x8B, 0x96, 12, 18, 24, 36 };
    static const uint8_t rates_5ghz[] = { 0x8C, 18, 0x98, 36, 0xB0, 72, 96, 108 };
    uint8_t f[QFRAME];
    uint32_t len = hdr(f, FC_TYPE_MGMT | FC_BEACON, 0, bcast, n->bssid, n->bssid);
    memset(f + len, 0, 8);              /* timestamp */
    f[len + 8] = 100;                   /* beacon interval */
    f[len + 9] = 0;
    uint16_t capab = CAP_ESS | (n->security != SEC_OPEN ? CAP_PRIVACY : 0) |
                     (n->band == BAND_2GHZ ? CAP_SHORT_SLOT : 0);
    f[len + 10] = (uint8_t)capab;
    f[len + 11] = (uint8_t)(capab >> 8);
    len += 12;
    size_t sl = strlen(n->ssid);
    f[len++] = IE_SSID;
    f[len++] = (uint8_t)sl;
    memcpy(f + len, n->ssid, sl);
    len += (uint32_t)sl;
    f[len++] = IE_RATES;
    f[len++] = 8;
    memcpy(f + len, n->band == BAND_2GHZ ? rates_2ghz : rates_5ghz, 8);
    len += 8;
    f[len++] = IE_DS_PARAM;
    f[len++] = 1;
    f[len++] = n->channel;
    f[len++] = IE_TIM;
    f[len++] = 4;
    f[len++] = 0;
    f[len++] = 1;                       /* DTIM period */
    f[len++] = 0;
    f[len++] = 0;
    if (n->security != SEC_OPEN)
        len += put_rsn(f + len, n->security);
    wifi_rx(f, len, n->rssi, n->channel, n->band, 0);
}

static void service(void)
{
    if (s.scan_due && (int32_t)(ticks - s.scan_due) >= 0) {
        s.scan_due = 0;
        for (int i = 0; i < NNETS; i++)
            beacon(&nets[i]);
        wifi_scan_done();
    }
    for (;;) {
        uint8_t f[QFRAME];
        uint32_t len;
        int rssi, channel, band;
        spin_lock(&sim_lock);
        uint32_t i = s.qtail % QLEN;
        if (s.qtail == s.qhead || (int32_t)(ticks - s.q[i].due) < 0) {
            spin_unlock(&sim_lock);
            break;
        }
        len = s.q[i].len;
        memcpy(f, s.q[i].f, len);
        rssi = s.q[i].rssi;
        channel = s.q[i].channel;
        band = s.q[i].band;
        s.qtail++;
        spin_unlock(&sim_lock);
        wifi_rx(f, len, rssi, channel, band, 0);
    }
}

static int start(void)
{
    strlcpy(sim.firmware, "simulated", sizeof(sim.firmware));
    return 0;
}

static int scan(void)
{
    s.scan_due = ticks + SCAN_TIME;
    return 0;
}

/* ---- the access point's side ---- */

static const struct sim_net *net_by_bssid(const uint8_t *bssid)
{
    for (int i = 0; i < NNETS; i++)
        if (!memcmp(nets[i].bssid, bssid, 6))
            return &nets[i];
    return NULL;
}

static int join(const struct wifi_bss *bss)
{
    s.joined = net_by_bssid(bss->bssid);
    if (!s.joined) {
        strlcpy(sim.error, "no such simulated network", sizeof(sim.error));
        return -EINVAL;
    }
    if (s.joined->security == SEC_WPA2_PSK)
        pbkdf2_sha1(SIM_PASSWORD, sizeof(SIM_PASSWORD) - 1, (const uint8_t *)s.joined->ssid,
                    strlen(s.joined->ssid), 4096, s.pmk, WPA_PMK_LEN);
    return 0;
}

static void reply_mgmt(uint8_t subtype, const uint8_t *body, uint32_t blen)
{
    uint8_t f[HDR_LEN + 16];
    uint32_t len = hdr(f, FC_TYPE_MGMT | subtype, 0, sim.mac, s.joined->bssid, s.joined->bssid);
    memcpy(f + len, body, blen);
    deliver_later(f, len + blen, s.joined);
}

static int tx_mgmt(const uint8_t *f, uint32_t len)
{
    const struct ieee80211_hdr *h = (const void *)f;
    if (len < HDR_LEN || !s.joined || memcmp(h->addr1, s.joined->bssid, 6))
        return 0;                       /* sent into the void */
    uint8_t stype = h->fc[0] & FC_STYPE_MASK;
    if (stype == FC_AUTH) {
        static const uint8_t ok[6] = { 0, 0, 2, 0, 0, 0 };     /* open system, seq 2, success */
        reply_mgmt(FC_AUTH, ok, sizeof(ok));
    } else if (stype == FC_ASSOC_REQ) {
        static const uint8_t ok[6] = { 0x01, 0x04, 0, 0, 0x01, 0xC0 };  /* capab, success, AID 1 */
        reply_mgmt(FC_ASSOC_RESP, ok, sizeof(ok));
    }
    return 0;
}

/* An EAPOL-Key frame from the AP, as an 802.11 data frame. */
static void send_eapol(uint16_t info, const uint8_t *nonce, const uint8_t *kd, uint32_t kd_len, int mic)
{
    uint8_t e[99 + 96];
    uint32_t elen = 99 + kd_len;
    memset(e, 0, elen);
    e[0] = 2;
    e[1] = 3;
    e[2] = (uint8_t)((elen - 4) >> 8);
    e[3] = (uint8_t)(elen - 4);
    e[4] = 2;
    e[5] = (uint8_t)(info >> 8);
    e[6] = (uint8_t)info;
    e[8] = 16;
    s.replay++;
    for (int i = 0; i < 8; i++)
        e[9 + i] = (uint8_t)(s.replay >> (56 - 8 * i));
    if (nonce)
        memcpy(e + 17, nonce, WPA_NONCE_LEN);
    e[97] = (uint8_t)(kd_len >> 8);
    e[98] = (uint8_t)kd_len;
    memcpy(e + 99, kd, kd_len);
    if (mic) {
        uint8_t h[SHA1_SIZE];
        hmac_sha1(s.ptk, WPA_KCK_LEN, e, elen, h);
        memcpy(e + 81, h, 16);
    }
    uint8_t f[QFRAME];
    uint32_t len = hdr(f, FC_TYPE_DATA, FC_FROMDS, sim.mac, s.joined->bssid, s.joined->bssid);
    static const uint8_t snap[8] = { 0xAA, 0xAA, 0x03, 0, 0, 0, 0x88, 0x8E };
    memcpy(f + len, snap, 8);
    memcpy(f + len + 8, e, elen);
    deliver_later(f, len + 8 + elen, s.joined);
}

static int associated(const struct wifi_bss *bss, uint16_t aid)
{
    (void)bss;
    (void)aid;
    if (s.joined && s.joined->security == SEC_WPA2_PSK) {
        random_bytes(s.anonce, WPA_NONCE_LEN);
        send_eapol(0x008A, s.anonce, NULL, 0, 0);           /* message 1 */
    }
    return 0;
}

static void derive_ptk(const uint8_t *snonce)
{
    uint8_t data[76];
    const uint8_t *aa = s.joined->bssid, *spa = sim.mac;
    int a = memcmp(aa, spa, 6) < 0, b = memcmp(s.anonce, snonce, WPA_NONCE_LEN) < 0;
    memcpy(data, a ? aa : spa, 6);
    memcpy(data + 6, a ? spa : aa, 6);
    memcpy(data + 12, b ? s.anonce : snonce, WPA_NONCE_LEN);
    memcpy(data + 44, b ? snonce : s.anonce, WPA_NONCE_LEN);
    wpa_prf_sha1(s.pmk, WPA_PMK_LEN, "Pairwise key expansion", data, sizeof(data), s.ptk, WPA_PTK_LEN);
}

/* Message 2 from the station: if its MIC checks (the right password), message 3. */
static void station_eapol(const uint8_t *e, uint32_t len)
{
    if (len < 99 || e[1] != 3)
        return;
    uint16_t info = (uint16_t)(e[5] << 8 | e[6]);
    if ((info & 0x0308) != 0x0108)
        return;                         /* message 4 (secure set), or not pairwise: nothing to do */
    derive_ptk(e + 17);
    uint8_t copy[256], h[SHA1_SIZE];
    if (len > sizeof(copy))
        return;
    memcpy(copy, e, len);
    memset(copy + 81, 0, 16);
    hmac_sha1(s.ptk, WPA_KCK_LEN, copy, len, h);
    if (memcmp(h, e + 81, 16)) {
        klog("wifi-sim: message 2 MIC does not check: wrong password\n");
        return;                         /* as a real AP: no answer */
    }
    uint8_t kd[64], gtk[16];
    uint32_t n = put_rsn(kd, SEC_WPA2_PSK);
    random_bytes(gtk, sizeof(gtk));
    kd[n++] = IE_VENDOR;
    kd[n++] = 22;
    kd[n++] = 0x00;
    kd[n++] = 0x0F;
    kd[n++] = 0xAC;
    kd[n++] = 1;                        /* GTK KDE */
    kd[n++] = 1;                        /* key 1 */
    kd[n++] = 0;
    memcpy(kd + n, gtk, 16);
    n += 16;
    if (n % 8) {
        kd[n++] = 0xDD;
        while (n % 8)
            kd[n++] = 0;
    }
    uint8_t wrapped[72];
    aes_wrap(s.ptk + WPA_KCK_LEN, kd, n, wrapped);
    send_eapol(0x13CA, s.anonce, wrapped, n + 8, 1);         /* message 3 */
}

/* Called with interrupts off for the network stack's frames: only EAPOL is looked at. */
static int tx_data(const uint8_t *f, uint32_t len, int encrypt)
{
    (void)encrypt;
    if (!s.joined || len < HDR_LEN + LLC_SNAP_LEN)
        return 0;
    const uint8_t *snap = f + HDR_LEN;
    if (snap[6] != 0x88 || snap[7] != 0x8E || s.joined->security != SEC_WPA2_PSK)
        return 0;                       /* no one is listening */
    station_eapol(snap + LLC_SNAP_LEN, len - HDR_LEN - LLC_SNAP_LEN);
    return 0;
}

static int authorized(void)
{
    return 0;
}

static void leave(void)
{
    s.joined = NULL;
    spin_lock(&sim_lock);
    s.qtail = s.qhead;                  /* answers still in the air are lost */
    spin_unlock(&sim_lock);
}

static int set_key(int pairwise, int idx, const uint8_t *key, uint32_t len, const uint8_t rsc[8])
{
    (void)pairwise;
    (void)idx;
    (void)key;
    (void)len;
    (void)rsc;
    return 0;
}

int wifi_sim_probe(void)
{
    if (!cmdline_has("wifi.sim"))
        return -1;
    static const uint8_t mac[6] = { 0x02, 0x00, 0x00, 0x5E, 0x10, 0x01 };
    sim = (struct wifi_hw){
        .name = "simulated wireless card",
        .start = start, .service = service, .scan = scan, .join = join,
        .associated = associated, .authorized = authorized, .leave = leave,
        .tx_mgmt = tx_mgmt, .tx_data = tx_data, .set_key = set_key,
    };
    memcpy(sim.mac, mac, 6);
    wifi_register(&sim);
    boot_ok("Network: simulated wireless card (wifi.sim), password \"" SIM_PASSWORD "\"");
    return 0;
}
