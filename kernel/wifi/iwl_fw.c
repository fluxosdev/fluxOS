/*
 * The Intel firmware file (iwlwifi-*.ucode): a header and a list of
 * type-length-value records. What we take from it: the image loader
 * (IML), the code and data sections of the two processors (LMAC, then
 * UMAC, split by separator sections) and the paged part, the versions of
 * every command the firmware speaks, and its capability and API flags.
 * The layout is that of Linux's iwlwifi (fw/file.h, GPL-2.0 OR
 * BSD-3-Clause). Pure parsing, over the file in memory: sections point
 * into the buffer, which the caller keeps until it has copied them.
 */
#include <string.h>
#include "iwl_fw.h"

#define TLV_MAGIC 0x0A4C5749            /* "IWL\n" */
#define HDR_HUMAN 64
#define HDR_LEN   (4 + 4 + HDR_HUMAN + 4 + 4 + 8)

#define TLV_PROBE_MAX_LEN   6
#define TLV_FLAGS           18
#define TLV_SEC_RT          19
#define TLV_PHY_SKU         23
#define TLV_NUM_OF_CPU      27
#define TLV_API_CHANGES_SET 29
#define TLV_ENABLED_CAPA    30
#define TLV_N_SCAN_CHANNELS 31
#define TLV_PAGING          32
#define TLV_FW_VERSION      36
#define TLV_CMD_VERSIONS    48
#define TLV_IML             52
#define TLV_UMAC_DEBUG      54
#define TLV_LMAC_DEBUG      55
#define TLV_FW_NUM_STATIONS 0x100

#define SEP_CPU1_CPU2 0xFFFFCCCCu
#define SEP_PAGING    0xAAAABBBBu
#define FW_ADDR_CACHE_CONTROL 0xC0000000u

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

/* Check the header, payload AND alignment before advancing. Subtractions
 * avoid wrapping a length or offset supplied by a damaged file. */
static int tlv_size(const uint8_t *p, uint32_t remaining, uint32_t *size)
{
    if (remaining < 8)
        return IWL_FW_ETRUNC;
    uint32_t len = le32(p + 4);
    uint32_t pad = (0u - len) & 3u;
    if (len > remaining - 8 || pad > remaining - 8 - len)
        return IWL_FW_ETRUNC;
    *size = 8 + len + pad;
    return 0;
}

int iwl_fw_parse(const uint8_t *data, uint32_t len, struct iwl_fw *fw)
{
    memset(fw, 0, sizeof(*fw));
    if (len < HDR_LEN || le32(data) != 0 || le32(data + 4) != TLV_MAGIC)
        return IWL_FW_EHEADER;
    memcpy(fw->human, data + 8, HDR_HUMAN);
    fw->human[HDR_HUMAN - 1] = 0;
    fw->api_ver = le32(data + 8 + HDR_HUMAN) & 0xFF;
    int part = 0;                       /* 0 LMAC, 1 UMAC, 2 paging */
    uint32_t off = HDR_LEN;
    while (off < len) {
        uint32_t size;
        if (tlv_size(data + off, len - off, &size) < 0)
            return IWL_FW_ETRUNC;
        uint32_t type = le32(data + off), tlen = le32(data + off + 4);
        const uint8_t *v = data + off + 8;
        if (tlen > len - off - 8)
            return IWL_FW_ETRUNC;
        switch (type) {
        case TLV_SEC_RT: {
            if (tlen < 4)
                return IWL_FW_ETRUNC;
            uint32_t addr = le32(v);
            if (addr == SEP_CPU1_CPU2) {
                part = 1;
                break;
            }
            if (addr == SEP_PAGING) {
                part = 2;
                break;
            }
            struct iwl_fw_sec_list *l = part == 0 ? &fw->lmac : part == 1 ? &fw->umac : &fw->paging;
            if (l->n == IWL_FW_MAX_SECS)
                return IWL_FW_ETOOMANY;
            l->sec[l->n].addr = addr;
            l->sec[l->n].data = v + 4;
            l->sec[l->n].len = tlen - 4;
            l->n++;
            break;
        }
        case TLV_IML:
            if (!tlen)
                return IWL_FW_EFORMAT;
            fw->iml = v;
            fw->iml_len = tlen;
            break;
        case TLV_CMD_VERSIONS:
            if (tlen % 4)
                return IWL_FW_EFORMAT;
            if (tlen / 4 > IWL_FW_MAX_CMD_VERSIONS)
                return IWL_FW_ETOOMANY;
            memcpy(fw->cmd_versions, v, tlen);
            fw->n_cmd_versions = tlen / 4;
            break;
        case TLV_API_CHANGES_SET:
        case TLV_ENABLED_CAPA:
            if (tlen >= 8) {
                uint32_t idx = le32(v), bits = le32(v + 4);
                uint32_t *set = type == TLV_API_CHANGES_SET ? fw->api : fw->capa;
                if (idx < IWL_FW_FLAG_WORDS)
                    set[idx] = bits;
            }
            break;
        case TLV_FLAGS:
            if (tlen >= 4)
                fw->ucode_flags = le32(v);
            break;
        case TLV_NUM_OF_CPU:
            if (tlen >= 4)
                fw->num_cpus = le32(v);
            break;
        case TLV_PAGING:
            if (tlen >= 4)
                fw->paging_size = le32(v);
            break;
        case TLV_N_SCAN_CHANNELS:
            if (tlen >= 4)
                fw->n_scan_channels = le32(v);
            break;
        case TLV_PHY_SKU:
            if (tlen >= 4)
                fw->phy_sku = le32(v);
            break;
        case TLV_FW_VERSION:
            if (tlen >= 12) {
                fw->major = le32(v);
                fw->minor = le32(v + 4);
                fw->local = le32(v + 8);
            }
            break;
        case TLV_UMAC_DEBUG:
            if (tlen >= 8)
                fw->umac_error_addr = le32(v) & ~FW_ADDR_CACHE_CONTROL;
            break;
        case TLV_LMAC_DEBUG:
            if (tlen >= 32)
                fw->lmac_error_addr = le32(v) & ~FW_ADDR_CACHE_CONTROL;
            break;
        case TLV_FW_NUM_STATIONS:
            if (tlen >= 4)
                fw->num_stations = le32(v);
            break;
        default:
            break;
        }
        off += size;
    }
    if (!fw->lmac.n || !fw->umac.n || !fw->iml)
        return IWL_FW_EMISSING;
    return 0;
}

