/*
 * WPA2-Personal from the station's side: the RSN element, PTK derivation,
 * the 4-way handshake and the group key handshake (IEEE 802.11-2016
 * 12.7). Only the combination nearly every home network offers: PSK
 * authentication with CCMP for both the pairwise and group ciphers, key
 * descriptor version 2 (HMAC-SHA1 MICs, AES key wrap). The MIC is checked
 * on everything that carries one, the replay counter must grow, and the
 * AP's RSN element in message 3 must agree with the one it advertised.
 */
#include <string.h>
#include <flux/aes.h>
#include <flux/sha1.h>
#include "wpa.h"

/* ---- RSN element ---- */

#define IE_RSN 48
#define SUITE_OUI0 0x00
#define SUITE_OUI1 0x0F
#define SUITE_OUI2 0xAC
#define AKM_8021X 1
#define AKM_PSK   2
#define AKM_SAE   8

static uint32_t suite(const uint8_t *p)
{
    if (p[0] != SUITE_OUI0 || p[1] != SUITE_OUI1 || p[2] != SUITE_OUI2)
        return 0xFF;
    return p[3];
}

int rsn_parse(const uint8_t *ie, uint32_t len, struct rsn_info *ri)
{
    memset(ri, 0, sizeof(*ri));
    if (len < 4 || ie[0] != IE_RSN || ie[1] + 2u > len || ie[1] < 2)
        return -1;
    const uint8_t *p = ie + 2, *end = ie + 2 + ie[1];
    if ((p[0] | p[1] << 8) != 1)
        return -1;                      /* RSN version 1 */
    p += 2;
    /* Everything after the version is optional; the defaults are CCMP and 802.1X. */
    ri->group = RSN_CIPHER_CCMP;
    ri->pairwise_ccmp = 1;
    ri->akm_other = 1;
    if (end - p < 4)
        return 0;
    ri->group = suite(p);
    p += 4;
    if (end - p < 2)
        return 0;
    uint32_t n = p[0] | p[1] << 8;
    p += 2;
    if ((uint32_t)(end - p) < n * 4)
        return -1;
    ri->pairwise_ccmp = 0;
    for (uint32_t i = 0; i < n; i++, p += 4)
        if (suite(p) == RSN_CIPHER_CCMP)
            ri->pairwise_ccmp = 1;
    if (end - p < 2)
        return 0;
    n = p[0] | p[1] << 8;
    p += 2;
    if ((uint32_t)(end - p) < n * 4)
        return -1;
    ri->akm_other = 0;
    for (uint32_t i = 0; i < n; i++, p += 4) {
        uint32_t a = suite(p);
        if (a == AKM_PSK)
            ri->akm_psk = 1;
        else if (a == AKM_SAE)
            ri->akm_sae = 1;
        else
            ri->akm_other = 1;
    }
    if (end - p >= 2)
        ri->caps = (uint16_t)(p[0] | p[1] << 8);
    return 0;
}

int rsn_security(const uint8_t *rsn_ie, uint32_t rsn_len, int privacy)
{
    if (!rsn_ie)
        return privacy ? SEC_OTHER : SEC_OPEN;     /* WEP or WPA1 only */
    struct rsn_info ri;
    if (rsn_parse(rsn_ie, rsn_len, &ri) < 0)
        return SEC_OTHER;
    if (ri.group == RSN_CIPHER_CCMP && ri.pairwise_ccmp && ri.akm_psk && !(ri.caps & RSN_CAP_MFPR))
        return SEC_WPA2_PSK;
    return SEC_OTHER;
}

uint32_t rsn_build(uint8_t *out)
{
    static const uint8_t ie[] = {
        IE_RSN, 20, 1, 0,
        SUITE_OUI0, SUITE_OUI1, SUITE_OUI2, RSN_CIPHER_CCMP,          /* group */
        1, 0, SUITE_OUI0, SUITE_OUI1, SUITE_OUI2, RSN_CIPHER_CCMP,    /* pairwise */
        1, 0, SUITE_OUI0, SUITE_OUI1, SUITE_OUI2, AKM_PSK,            /* AKM */
        0, 0,                                                         /* capabilities */
    };
    memcpy(out, ie, sizeof(ie));
    return sizeof(ie);
}

/* ---- keys ---- */

