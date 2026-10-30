/* The Intel firmware file, parsed (iwl_fw.c). */
#ifndef IWL_FW_H
#define IWL_FW_H

#include <stdint.h>

#define IWL_FW_MAX_SECS   32
#define IWL_FW_FLAG_WORDS 8             /* 256 capability bits, 256 API bits */
#define IWL_FW_MAX_CMD_VERSIONS 512

struct iwl_fw_sec {
    uint32_t addr;                      /* where the firmware wants it */
    const uint8_t *data;
    uint32_t len;
};

struct iwl_fw_sec_list {
    struct iwl_fw_sec sec[IWL_FW_MAX_SECS];
    uint32_t n;
};

struct iwl_fw {
    char human[64];                     /* "release/core86:..." */
    uint32_t api_ver;                   /* 89 */
    uint32_t major, minor, local;       /* the version the firmware reports */
    const uint8_t *iml;                 /* image loader */
    uint32_t iml_len;
    struct iwl_fw_sec_list lmac, umac, paging;
    uint32_t paging_size;
    /* Kept after the firmware file is freed at ALIVE. */
    uint8_t cmd_versions[IWL_FW_MAX_CMD_VERSIONS * 4];
    uint32_t n_cmd_versions;
    uint32_t capa[IWL_FW_FLAG_WORDS], api[IWL_FW_FLAG_WORDS];
    uint32_t ucode_flags;
    uint32_t num_cpus, n_scan_channels, phy_sku, num_stations;
    uint32_t umac_error_addr, lmac_error_addr;
};

int iwl_fw_parse(const uint8_t *data, uint32_t len, struct iwl_fw *fw);
int iwl_fw_cmd_ver(const struct iwl_fw *fw, uint8_t group, uint8_t cmd);   /* 0: not listed */
int iwl_fw_notif_ver(const struct iwl_fw *fw, uint8_t group, uint8_t cmd);
int iwl_fw_has_capa(const struct iwl_fw *fw, uint32_t bit);
int iwl_fw_has_api(const struct iwl_fw *fw, uint32_t bit);
const char *iwl_fw_strerror(int err);

/* The chunks of the PNVM file's section for this card (they point into
 * buf): how many, 0 if there is none, or IWL_FW_E* on malformed input.
 * Outputs must only be consumed when the return value is positive. */
#define IWL_PNVM_MAX_CHUNKS 64
int iwl_pnvm_find(const uint8_t *buf, uint32_t len, const uint32_t sku[3], uint32_t mac_type,
                  uint32_t rf_id, const uint8_t *chunks[], uint32_t lens[]);

#define IWL_FW_EHEADER  -1
#define IWL_FW_ETRUNC   -2
#define IWL_FW_ETOOMANY -3
#define IWL_FW_EMISSING -4
#define IWL_FW_EFORMAT  -5

#endif
