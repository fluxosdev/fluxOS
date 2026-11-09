/* The wireless code that needs no hardware: the WPA2 crypto against
 * published test vectors, and the 4-way and group key handshakes against
 * a simulated access point. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../lib/sha1.c"
#include "../lib/aes.c"
#include "../kernel/wifi/wpa.c"
#include "../kernel/wifi/iwl_fw.c"

static int failures;
#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); failures++; } } while (0)

static uint8_t nonce_byte = 0x5A;
void random_bytes(void *buf, uint32_t n) { memset(buf, nonce_byte, n); }

static void unhex(const char *s, uint8_t *out)
{
    for (; s[0] && s[1]; s += 2)
        sscanf(s, "%2hhx", out++);
}

static int is_hex(const uint8_t *p, size_t n, const char *want)
{
    uint8_t w[128];
    unhex(want, w);
    return strlen(want) == 2 * n && !memcmp(p, w, n);
}

static void test_crypto(void)
{
    uint8_t d[64], k[80];
    sha1("abc", 3, d);
    CHECK(is_hex(d, 20, "a9993e364706816aba3e25717850c26c9cd0d89d"));
    const char *m = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    sha1(m, strlen(m), d);
    CHECK(is_hex(d, 20, "84983e441c3bd26ebaae4aa1f95129e5e54670f1"));
    memset(k, 0x0b, 20);
    hmac_sha1(k, 20, "Hi There", 8, d);                         /* RFC 2202 1 */
    CHECK(is_hex(d, 20, "b617318655057264e28bc0b6fb378c8ef146be00"));
    memset(k, 0xaa, 80);
    hmac_sha1(k, 80, "Test Using Larger Than Block-Size Key - Hash Key First", 54, d);   /* 6 */
    CHECK(is_hex(d, 20, "aa4ae5e15272d00e95705637ce8a3b55ed402112"));
    /* IEEE 802.11-2016 J.4: passphrase to PSK */
    pbkdf2_sha1("password", 8, (const uint8_t *)"IEEE", 4, 4096, d, 32);
    CHECK(is_hex(d, 32, "f42c6fc52df0ebef9ebb4b90b38a5f902e83fe1b135a70e23aed762e9710a12e"));
    pbkdf2_sha1("ThisIsAPassword", 15, (const uint8_t *)"ThisIsASSID", 11, 4096, d, 32);
    CHECK(is_hex(d, 32, "0dc0d6eb90555ed6419756b9a15ec3e3209b63df707dd508d14581f8982721af"));
    /* the 802.11 PRF */
    memset(k, 0x0b, 20);
    wpa_prf_sha1(k, 20, "prefix", (const uint8_t *)"Hi There", 8, d, 64);
    CHECK(is_hex(d, 64, "bcd4c650b30b9684951829e0d75f9d54b862175ed9f00606e17d8da35402ffee"
                        "75df78c3d31e0f889f012120c0862beb67753e7439ae242edb8373698356cf5a"));
    /* FIPS 197 C.1 and RFC 3394 4.1 */
    uint8_t key[16], pt[16], ct[16], w[32], u[24];
    unhex("000102030405060708090a0b0c0d0e0f", key);
    unhex("00112233445566778899aabbccddeeff", pt);
    struct aes128 a;
    aes128_init(&a, key);
    aes128_encrypt(&a, pt, ct);
    CHECK(is_hex(ct, 16, "69c4e0d86a7b0430d8cdb78070b4c55a"));
    aes128_decrypt(&a, ct, d);
    CHECK(!memcmp(d, pt, 16));
    aes_wrap(key, pt, 16, w);
    CHECK(is_hex(w, 24, "1fa68b0a8112b447aef34bd8fb5a7b829d3e862371d2cfe5"));
    CHECK(aes_unwrap(key, w, 16, u) == 0 && !memcmp(u, pt, 16));
    w[5] ^= 1;
    CHECK(aes_unwrap(key, w, 16, u) < 0);
    /* three blocks, as OpenSSL's id-aes128-wrap gives it */
    uint8_t kd[24];
    unhex("00112233445566778899aabbccddeeff0001020304050607", kd);
    aes_wrap(key, kd, 24, w);
    CHECK(is_hex(w, 32, "889671106535a9f86d9f9a262f674569efa38d7535aac77527cab92855bddd6e"));
    CHECK(aes_unwrap(key, w, 24, u) == 0 && !memcmp(u, kd, 24));
}