void wpa_prf_sha1(const uint8_t *key, uint32_t key_len, const char *label, const uint8_t *data,
                  uint32_t data_len, uint8_t *out, uint32_t out_len)
{
    uint8_t zero = 0, counter = 0, h[SHA1_SIZE];
    const uint8_t *d[4] = { (const uint8_t *)label, &zero, data, &counter };
    size_t l[4] = { strlen(label), 1, data_len, 1 };
    while (out_len) {
        hmac_sha1_vec(key, key_len, 4, d, l, h);
        uint32_t take = out_len < SHA1_SIZE ? out_len : SHA1_SIZE;
        memcpy(out, h, take);
        out += take;
        out_len -= take;
        counter++;
    }
}

static void derive_ptk(struct wpa_sm *sm)
{
    uint8_t data[6 + 6 + WPA_NONCE_LEN + WPA_NONCE_LEN];
    int a_first = memcmp(sm->aa, sm->spa, 6) < 0;
    memcpy(data, a_first ? sm->aa : sm->spa, 6);
    memcpy(data + 6, a_first ? sm->spa : sm->aa, 6);
    int an_first = memcmp(sm->anonce, sm->snonce, WPA_NONCE_LEN) < 0;
    memcpy(data + 12, an_first ? sm->anonce : sm->snonce, WPA_NONCE_LEN);
    memcpy(data + 12 + WPA_NONCE_LEN, an_first ? sm->snonce : sm->anonce, WPA_NONCE_LEN);
    wpa_prf_sha1(sm->pmk, WPA_PMK_LEN, "Pairwise key expansion", data, sizeof(data),
                 sm->ptk, WPA_PTK_LEN);
}

/* ---- EAPOL-Key frames ---- */

#define EAPOL_VERSION   1
#define EAPOL_KEY       3
#define DESC_RSN        2

#define KI_VERSION_MASK 0x0007
#define KI_HMAC_SHA1_AES 2
#define KI_PAIRWISE     0x0008
#define KI_INSTALL      0x0040
#define KI_ACK          0x0080
#define KI_MIC          0x0100
#define KI_SECURE       0x0200
#define KI_ERROR        0x0400
#define KI_REQUEST      0x0800
#define KI_ENCRYPTED    0x1000

