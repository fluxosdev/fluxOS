/*
 * WPA2-Personal (IEEE 802.11i, RSN with PSK and CCMP): the RSN element,
 * the 4-way handshake and the group key handshake, from the station's
 * side. Pure logic over byte buffers (wpa.c): it builds the replies, and
 * says which keys to install; the caller sends and installs them.
 */
#ifndef WIFI_WPA_H
#define WIFI_WPA_H

#include <stdint.h>

#define WPA_PMK_LEN   32
#define WPA_NONCE_LEN 32
#define WPA_KCK_LEN   16
#define WPA_KEK_LEN   16
#define WPA_TK_LEN    16                /* CCMP-128 */
#define WPA_PTK_LEN   (WPA_KCK_LEN + WPA_KEK_LEN + WPA_TK_LEN)
#define WPA_IE_MAX    64
#define WPA_GTK_MAX   32
#define WPA_EAPOL_MAX 256               /* what we send; received frames may be longer */

/* What a network's RSN element (or its absence) offers. */
#define SEC_OPEN      0
#define SEC_WPA2_PSK  1                 /* CCMP pairwise and group, PSK: what we do */
#define SEC_OTHER     2                 /* WPA1, TKIP, WEP, SAE only, 802.1X, MFP required... */

struct rsn_info {
    uint32_t group;                     /* suite selectors as 00-0F-AC-n: n, or 0xFF for others */
    uint32_t pairwise_ccmp;             /* CCMP is among the pairwise suites */
    uint32_t akm_psk, akm_sae, akm_other;
    uint16_t caps;
};

#define RSN_CIPHER_TKIP 2
#define RSN_CIPHER_CCMP 4
#define RSN_CAP_MFPR    0x0040
#define RSN_CAP_MFPC    0x0080

/* Parse an RSN element (starting with its id, 48). 0, or -1 if malformed. */
int rsn_parse(const uint8_t *ie, uint32_t len, struct rsn_info *ri);
/* What we can do with a network: SEC_* from its RSN element (NULL: none)
 * and whether its beacon says Privacy. */
int rsn_security(const uint8_t *rsn_ie, uint32_t rsn_len, int privacy);
/* Our RSN element for the association request: CCMP/CCMP/PSK. Its length. */
uint32_t rsn_build(uint8_t *out);

enum wpa_state { WPA_IDLE, WPA_WAIT_MSG1, WPA_WAIT_MSG3, WPA_DONE };

struct wpa_sm {
    enum wpa_state state;
    uint8_t pmk[WPA_PMK_LEN];
    uint8_t aa[6], spa[6];              /* the AP's address and ours */
    uint8_t own_ie[WPA_IE_MAX], ap_ie[WPA_IE_MAX];
    uint32_t own_ie_len, ap_ie_len;     /* ap_ie: from its beacon or probe response */
    uint8_t anonce[WPA_NONCE_LEN], snonce[WPA_NONCE_LEN];
    uint8_t ptk[WPA_PTK_LEN];           /* KCK, KEK, TK */
    uint8_t tptk[WPA_PTK_LEN];          /* from the latest message 1, until message 3 checks */
    int tptk_set;
    uint8_t replay[8];
    int replay_valid;
};

#define WPA_KCK(sm) ((sm)->ptk)
#define WPA_KEK(sm) ((sm)->ptk + WPA_KCK_LEN)
#define WPA_TK(sm)  ((sm)->ptk + WPA_KCK_LEN + WPA_KEK_LEN)

/* What to do after a received EAPOL frame. */
struct wpa_result {
    uint8_t reply[WPA_EAPOL_MAX];       /* an EAPOL frame to send (to the AP, type 888E) */
    uint32_t reply_len;                 /* 0: nothing to send */
    int install_ptk;                    /* after sending the reply: the TK in WPA_TK() */
    int install_gtk;
    uint8_t gtk[WPA_GTK_MAX];
    uint32_t gtk_len;
    int gtk_idx;                        /* 1..3 */
    uint8_t gtk_rsc[8];                 /* the group key's receive sequence counter */
    int connected;                      /* the 4-way handshake just completed */
};

/* Start a handshake, after a successful association. */
void wpa_start(struct wpa_sm *sm, const uint8_t pmk[WPA_PMK_LEN], const uint8_t aa[6],
               const uint8_t spa[6], const uint8_t *own_ie, uint32_t own_ie_len,
               const uint8_t *ap_ie, uint32_t ap_ie_len);
/*
 * An EAPOL frame from the AP (from the EAPOL header on). Returns 0 when
 * it was used (see res), or a negative WPA_E* when it was dropped; the
 * handshake goes on either way, as a dropped frame may just be a stray.
 */
int wpa_rx(struct wpa_sm *sm, const uint8_t *frame, uint32_t len, struct wpa_result *res);

#define WPA_EBADFRAME  -1               /* not an EAPOL-Key frame we understand */
#define WPA_EREPLAY    -2               /* replay counter not newer */
#define WPA_EMIC       -3               /* MIC does not check: almost always a wrong password */
#define WPA_ESTATE     -4               /* not expected now */
#define WPA_EIE        -5               /* the AP's RSN element in message 3 differs from its beacon's */
#define WPA_EKEYDATA   -6               /* key data does not unwrap or has no GTK */
const char *wpa_strerror(int err);

/* The PRF of IEEE 802.11-2016 12.7.1.2 (exported for tests). */
void wpa_prf_sha1(const uint8_t *key, uint32_t key_len, const char *label, const uint8_t *data,
                  uint32_t data_len, uint8_t *out, uint32_t out_len);

/* Supplied by the environment: random bytes (the kernel's pool). */
void random_bytes(void *buf, uint32_t n);

#endif