static void test_rsn(void)
{
    uint8_t ie[64];
    uint32_t n = rsn_build(ie);
    struct rsn_info ri;
    CHECK(n == 22 && rsn_parse(ie, n, &ri) == 0);
    CHECK(ri.group == RSN_CIPHER_CCMP && ri.pairwise_ccmp && ri.akm_psk && !ri.akm_other);
    CHECK(rsn_security(ie, n, 1) == SEC_WPA2_PSK);
    CHECK(rsn_security(NULL, 0, 0) == SEC_OPEN);
    CHECK(rsn_security(NULL, 0, 1) == SEC_OTHER);               /* WEP or WPA1 */
    ie[19] = 8;                                                 /* AKM SAE: WPA3 only */
    CHECK(rsn_security(ie, n, 1) == SEC_OTHER);
    /* WPA2/WPA3 transition: PSK and SAE; MFP capable but not required */
    static const uint8_t mixed[] = { 48, 24, 1, 0, 0, 0x0f, 0xac, 4, 1, 0, 0, 0x0f, 0xac, 4,
                                     2, 0, 0, 0x0f, 0xac, 2, 0, 0x0f, 0xac, 8, 0x80, 0 };
    CHECK(rsn_security(mixed, sizeof(mixed), 1) == SEC_WPA2_PSK);
    uint8_t req[sizeof(mixed)];
    memcpy(req, mixed, sizeof(mixed));
    req[24] = 0xC0;                                             /* MFP required */
    CHECK(rsn_security(req, sizeof(req), 1) == SEC_OTHER);
    static const uint8_t tkip_group[] = { 48, 20, 1, 0, 0, 0x0f, 0xac, 2, 1, 0, 0, 0x0f, 0xac, 4,
                                          1, 0, 0, 0x0f, 0xac, 2, 0, 0 };
    CHECK(rsn_security(tkip_group, sizeof(tkip_group), 1) == SEC_OTHER);
    CHECK(rsn_parse(ie, 3, &ri) < 0);
}

/* An access point's side, built here byte by byte from the standard. */
struct ap {
    uint8_t pmk[32], aa[6], spa[6], anonce[32], ptk[48], ie[64];
    uint32_t ie_len;
    uint64_t replay;
};

static uint32_t ap_frame(struct ap *ap, uint8_t *f, uint16_t info, const uint8_t *nonce,
                         const uint8_t *keydata, uint32_t kd_len, int mic)
{
    uint32_t len = 99 + kd_len;
    memset(f, 0, len);
    f[0] = 2;
    f[1] = 3;
    f[2] = (uint8_t)((len - 4) >> 8);
    f[3] = (uint8_t)(len - 4);
    f[4] = 2;
    f[5] = (uint8_t)(info >> 8);
    f[6] = (uint8_t)info;
    f[8] = 16;
    for (int i = 0; i < 8; i++)
        f[9 + i] = (uint8_t)(ap->replay >> (56 - 8 * i));
    if (nonce)
        memcpy(f + 17, nonce, 32);
    f[65] = 0x2A;                                               /* RSC */
    f[97] = (uint8_t)(kd_len >> 8);
    f[98] = (uint8_t)kd_len;
    if (kd_len)
        memcpy(f + 99, keydata, kd_len);
    if (mic) {
        uint8_t h[20];
        hmac_sha1(ap->ptk, 16, f, len, h);
        memcpy(f + 81, h, 16);
    }
    return len;
}

/* GTK KDE (and the AP's RSN element), padded and wrapped with the KEK. */
static uint32_t ap_keydata(struct ap *ap, uint8_t *out, int with_ie, int idx, const uint8_t *gtk)
{
    uint8_t kd[96];
    uint32_t n = 0;
    if (with_ie) {
        memcpy(kd, ap->ie, ap->ie_len);
        n = ap->ie_len;
    }
    kd[n++] = 0xdd;
    kd[n++] = 22;
    kd[n++] = 0x00;
    kd[n++] = 0x0f;
    kd[n++] = 0xac;
    kd[n++] = 1;
    kd[n++] = (uint8_t)idx;
    kd[n++] = 0;
    memcpy(kd + n, gtk, 16);
    n += 16;
    if (n % 8) {
        kd[n++] = 0xdd;
        while (n % 8)
            kd[n++] = 0;
    }
    aes_wrap(ap->ptk + 16, kd, n, out);
    return n + 8;
}