int iwl_fw_cmd_ver(const struct iwl_fw *fw, uint8_t group, uint8_t cmd)
{
    for (uint32_t i = 0; i < fw->n_cmd_versions; i++) {
        const uint8_t *e = fw->cmd_versions + 4 * i;
        if (e[0] == cmd && e[1] == group)
            return e[2];
    }
    return 0;
}

int iwl_fw_notif_ver(const struct iwl_fw *fw, uint8_t group, uint8_t cmd)
{
    for (uint32_t i = 0; i < fw->n_cmd_versions; i++) {
        const uint8_t *e = fw->cmd_versions + 4 * i;
        if (e[0] == cmd && e[1] == group)
            return e[3];
    }
    return 0;
}

int iwl_fw_has_capa(const struct iwl_fw *fw, uint32_t bit)
{
    return bit / 32 < IWL_FW_FLAG_WORDS && (fw->capa[bit / 32] >> (bit % 32) & 1);
}

int iwl_fw_has_api(const struct iwl_fw *fw, uint32_t bit)
{
    return bit / 32 < IWL_FW_FLAG_WORDS && (fw->api[bit / 32] >> (bit % 32) & 1);
}

const char *iwl_fw_strerror(int err)
{
    switch (err) {
    case IWL_FW_EHEADER:  return "not an Intel firmware file";
    case IWL_FW_ETRUNC:   return "firmware file truncated";
    case IWL_FW_ETOOMANY: return "firmware has more sections than expected";
    case IWL_FW_EMISSING: return "firmware lacks its image loader or code sections";
    case IWL_FW_EFORMAT:  return "invalid firmware record";
    default:              return "bad firmware file";
    }
}

/* ---- the PNVM file ---- */

#define PNVM_TLV_SEC_RT  19
#define PNVM_TLV_HW_TYPE 58
#define PNVM_TLV_SKU     64

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

int iwl_pnvm_find(const uint8_t *buf, uint32_t len, const uint32_t sku[3], uint32_t mac_type,
                  uint32_t rf_id, const uint8_t *chunks[], uint32_t lens[])
{
    /* Sections, each from a PNVM_SKU record to the next: ours matches the
     * SKU from ALIVE and the card's MAC and RF types. */
    int n = 0, in_section = 0, hw_match = 0;
    for (uint32_t off = 0; off < len;) {
        uint32_t size;
        if (tlv_size(buf + off, len - off, &size) < 0)
            return IWL_FW_ETRUNC;
        uint32_t type = le32(buf + off), tlen = le32(buf + off + 4);
        const uint8_t *v = buf + off + 8;
        off += size;
        if (type == PNVM_TLV_SKU) {
            if (in_section && hw_match && n > 0)
                return n;
            in_section = tlen >= 12 && le32(v) == sku[0] && le32(v + 4) == sku[1] &&
                         le32(v + 8) == sku[2];
            hw_match = 0;
            n = 0;
        } else if (!in_section) {
            continue;
        } else if (type == PNVM_TLV_HW_TYPE && tlen >= 4 && !hw_match) {
            hw_match = le16(v) == mac_type && le16(v + 2) == rf_id;
        } else if (type == PNVM_TLV_SEC_RT) {
            if (tlen < 4)
                return IWL_FW_ETRUNC;
            if (le32(v) == 0xDDDDEEEE)
                continue;
            if (tlen == 4)
                return IWL_FW_EFORMAT;
            if (n == IWL_PNVM_MAX_CHUNKS)
                return IWL_FW_ETOOMANY;
            chunks[n] = v + 4;
            lens[n] = tlen - 4;
            n++;
        }
    }
    return in_section && hw_match ? n : 0;  /* the last section in the file */
}