/* Offsets from the start of the EAPOL header. */
#define O_VERSION   0
#define O_TYPE      1
#define O_LEN       2
#define O_DESC      4
#define O_INFO      5
#define O_KEYLEN    7
#define O_REPLAY    9
#define O_NONCE     17
#define O_IV        49
#define O_RSC       65
#define O_MIC       81
#define O_DATALEN   97
#define O_DATA      99

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static void put_be16(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static void compute_mic(const struct wpa_sm *sm, const uint8_t *frame, uint32_t len, uint8_t mic[16])
{
    uint8_t h[SHA1_SIZE];
    const uint8_t zero[16] = { 0 };
    const uint8_t *d[3] = { frame, zero, frame + O_MIC + 16 };
    size_t l[3] = { O_MIC, 16, len - O_MIC - 16 };
    hmac_sha1_vec(WPA_KCK(sm), WPA_KCK_LEN, 3, d, l, h);
    memcpy(mic, h, 16);
}

static int mic_ok(const struct wpa_sm *sm, const uint8_t *frame, uint32_t len)
{
    uint8_t mic[16];
    compute_mic(sm, frame, len, mic);
    uint8_t diff = 0;
    for (int i = 0; i < 16; i++)
        diff |= mic[i] ^ frame[O_MIC + i];
    return diff == 0;
}

/* A reply with no key data beyond `data`; its MIC filled in when asked. */
static void build_reply(const struct wpa_sm *sm, struct wpa_result *res, uint16_t info,
                        const uint8_t *replay, const uint8_t *nonce, const uint8_t *data,
                        uint32_t data_len)
{
    uint8_t *f = res->reply;
    uint32_t len = O_DATA + data_len;
    memset(f, 0, len);
    f[O_VERSION] = EAPOL_VERSION;
    f[O_TYPE] = EAPOL_KEY;
    put_be16(f + O_LEN, len - 4);
    f[O_DESC] = DESC_RSN;
    put_be16(f + O_INFO, info);
    memcpy(f + O_REPLAY, replay, 8);
    if (nonce)
        memcpy(f + O_NONCE, nonce, WPA_NONCE_LEN);
    put_be16(f + O_DATALEN, data_len);
    if (data_len)
        memcpy(f + O_DATA, data, data_len);
    if (info & KI_MIC)
        compute_mic(sm, f, len, f + O_MIC);
    res->reply_len = len;
}

/* Find the GTK KDE (and the RSN element) in decrypted key data. */
static int parse_key_data(const uint8_t *p, uint32_t len, struct wpa_result *res,
                          const uint8_t **rsn, uint32_t *rsn_len)
{
    const uint8_t *end = p + len;
    int found = 0;
    while (end - p >= 2) {
        if (p[0] == 0xDD && p[1] == 0)
            break;                      /* padding */
        uint32_t l = p[1];
        if ((uint32_t)(end - p) < 2 + l)
            break;
        if (p[0] == IE_RSN && rsn && !*rsn) {
            *rsn = p;
            *rsn_len = 2 + l;
        } else if (p[0] == 0xDD && l >= 6 + 16 && p[2] == SUITE_OUI0 && p[3] == SUITE_OUI1 &&
                   p[4] == SUITE_OUI2 && p[5] == 1) {      /* GTK KDE */
            uint32_t glen = l - 6;
            if (glen > WPA_GTK_MAX)
                return -1;
            res->gtk_idx = p[6] & 3;
            memcpy(res->gtk, p + 8, glen);
            res->gtk_len = glen;
            found = 1;
        }
        p += 2 + l;
    }
    return found ? 0 : -1;
}

static int same_rsn(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
    if (alen == blen && !memcmp(a, b, alen))
        return 1;
    struct rsn_info x, y;
    if (rsn_parse(a, alen, &x) < 0 || rsn_parse(b, blen, &y) < 0)
        return 0;
    return x.group == y.group && x.pairwise_ccmp == y.pairwise_ccmp && x.akm_psk == y.akm_psk &&
           (x.caps & RSN_CAP_MFPR) == (y.caps & RSN_CAP_MFPR);
}

void wpa_start(struct wpa_sm *sm, const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6],
               const uint8_t spa[6], const uint8_t *own_ie, uint32_t own_ie_len,
               const uint8_t *ap_ie, uint32_t ap_ie_len)
{
    memset(sm, 0, sizeof(*sm));
    memcpy(sm->pmk, pmk, WPA_PMK_LEN);
    memcpy(sm->aa, aa, 6);
    memcpy(sm->spa, spa, 6);
    sm->own_ie_len = own_ie_len > WPA_IE_MAX ? WPA_IE_MAX : own_ie_len;
    memcpy(sm->own_ie, own_ie, sm->own_ie_len);
    sm->ap_ie_len = ap_ie_len > WPA_IE_MAX ? WPA_IE_MAX : ap_ie_len;
    memcpy(sm->ap_ie, ap_ie, sm->ap_ie_len);
    sm->state = WPA_WAIT_MSG1;
}