static void ap_derive(struct ap *ap, const uint8_t *snonce)
{
    uint8_t data[76];
    int a = memcmp(ap->aa, ap->spa, 6) < 0, b = memcmp(ap->anonce, snonce, 32) < 0;
    memcpy(data, a ? ap->aa : ap->spa, 6);
    memcpy(data + 6, a ? ap->spa : ap->aa, 6);
    memcpy(data + 12, b ? ap->anonce : snonce, 32);
    memcpy(data + 44, b ? snonce : ap->anonce, 32);
    wpa_prf_sha1(ap->pmk, 32, "Pairwise key expansion", data, sizeof(data), ap->ptk, 48);
}

static int mic_valid(const uint8_t *kck, const uint8_t *f, uint32_t len)
{
    uint8_t copy[300], h[20];
    memcpy(copy, f, len);
    memset(copy + 81, 0, 16);
    hmac_sha1(kck, 16, copy, len, h);
    return !memcmp(h, f + 81, 16);
}

static void test_handshake(void)
{
    struct ap ap = { .aa = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 },
                     .spa = { 0x02, 0xaa, 0xbb, 0xcc, 0xdd, 0xee }, .replay = 1 };
    pbkdf2_sha1("correct horse", 13, (const uint8_t *)"HomeNet", 7, 4096, ap.pmk, 32);
    for (int i = 0; i < 32; i++)
        ap.anonce[i] = (uint8_t)(i * 7);
    ap.ie_len = rsn_build(ap.ie);
    uint8_t own[64];
    uint32_t own_len = rsn_build(own);

    struct wpa_sm sm;
    struct wpa_result r;
    wpa_start(&sm, ap.pmk, ap.aa, ap.spa, own, own_len, ap.ie, ap.ie_len);
    uint8_t f[300], kd[128];

    /* A message 3 before message 1 is out of order. */
    uint8_t gtk[16];
    for (int i = 0; i < 16; i++)
        gtk[i] = (uint8_t)(0xC0 + i);
    uint32_t len = ap_frame(&ap, f, 0x13CA, ap.anonce, kd, 0, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == WPA_ESTATE);

    /* Message 1 -> message 2 carrying our RSN element, MIC by the PTK the AP derives too. */
    len = ap_frame(&ap, f, 0x008A, ap.anonce, NULL, 0, 0);
    CHECK(wpa_rx(&sm, f, len, &r) == 0);
    CHECK(r.reply_len == 99 + own_len && !r.install_ptk);
    uint8_t snonce[32];
    memset(snonce, 0x5A, 32);
    CHECK(!memcmp(r.reply + 17, snonce, 32));
    CHECK(r.reply[5] == 0x01 && r.reply[6] == 0x0A);            /* MIC, pairwise, version 2 */
    CHECK(!memcmp(r.reply + 99, own, own_len));
    ap_derive(&ap, snonce);
    CHECK(!memcmp(ap.ptk, sm.tptk, 48));                        /* aside until message 3 */
    CHECK(mic_valid(ap.ptk, r.reply, r.reply_len));

    /* Message 3 with a wrong ANonce is refused; with a bad MIC too. */
    uint8_t other[32];
    memset(other, 1, 32);
    ap.replay = 2;
    uint32_t kdl = ap_keydata(&ap, kd, 1, 1, gtk);
    len = ap_frame(&ap, f, 0x13CA, other, kd, kdl, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == WPA_ESTATE);
    len = ap_frame(&ap, f, 0x13CA, ap.anonce, kd, kdl, 1);
    f[90] ^= 0x40;
    CHECK(wpa_rx(&sm, f, len, &r) == WPA_EMIC);

    /* A different RSN element in message 3 than in the beacon: refused. */
    struct ap bad = ap;
    bad.ie[19] = 1;                                             /* AKM 802.1X */
    uint8_t kd2[128];
    uint32_t kdl2 = ap_keydata(&bad, kd2, 1, 1, gtk);
    len = ap_frame(&ap, f, 0x13CA, ap.anonce, kd2, kdl2, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == WPA_EIE && !r.install_ptk && !r.reply_len);

    /* The real message 3: message 4, the PTK and the GTK. */
    len = ap_frame(&ap, f, 0x13CA, ap.anonce, kd, kdl, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == 0);
    CHECK(r.install_ptk && r.install_gtk && r.connected);
    CHECK(r.gtk_idx == 1 && r.gtk_len == 16 && !memcmp(r.gtk, gtk, 16) && r.gtk_rsc[0] == 0x2A);
    CHECK(r.reply_len == 99 && r.reply[5] == 0x03 && r.reply[6] == 0x0A);
    CHECK(r.reply[16] == 2);                                    /* the replay counter echoed */
    CHECK(mic_valid(ap.ptk, r.reply, r.reply_len));
    CHECK(!memcmp(WPA_TK(&sm), ap.ptk + 32, 16));

    /* The same message 3 again is a replay. */
    CHECK(wpa_rx(&sm, f, len, &r) == WPA_EREPLAY);

    /* Our message 4 was lost, so the AP retransmits message 3 with a new
     * counter: message 4 again, but no key is installed a second time. */
    ap.replay = 3;
    len = ap_frame(&ap, f, 0x13CA, ap.anonce, kd, kdl, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == 0);
    CHECK(!r.install_ptk && !r.install_gtk && !r.connected);
    CHECK(r.reply_len == 99 && r.reply[5] == 0x03 && r.reply[6] == 0x0A && r.reply[16] == 3);
    CHECK(mic_valid(ap.ptk, r.reply, r.reply_len));

    /* A forged message 1 (it carries no MIC) leaves the keys in use alone. */
    uint8_t live[48], forged[32];
    memcpy(live, sm.ptk, 48);
    memset(forged, 0xEE, 32);
    ap.replay = 4;
    nonce_byte = 0x77;
    len = ap_frame(&ap, f, 0x008A, forged, NULL, 0, 0);
    CHECK(wpa_rx(&sm, f, len, &r) == 0 && r.reply_len && !r.install_ptk);
    CHECK(!memcmp(sm.ptk, live, 48) && sm.state == WPA_DONE);
    nonce_byte = 0x5A;

    /* Group rekey: key 2. */
    for (int i = 0; i < 16; i++)
        gtk[i] = (uint8_t)(0x30 + i);
    ap.replay = 5;
    kdl = ap_keydata(&ap, kd, 0, 2, gtk);
    len = ap_frame(&ap, f, 0x1382, NULL, kd, kdl, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == 0);
    CHECK(r.install_gtk && !r.install_ptk && !r.connected && r.gtk_idx == 2);
    CHECK(!memcmp(r.gtk, gtk, 16));
    CHECK(r.reply_len == 99 && r.reply[5] == 0x03 && r.reply[6] == 0x02);
    CHECK(mic_valid(ap.ptk, r.reply, r.reply_len));

    /* A real pairwise rekey: the new keys go in once message 3 proves them. */
    ap.anonce[0] ^= 0xFF;
    ap.replay = 6;
    len = ap_frame(&ap, f, 0x008A, ap.anonce, NULL, 0, 0);
    CHECK(wpa_rx(&sm, f, len, &r) == 0 && !r.install_ptk && !memcmp(sm.ptk, live, 48));
    ap_derive(&ap, snonce);
    CHECK(mic_valid(ap.ptk, r.reply, r.reply_len));
    ap.replay = 7;
    kdl = ap_keydata(&ap, kd, 1, 1, gtk);
    len = ap_frame(&ap, f, 0x13CA, ap.anonce, kd, kdl, 1);
    CHECK(wpa_rx(&sm, f, len, &r) == 0 && r.install_ptk && r.install_gtk && !r.connected);
    CHECK(!memcmp(sm.ptk, ap.ptk, 48) && memcmp(sm.ptk, live, 48));

    /* Truncated and foreign frames are dropped. */
    CHECK(wpa_rx(&sm, f, 50, &r) == WPA_EBADFRAME);
    f[4] = 254;                                                 /* WPA1 descriptor */
    CHECK(wpa_rx(&sm, f, len, &r) == WPA_EBADFRAME);
}