int wpa_rx(struct wpa_sm *sm, const uint8_t *f, uint32_t len, struct wpa_result *res)
{
    memset(res, 0, sizeof(*res));
    if (len < O_DATA || f[O_TYPE] != EAPOL_KEY || f[O_DESC] != DESC_RSN)
        return WPA_EBADFRAME;
    uint32_t body = be16(f + O_LEN);
    if (body + 4 > len || body + 4 < O_DATA)
        return WPA_EBADFRAME;
    len = body + 4;                     /* ignore link-layer padding */
    uint16_t info = be16(f + O_INFO);
    uint32_t data_len = be16(f + O_DATALEN);
    if ((info & KI_VERSION_MASK) != KI_HMAC_SHA1_AES || O_DATA + data_len > len)
        return WPA_EBADFRAME;
    if (!(info & KI_ACK) || (info & KI_REQUEST))
        return WPA_EBADFRAME;           /* only frames from the authenticator */
    if (sm->replay_valid && memcmp(f + O_REPLAY, sm->replay, 8) <= 0)
        return WPA_EREPLAY;
    const uint8_t *data = f + O_DATA;

    if ((info & KI_PAIRWISE) && !(info & KI_MIC)) {
        /* Message 1: a new ANonce. A repeated message 1 starts over. */
        if (sm->state != WPA_WAIT_MSG1 && sm->state != WPA_WAIT_MSG3 && sm->state != WPA_DONE)
            return WPA_ESTATE;
        memcpy(sm->anonce, f + O_NONCE, WPA_NONCE_LEN);
        random_bytes(sm->snonce, WPA_NONCE_LEN);
        derive_ptk(sm);
        build_reply(sm, res, KI_HMAC_SHA1_AES | KI_PAIRWISE | KI_MIC, f + O_REPLAY, sm->snonce,
                    sm->own_ie, sm->own_ie_len);
        sm->state = WPA_WAIT_MSG3;
        return 0;
    }

    if ((info & (KI_PAIRWISE | KI_MIC | KI_INSTALL)) == (KI_PAIRWISE | KI_MIC | KI_INSTALL)) {
        /* Message 3: the AP has the same PTK (the MIC says so) and sends the GTK. */
        if (sm->state != WPA_WAIT_MSG3 && sm->state != WPA_DONE)
            return WPA_ESTATE;
        if (memcmp(f + O_NONCE, sm->anonce, WPA_NONCE_LEN))
            return WPA_ESTATE;          /* not the ANonce of the message 1 we answered */
        if (!mic_ok(sm, f, len))
            return WPA_EMIC;
        if (!(info & KI_ENCRYPTED) || data_len < 24 || data_len % 8 || data_len - 8 > 256)
            return WPA_EKEYDATA;
        uint8_t plain[256];
        if (aes_unwrap(WPA_KEK(sm), data, data_len - 8, plain) < 0)
            return WPA_EKEYDATA;
        const uint8_t *rsn = NULL;
        uint32_t rsn_len = 0;
        if (parse_key_data(plain, data_len - 8, res, &rsn, &rsn_len) < 0)
            return WPA_EKEYDATA;
        if (!rsn || !same_rsn(rsn, rsn_len, sm->ap_ie, sm->ap_ie_len)) {
            memset(res, 0, sizeof(*res));
            return WPA_EIE;
        }
        memcpy(sm->replay, f + O_REPLAY, 8);
        sm->replay_valid = 1;
        memcpy(res->gtk_rsc, f + O_RSC, 8);
        build_reply(sm, res, KI_HMAC_SHA1_AES | KI_PAIRWISE | KI_MIC | KI_SECURE, f + O_REPLAY,
                    NULL, NULL, 0);
        res->install_ptk = 1;
        res->install_gtk = 1;
        res->connected = sm->state != WPA_DONE;
        sm->state = WPA_DONE;
        return 0;
    }

    if (!(info & KI_PAIRWISE) && (info & KI_MIC) && (info & KI_SECURE)) {
        /* Group key handshake message 1: a new GTK. */
        if (sm->state != WPA_DONE)
            return WPA_ESTATE;
        if (!mic_ok(sm, f, len))
            return WPA_EMIC;
        if (!(info & KI_ENCRYPTED) || data_len < 24 || data_len % 8 || data_len - 8 > 256)
            return WPA_EKEYDATA;
        uint8_t plain[256];
        if (aes_unwrap(WPA_KEK(sm), data, data_len - 8, plain) < 0 ||
            parse_key_data(plain, data_len - 8, res, NULL, NULL) < 0)
            return WPA_EKEYDATA;
        memcpy(sm->replay, f + O_REPLAY, 8);
        sm->replay_valid = 1;
        memcpy(res->gtk_rsc, f + O_RSC, 8);
        build_reply(sm, res, KI_HMAC_SHA1_AES | KI_MIC | KI_SECURE, f + O_REPLAY, NULL, NULL, 0);
        res->install_gtk = 1;
        return 0;
    }
    return WPA_EBADFRAME;
}

const char *wpa_strerror(int err)
{
    switch (err) {
    case WPA_EBADFRAME: return "not an EAPOL-Key frame we handle";
    case WPA_EREPLAY:   return "replayed frame";
    case WPA_EMIC:      return "MIC failure (wrong password?)";
    case WPA_ESTATE:    return "unexpected handshake message";
    case WPA_EIE:       return "RSN element mismatch";
    case WPA_EKEYDATA:  return "bad key data";
    default:            return "error";
    }
}