/* The firmware file we ship, parsed: the counts are those of iwlwifi-ty-a0-gf-a0-89. */
static void test_firmware(void)
{
    FILE *f = fopen("firmware/iwlwifi/iwlwifi-ty-a0-gf-a0-89.ucode", "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    static uint8_t buf[4 << 20];
    uint32_t n = (uint32_t)fread(buf, 1, sizeof(buf), f);
    fclose(f);
    struct iwl_fw fw;
    CHECK(iwl_fw_parse(buf, n, &fw) == 0);
    CHECK(fw.api_ver == 89 && fw.num_cpus == 2);
    CHECK(fw.lmac.n == 15 && fw.umac.n == 16 && fw.paging.n == 25 && fw.paging_size == 761856);
    CHECK(fw.lmac.sec[0].addr == 0x00440000 && fw.umac.sec[0].addr == 0x80440000);
    CHECK(fw.iml_len == 13944 && fw.n_cmd_versions == 218);
    CHECK(fw.umac_error_addr && fw.lmac_error_addr);
    CHECK(iwl_fw_cmd_ver(&fw, 1, 0x0D) == 17);                  /* SCAN_REQ_UMAC */
    uint32_t total = 0;
    for (uint32_t i = 0; i < fw.paging.n; i++)
        total += fw.paging.sec[i].len;
    CHECK(total > fw.paging_size);              /* the pages plus their 1656-byte signature */
    /* ALIVE releases the file; command versions must remain usable. */
    memset(buf, 0, n);
    CHECK(iwl_fw_cmd_ver(&fw, 1, 0x0D) == 17);
    f = fopen("firmware/iwlwifi/iwlwifi-ty-a0-gf-a0-89.ucode", "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    CHECK(fread(buf, 1, n, f) == n);
    fclose(f);
    CHECK(iwl_fw_parse(buf, 100, &fw) == IWL_FW_ETRUNC || iwl_fw_parse(buf, 100, &fw) == IWL_FW_EHEADER);
    buf[4] ^= 1;
    CHECK(iwl_fw_parse(buf, n, &fw) == IWL_FW_EHEADER);
}

/* The PNVM we ship: each of its four SKUs finds its two chunks; others none. */
static void test_pnvm(void)
{
    FILE *f = fopen("firmware/iwlwifi/iwlwifi-ty-a0-gf-a0.pnvm", "rb");
    CHECK(f != NULL);
    if (!f)
        return;
    static uint8_t buf[256 << 10];
    uint32_t n = (uint32_t)fread(buf, 1, sizeof(buf), f);
    fclose(f);
    static const uint32_t skus[4] = { 0x000610D0, 0x000510D0, 0x004510D0, 0x004610D0 };
    const uint8_t *chunks[IWL_PNVM_MAX_CHUNKS], *first[4] = { 0 };
    uint32_t lens[IWL_PNVM_MAX_CHUNKS];
    for (int i = 0; i < 4; i++) {
        uint32_t sku[3] = { skus[i], 0, 0 };
        int c = iwl_pnvm_find(buf, n, sku, 0x42, 0x10D, chunks, lens);
        CHECK(c == 2);
        CHECK(lens[0] == 1656 && lens[1] > 10000);
        CHECK(chunks[1] > chunks[0] && chunks[1] + lens[1] <= buf + n);
        first[i] = chunks[0];
    }
    CHECK(first[0] != first[1] && first[2] != first[3]);    /* each its own section */
    uint32_t other[3] = { 0x000710D0, 0, 0 };
    CHECK(iwl_pnvm_find(buf, n, other, 0x42, 0x10D, chunks, lens) == 0);
    uint32_t sku[3] = { skus[3], 0, 0 };
    CHECK(iwl_pnvm_find(buf, n, sku, 0x42, 0x10E, chunks, lens) == 0);     /* another radio */
    CHECK(iwl_pnvm_find(buf, n, sku, 0x41, 0x10D, chunks, lens) == 0);
}

static void fw_put32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t fw_tlv(uint8_t *buf, uint32_t off, uint32_t type,
                       const void *payload, uint32_t len)
{
    fw_put32(buf + off, type);
    fw_put32(buf + off + 4, len);
    if (len)
        memcpy(buf + off + 8, payload, len);
    uint32_t padded = (len + 3) & ~3u;
    memset(buf + off + 8 + len, 0, padded - len);
    return off + 8 + padded;
}

static void test_firmware_invalid(void)
{
    uint8_t buf[4096] = { 0 };
    struct iwl_fw fw;
    fw_put32(buf + 4, TLV_MAGIC);
    uint32_t n = HDR_LEN;
    uint8_t section[5] = { 0, 0, 0x44, 0, 0xAA };
    uint8_t sep[4];
    fw_put32(sep, SEP_CPU1_CPU2);
    n = fw_tlv(buf, n, TLV_SEC_RT, section, sizeof(section));
    n = fw_tlv(buf, n, TLV_SEC_RT, sep, sizeof(sep));
    n = fw_tlv(buf, n, TLV_SEC_RT, section, sizeof(section));
    n = fw_tlv(buf, n, TLV_IML, "IML!", 4);
    CHECK(iwl_fw_parse(buf, n, &fw) == 0);
    /* Every partial header, payload, or padding must be rejected. */
    uint32_t end = fw_tlv(buf, n, 0xDEAD, "abcde", 5);
    CHECK(iwl_fw_parse(buf, end, &fw) == 0);
    for (uint32_t cut = n + 1; cut < end; cut++)
        CHECK(iwl_fw_parse(buf, cut, &fw) == IWL_FW_ETRUNC);
    fw_put32(buf + n + 4, UINT32_MAX);
    CHECK(iwl_fw_parse(buf, end, &fw) == IWL_FW_ETRUNC);
    end = fw_tlv(buf, n, TLV_CMD_VERSIONS, "abc", 3);
    CHECK(iwl_fw_parse(buf, end, &fw) == IWL_FW_EFORMAT);
    uint8_t versions[(IWL_FW_MAX_CMD_VERSIONS + 1) * 4] = { 0 };
    end = fw_tlv(buf, n, TLV_CMD_VERSIONS, versions, sizeof(versions));
    CHECK(iwl_fw_parse(buf, end, &fw) == IWL_FW_ETOOMANY);
    end = fw_tlv(buf, n, TLV_IML, NULL, 0);
    CHECK(iwl_fw_parse(buf, end, &fw) == IWL_FW_EFORMAT);
}

static void test_pnvm_invalid(void)
{
    uint8_t buf[4096] = { 0 };
    uint32_t sku[3] = { 0x610D0, 0, 0 };
    uint8_t sku_bytes[12] = { 0 }, hw[4] = { 0x42, 0, 0x0D, 1 };
    uint8_t section[5] = { 0, 0, 0, 0, 0xAA };
    fw_put32(sku_bytes, sku[0]);
    const uint8_t *chunks[IWL_PNVM_MAX_CHUNKS];
    uint32_t lens[IWL_PNVM_MAX_CHUNKS];
    uint32_t n = fw_tlv(buf, 0, PNVM_TLV_SKU, sku_bytes, sizeof(sku_bytes));
    n = fw_tlv(buf, n, PNVM_TLV_HW_TYPE, hw, sizeof(hw));
    uint32_t start = n;
    n = fw_tlv(buf, n, PNVM_TLV_SEC_RT, section, sizeof(section));
    CHECK(iwl_pnvm_find(buf, n, sku, 0x42, 0x10D, chunks, lens) == 1);
    for (uint32_t cut = start + 1; cut < n; cut++)
        CHECK(iwl_pnvm_find(buf, cut, sku, 0x42, 0x10D, chunks, lens) == IWL_FW_ETRUNC);
    /* Never return a partial chunk set if a later record is damaged. */
    CHECK(iwl_pnvm_find(buf, n + 1, sku, 0x42, 0x10D, chunks, lens) == IWL_FW_ETRUNC);
    for (int i = 1; i < IWL_PNVM_MAX_CHUNKS; i++)
        n = fw_tlv(buf, n, PNVM_TLV_SEC_RT, section, sizeof(section));
    CHECK(iwl_pnvm_find(buf, n, sku, 0x42, 0x10D, chunks, lens) == IWL_PNVM_MAX_CHUNKS);
    n = fw_tlv(buf, n, PNVM_TLV_SEC_RT, section, sizeof(section));
    CHECK(iwl_pnvm_find(buf, n, sku, 0x42, 0x10D, chunks, lens) == IWL_FW_ETOOMANY);
}

int main(void)
{
    test_firmware_invalid();
    test_pnvm_invalid();
    test_pnvm();
    test_crypto();
    test_firmware();
    test_rsn();
    test_handshake();
    if (failures) {
        printf("kernel-wifi: %d FAILED\n", failures);
        return 1;
    }
    printf("kernel-wifi: all passed\n");
    return 0;
}
