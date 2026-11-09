/*
 * Intel AX210: the firmware's side. Bring-up once the transport has run
 * the firmware (ALIVE, the PNVM, the init commands Linux's iwlmvm sends
 * for this device when there are no BIOS tables), reading the NVM, the
 * crash dump, and the notifications nobody waits for. The station
 * operations (scan, join, TX, keys) are in iwl_sta.c.
 *
 * The sequence and every command's layout are those of iwlmvm for
 * firmware API 89 with no ACPI/UEFI tables, no power save and a single
 * RX queue; commands whose payload depends on the version use the
 * version this firmware lists.
 */
#include "kernel.h"
#include "drivers.h"
#include "mm.h"
#include "proc.h"
#include "vfs.h"
#include "iwl.h"
#include "wifi.h"

#define FW_PATH   "/lib/firmware/iwlwifi-ty-a0-gf-a0-89.ucode"
#define PNVM_PATH "/lib/firmware/iwlwifi-ty-a0-gf-a0.pnvm"

#define ALIVE_TIMEOUT 2000              /* ms */
#define INIT_TIMEOUT  2000
#define PNVM_TIMEOUT  250

#define ALIVE_LEN       144
#define ALIVE_STATUS_OK 0xCAFE
#define NVM_INFO_LEN    468

struct iwl_dev iwl;

static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | p[1] << 8);
}

/* ---- files ---- */

/* A whole file into contiguous pages (they are DMA-able too). 0, or -errno. */
int iwl_read_file(const char *path, uint8_t **out, uint32_t *len, uint32_t *pages)
{
    vfs_lock();
    struct vnode *vn;
    int r = vfs_resolve(path, NULL, &vn);
    if (r < 0 || vn->type != VN_FILE) {
        vfs_unlock();
        return r < 0 ? r : -ENOENT;
    }
    uint32_t size = vn->size;
    if (!size || size > (16u << 20)) {
        vfs_unlock();
        return -EINVAL;
    }
    uint32_t n = (size + PAGE_SIZE - 1) / PAGE_SIZE;
    uint8_t *buf = n ? (uint8_t *)pmm_alloc_contig(n) : NULL;
    if (!buf) {
        vfs_unlock();
        return -ENOMEM;
    }
    r = vfs_read(vn, buf, 0, size);
    vfs_unlock();
    if (r != (int)size) {
        for (uint32_t i = 0; i < n; i++)
            pmm_free((uint32_t)buf + i * PAGE_SIZE);
        return r < 0 ? r : -EIO;
    }
    *out = buf;
    *len = size;
    *pages = n;
    return 0;
}

void iwl_free_file(uint8_t *buf, uint32_t pages)
{
    for (uint32_t i = 0; buf && i < pages; i++)
        pmm_free((uint32_t)buf + i * PAGE_SIZE);
}

/* ---- crash dump ---- */

static const char *assert_name(uint32_t id)
{
    static const struct { uint32_t id; const char *name; } names[] = {
        { 0x34, "NMI_INTERRUPT_WDG" }, { 0x35, "SYSASSERT" },
        { 0x37, "UCODE_VERSION_MISMATCH" }, { 0x38, "BAD_COMMAND" }, { 0x39, "BAD_COMMAND" },
        { 0x3C, "NMI_INTERRUPT_DATA_ACTION_PT" }, { 0x3D, "FATAL_ERROR" },
        { 0x46, "NMI_TRM_HW_ERR" }, { 0x4C, "NMI_INTERRUPT_TRM" },
        { 0x54, "NMI_INTERRUPT_BREAK_POINT" }, { 0x5C, "NMI_INTERRUPT_WDG_RXF_FULL" },
        { 0x64, "NMI_INTERRUPT_WDG_NO_RBD_RXF_FULL" }, { 0x66, "NMI_INTERRUPT_HOST" },
        { 0x70, "NMI_INTERRUPT_LMAC_FATAL" }, { 0x71, "NMI_INTERRUPT_UMAC_FATAL" },
        { 0x73, "NMI_INTERRUPT_OTHER_LMAC_FATAL" }, { 0x7C, "NMI_INTERRUPT_ACTION_PT" },
        { 0x84, "NMI_INTERRUPT_UNKNOWN" }, { 0x86, "NMI_INTERRUPT_INST_ACTION_PT" },
        { 0x88, "NMI_INTERRUPT_PREG" }, { 0x0010070D, "PNVM_MISSING" },
    };
    id &= ~0xF0000000u;
    for (uint32_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (names[i].id == id)
            return names[i].name;
    return "ADVANCED_SYSASSERT";
}

static int unreachable(uint32_t v)
{
    return (v & ~0xFu) == 0xA5A5A5A0 || (v & ~0xFu) == 0x5A5A5A50;
}

/* The firmware's error logs, in the form Linux prints them. */
void iwl_dump_errors(const char *why)
{
    klog("iwl: firmware error (%s), firmware %u.%08x.%u\n", why, iwl.fw.major, iwl.fw.minor,
         iwl.fw.local);
    uint32_t t[38];
    static const char *lmac_names[38] = {
        "valid", "error_id", "trm_hw_status0", "trm_hw_status1", "blink2", "ilink1", "ilink2",
        "data1", "data2", "data3", "bcon_time", "tsf_low", "tsf_hi", "gp1", "gp2", "fw_rev_type",
        "major", "minor", "hw_ver", "brd_ver", "log_pc", "frame_ptr", "stack_ptr", "hcmd",
        "isr0", "isr1", "isr2", "isr3", "isr4", "last_cmd_id", "wait_event", "l2p_control",
        "l2p_duration", "l2p_mhvalid", "l2p_addr_match", "lmpm_pmg_sel", "u_timestamp",
        "flow_handler",
    };
    static const char *umac_names[15] = {
        "valid", "error_id", "blink1", "blink2", "ilink1", "ilink2", "data1", "data2", "data3",
        "umac_major", "umac_minor", "frame_pointer", "stack_pointer", "cmd_header",
        "nic_isr_pref",
    };
    uint32_t lmac[2] = { iwl.lmac_err ? iwl.lmac_err : iwl.fw.lmac_error_addr, iwl.lmac2_err };
    for (int c = 0; c < 2; c++) {
        if (!lmac[c])
            continue;
        if (iwl_read_mem(lmac[c], t, 38) < 0 || unreachable(t[0])) {
            klog("iwl: LMAC%d error log unreadable (device inaccessible)\n", c + 1);
            continue;
        }
        if (!t[0])
            continue;
        klog("iwl: Start IWL Error Log Dump (LMAC%d):\n", c + 1);
        klog("iwl: 0x%08X | %s\n", t[1], assert_name(t[1]));
        for (int i = 2; i < 38; i++)
            klog("iwl: 0x%08X | %s\n", t[i], lmac_names[i]);
    }
    uint32_t umac = iwl.umac_err ? iwl.umac_err : iwl.fw.umac_error_addr;
    if (umac && iwl_read_mem(umac, t, 15) == 0 && !unreachable(t[0]) && t[0]) {
        klog("iwl: Start IWL Error Log Dump (UMAC):\n");
        klog("iwl: 0x%08X | %s\n", t[1], assert_name(t[1]));
        if ((t[1] & ~0xF0000000u) == 0x0010070D)
            klog("iwl: PNVM data is missing: install " PNVM_PATH "\n");
        for (int i = 2; i < 15; i++)
            klog("iwl: 0x%08X | %s\n", t[i], umac_names[i]);
    }
    uint32_t sb2 = iwl_read_prph(UMAG_SB_CPU_2_STATUS + UMAC_PRPH_OFFSET);
    klog("iwl: IML/ROM status 0x%08X data 0x%08X, FSEQ error 0x%08X, CFG flag 0x%08X\n", sb2,
         iwl_read_prph(UMAG_SB_CPU_1_STATUS + UMAC_PRPH_OFFSET), iwl_read_prph(FSEQ_ERROR_CODE),
         iwl_read_prph(SB_MODIFY_CFG_FLAG + UMAC_PRPH_OFFSET));
    klog("iwl: UMAC PC 0x%08X, LMAC1 PC 0x%08X\n", iwl_read_prph(0xD05C18),
         iwl_read_prph(0xD05C1C));
    iwl_trans_dump_queues();
}

void iwl_mvm_fw_error(int hw_error)
{
    if (iwl.dead)
        return;
    iwl.dead = 1;
    iwl_dump_errors(hw_error ? "hardware error" : "firmware assert");
    wifi_hw_failed("the wireless firmware crashed (details in dmesg)");
}

/* ---- PNVM ---- */

static int load_pnvm(void)
{
    uint8_t *buf;
    uint32_t len, pages;
    int r = iwl_read_file(PNVM_PATH, &buf, &len, &pages);
    if (r < 0) {
        klog("iwl: %s missing (%d); the firmware may refuse to run\n", PNVM_PATH, r);
        return r;
    }
    const uint8_t *chunks[IWL_PNVM_MAX_CHUNKS];
    uint32_t lens[IWL_PNVM_MAX_CHUNKS];
    uint32_t mac_type = (iwl.hw_rev & 0xFFF0) >> 4, rf_id = (iwl.hw_rf_id & 0x0FFF000) >> 12;
    int n = iwl_pnvm_find(buf, len, iwl.sku_id, mac_type, rf_id, chunks, lens);
    if (n < 0) {
        klog("iwl: invalid PNVM: %s\n", iwl_fw_strerror(n));
        iwl_free_file(buf, pages);
        return -EINVAL;
    }
    int found = n > 0;
    if (!found) {
        klog("iwl: no PNVM section for SKU %08x %08x %08x (MAC type %03x, RF %03x)\n",
             iwl.sku_id[0], iwl.sku_id[1], iwl.sku_id[2], mac_type, rf_id);
        n = 0;
    }
    struct iwl_notif_wait w;
    iwl_notif_arm(&w, GRP_REG_NVM, NTF_PNVM_COMPLETE, NULL, 0);
    r = iwl_trans_set_pnvm(chunks, lens, n);   /* copies the chunks into its own memory */
    iwl_free_file(buf, pages);
    if (r < 0) {
        iwl_notif_cancel(&w);
        return r;
    }
    iwl_write_prph(UREG_DOORBELL_TO_ISR6 + UMAC_PRPH_OFFSET, UREG_DOORBELL_TO_ISR6_PNVM);
    r = iwl_notif_wait(&w, PNVM_TIMEOUT);
    if (r < 0)
        klog("iwl: timeout waiting for the PNVM to load\n");
    else
        klog("iwl: PNVM loaded (%d chunks)\n", n);
    return r;
}

/* ---- bring-up ---- */

static int send(uint8_t group, uint8_t cmd, const void *data, uint32_t len, const char *what)
{
    int r = iwl_send_cmd(group, cmd, data, len, NULL, 0, NULL);
    if (r < 0)
        klog("iwl: %s failed (%d)\n", what, r);
    return r;
}

static int read_mac(void)
{
    uint32_t regs[2][2] = { { CSR_MAC_ADDR0_STRAP, CSR_MAC_ADDR1_STRAP },
                            { CSR_MAC_ADDR0_OTP, CSR_MAC_ADDR1_OTP } };
    for (int i = 0; i < 2; i++) {
        uint32_t a0 = iwl_read32(regs[i][0]), a1 = iwl_read32(regs[i][1]);
        uint8_t m[6] = { (uint8_t)(a0 >> 24), (uint8_t)(a0 >> 16), (uint8_t)(a0 >> 8), (uint8_t)a0,
                         (uint8_t)(a1 >> 8), (uint8_t)a1 };
        if (!(m[0] & 1) && (m[0] | m[1] | m[2] | m[3] | m[4] | m[5])) {
            memcpy(iwl.mac, m, 6);
            return 0;
        }
    }
    return -1;
}

static int read_nvm(void)
{
    static uint8_t rsp[NVM_INFO_LEN + 64];
    uint32_t zero = 0, got = 0;
    int r = iwl_send_cmd(GRP_REG_NVM, CMD_NVM_GET_INFO, &zero, 4, rsp, sizeof(rsp), &got);
    if (r < 0) {
        klog("iwl: NVM_GET_INFO failed (%d)\n", r);
        return r;
    }
    if (got != NVM_INFO_LEN) {
        klog("iwl: NVM_GET_INFO: %u bytes, expected %u\n", got, NVM_INFO_LEN);
        return -EIO;
    }
    uint32_t sku = le32(rsp + 8), tx = le32(rsp + 12), rx = le32(rsp + 16);
    uint8_t fw_tx = (uint8_t)(iwl.fw.phy_sku >> 16 & 0xF), fw_rx = (uint8_t)(iwl.fw.phy_sku >> 20 & 0xF);
    iwl.tx_ant = (uint8_t)tx ? fw_tx & (uint8_t)tx : fw_tx;
    iwl.rx_ant = (uint8_t)rx ? fw_rx & (uint8_t)rx : fw_rx;
    if (!iwl.tx_ant)
        iwl.tx_ant = fw_tx;
    if (!iwl.rx_ant)
        iwl.rx_ant = fw_rx;
    iwl.band_2ghz = (sku & 1) != 0;
    iwl.band_5ghz = (sku & 2) != 0;
    iwl.lar = le32(rsp + 20) != 0 && iwl_fw_has_capa(&iwl.fw, 1);
    extern uint16_t iwl_chan_flags[110];
    uint32_t channels = le32(rsp + 24);
    if (channels > 110) return -EIO;
    for (uint32_t i = 0; i < 110; i++)
        iwl_chan_flags[i] = i < channels ? (uint16_t)le32(rsp + 28 + 4 * i) : 0;
    klog("iwl: NVM version %u, bands%s%s, antennas tx %x rx %x, LAR %s\n", le16(rsp + 4),
         iwl.band_2ghz ? " 2.4GHz" : "", iwl.band_5ghz ? " 5GHz" : "", iwl.tx_ant, iwl.rx_ant,
         iwl.lar ? "on" : "off");
    return 0;
}

/* Channel flags from MCC_UPDATE (or the NVM): which may be used, which only passively. */
#define NVM_CH_VALID  0x0001
#define NVM_CH_ACTIVE 0x0008
#define NVM_CH_RADAR  0x0010
#define IWL_NUM_CHANNELS 110

uint16_t iwl_chan_flags[IWL_NUM_CHANNELS];
int iwl_have_chan_flags;

static const uint8_t nvm_channels_5ghz[] = {
    36, 40, 44, 48, 52, 56, 60, 64, 68, 72, 76, 80, 84, 88, 92, 96, 100, 104, 108, 112, 116,
    120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173, 177, 181,
};

/* Index in the NVM channel map of a channel, or -1. */
int iwl_chan_index(int channel, int band)
{
    if (band == BAND_2GHZ)
        return channel >= 1 && channel <= 14 ? channel - 1 : -1;
    for (uint32_t i = 0; i < sizeof(nvm_channels_5ghz); i++)
        if (nvm_channels_5ghz[i] == channel)
            return 14 + (int)i;
    return -1;
}

int iwl_chan_from_index(int idx, int *band)
{
    if (idx < 14) {
        *band = BAND_2GHZ;
        return idx + 1;
    }
    if (idx < 14 + (int)sizeof(nvm_channels_5ghz)) {
        *band = BAND_5GHZ;
        return nvm_channels_5ghz[idx - 14];
    }
    return -1;
}

static int mcc_update(void)
{
    uint8_t cmd[28] = { 0 };
    cmd[0] = 'Z';                       /* mcc = 'Z' << 8 | 'Z', little endian */
    cmd[1] = 'Z';
    cmd[2] = 0x10;                      /* MCC_SOURCE_GET_CURRENT */
    static uint8_t rsp[20 + 4 * 128];
    uint32_t got = 0;
    int r = iwl_send_cmd(GRP_LONG, CMD_MCC_UPDATE, cmd, sizeof(cmd), rsp, sizeof(rsp), &got);
    if (r < 0) {
        klog("iwl: MCC_UPDATE failed (%d)\n", r);
        return r;
    }
    uint32_t n = got >= 20 ? le32(rsp + 16) : 0;
    if (got < 20 || n > 128 || got != 20 + 4 * n) {
        klog("iwl: MCC_UPDATE response of %u bytes for %u channels\n", got, n);
        return -EIO;
    }
    uint16_t mcc = le16(rsp + 4);
    if (!mcc)
        mcc = 0x3030;                   /* "00": world */
    int active = 0, passive = 0;
    for (uint32_t i = 0; i < IWL_NUM_CHANNELS; i++) {
        iwl_chan_flags[i] = i < n ? (uint16_t)le32(rsp + 20 + 4 * i) : 0;
        if (i < 51 && (iwl_chan_flags[i] & NVM_CH_VALID)) {
            if ((iwl_chan_flags[i] & NVM_CH_ACTIVE) && !(iwl_chan_flags[i] & NVM_CH_RADAR))
                active++;
            else
                passive++;
        }
    }
    iwl_have_chan_flags = 1;
    klog("iwl: regulatory domain %c%c: %d channels active, %d passive only\n", mcc >> 8,
         mcc & 0xFF, active, passive);
    return 0;
}

static void mark_all_channels_usable(void)
{
    /* No LAR: retain the device's NVM channel permissions read above. */
    iwl_have_chan_flags = 1;
}

/* Load and run the firmware, then configure it as iwlmvm does. 0, or -errno. */
int iwl_mvm_start(char *error, uint32_t error_size)
{
    uint32_t fw_len = 0;
    int r = iwl_read_file(FW_PATH, &iwl.fw_file, &fw_len, &iwl.fw_file_pages);
    if (r < 0) {
        snprintf(error, error_size, "cannot read %s (%d)", FW_PATH, r);
        return r;
    }
    r = iwl_fw_parse(iwl.fw_file, fw_len, &iwl.fw);
    if (r < 0) {
        snprintf(error, error_size, "%s: %s", FW_PATH, iwl_fw_strerror(r));
        return -EINVAL;
    }
    klog("iwl: firmware %u.%08x.%u (%s)\n", iwl.fw.major, iwl.fw.minor, iwl.fw.local, iwl.fw.human);

    if (iwl_trans_power_up() < 0) {
        snprintf(error, error_size, "the card did not power up");
        return -EIO;
    }
    klog("iwl: SB_MODIFY_CFG_FLAG 0x%08x\n", iwl_read_prph(SB_MODIFY_CFG_FLAG + UMAC_PRPH_OFFSET));

    static uint8_t alive[ALIVE_LEN + 16];
    struct iwl_notif_wait w_alive, w_init;
    iwl_notif_arm(&w_alive, GRP_LEGACY, NTF_ALIVE, alive, sizeof(alive));
    iwl_notif_arm(&w_init, GRP_LEGACY, NTF_INIT_COMPLETE, NULL, 0);
    if (iwl_trans_start_fw() < 0) {
        iwl_notif_cancel(&w_alive);
        iwl_notif_cancel(&w_init);
        snprintf(error, error_size, "could not hand the firmware to the card");
        return -EIO;
    }
    r = iwl_notif_wait(&w_alive, ALIVE_TIMEOUT);
    if (r < 0 || w_alive.len < ALIVE_LEN) {
        iwl_notif_cancel(&w_init);
        iwl_dump_errors(r < 0 ? "no ALIVE" : "short ALIVE");
        snprintf(error, error_size, "the firmware did not start (no ALIVE)");
        return -EIO;
    }
    uint16_t status = le16(alive);
    iwl.sku_id[0] = le32(alive + 116);
    iwl.sku_id[1] = le32(alive + 120);
    iwl.sku_id[2] = le32(alive + 124);
    iwl.lmac_err = le32(alive + 4 + 16);
    iwl.lmac2_err = le32(alive + 4 + 48 + 16);
    uint32_t umac = le32(alive + 100 + 8) & ~FW_ADDR_CACHE_CONTROL_MASK;
    if (umac)
        iwl.umac_err = umac;
    if (status != ALIVE_STATUS_OK) {
        iwl_notif_cancel(&w_init);
        klog("iwl: ALIVE status 0x%04x\n", status);
        iwl_dump_errors("bad ALIVE");
        snprintf(error, error_size, "the firmware reported an error at start (0x%04x)", status);
        return -EIO;
    }
    klog("iwl: firmware alive, UMAC %u.%u, SKU %08x %08x %08x%s\n", le32(alive + 100),
         le32(alive + 104), iwl.sku_id[0], iwl.sku_id[1], iwl.sku_id[2],
         le16(alive + 2) & 1 ? ", RF kill switch on" : "");
    iwl_trans_fw_alive();
    iwl_free_file(iwl.fw_file, iwl.fw_file_pages);  /* the transport copied the sections */
    iwl.fw_file = NULL;

    if (iwl.sku_id[0] | iwl.sku_id[1] | iwl.sku_id[2])
        load_pnvm();                    /* Linux carries on after a PNVM failure, so do we */

    uint32_t flags = 1u << 1;           /* IWL_INIT_NVM */
    if (send(GRP_SYSTEM, CMD_INIT_EXTENDED_CFG, &flags, 4, "INIT_EXTENDED_CFG") < 0 ||
        send(GRP_REG_NVM, CMD_NVM_ACCESS_COMPLETE, (uint32_t[]){ 0 }, 4, "NVM_ACCESS_COMPLETE") < 0) {
        iwl_notif_cancel(&w_init);
        snprintf(error, error_size, "the firmware refused its init commands");
        return -EIO;
    }
    if (iwl_notif_wait(&w_init, INIT_TIMEOUT) < 0) {
        iwl_dump_errors("no INIT_COMPLETE");
        snprintf(error, error_size, "the firmware did not finish initialising");
        return -EIO;
    }
    if (read_nvm() < 0) {
        snprintf(error, error_size, "could not read the card's NVM");
        return -EIO;
    }
    if (read_mac() < 0) {
        snprintf(error, error_size, "the card has no valid MAC address");
        return -EIO;
    }

    uint32_t ldbg[2] = { 0x2B, 0x00010001 };        /* disable debug asserts */
    iwl_send_cmd(GRP_LONG, CMD_LDBG_CONFIG, ldbg, sizeof(ldbg), NULL, 0, NULL);
    uint32_t tx_ant = iwl.tx_ant;
    uint32_t bt[2] = { 1, 0x15 };                   /* BT_COEX_NW; SYNC2SCO, MPLUT, HIGH_BAND_RET */
    uint32_t soc[2] = { 0x1, 500 };                 /* discrete; 500 us xtal latency */
    if (send(GRP_LONG, CMD_TX_ANT_CONFIG, &tx_ant, 4, "TX_ANT_CONFIGURATION") < 0 ||
        send(GRP_LONG, CMD_BT_CONFIG, bt, sizeof(bt), "BT_CONFIG") < 0 ||
        send(GRP_SYSTEM, CMD_SOC_CONFIGURATION, soc, sizeof(soc), "SOC_CONFIGURATION") < 0) {
        snprintf(error, error_size, "the firmware refused its configuration");
        return -EIO;
    }
    uint32_t lari[7] = { 0x80 };                    /* China 2022 rules on, as without a BIOS */
    iwl_send_cmd(GRP_REG_NVM, CMD_LARI_CONFIG_CHANGE, lari, sizeof(lari), NULL, 0, NULL);
    uint8_t temp[20] = { 0 };
    iwl_send_cmd(GRP_PHY_OPS, CMD_TEMP_REPORTING_THS, temp, sizeof(temp), NULL, 0, NULL);
    uint32_t power = 0;                             /* no power save */
    if (send(GRP_LONG, CMD_POWER_TABLE, &power, 4, "POWER_TABLE") < 0) {
        snprintf(error, error_size, "the firmware refused its power settings");
        return -EIO;
    }
    if (iwl.lar) {
        if (mcc_update() < 0) {
            snprintf(error, error_size, "could not set the regulatory domain");
            return -EIO;
        }
    } else {
        mark_all_channels_usable();
    }
    uint32_t scan_cfg[3] = { 0, iwl.tx_ant, iwl.rx_ant };
    if (send(GRP_LONG, CMD_SCAN_CFG, scan_cfg, sizeof(scan_cfg), "SCAN_CFG") < 0) {
        snprintf(error, error_size, "the firmware refused its scan configuration");
        return -EIO;
    }
    return 0;
}

/* ---- notifications nobody is waiting for ---- */

void iwl_sta_rx(const struct iwl_rx_packet *pkt);  /* iwl_sta.c: RX frames, scan, TX status */

void iwl_mvm_rx(const struct iwl_rx_packet *pkt)
{
    uint32_t len = iwl_rx_payload_len(pkt);
    if ((pkt->group == GRP_LEGACY || pkt->group == GRP_LONG) && pkt->cmd == NTF_REPLY_ERROR) {
        if (len >= 8)
            klog("iwl: firmware rejected command 0x%02x (seq 0x%04x): error type 0x%08x, "
                 "service 0x%08x\n", pkt->data[4], le16(pkt->data + 6), le32(pkt->data),
                 len >= 12 ? le32(pkt->data + 8) : 0);
        return;
    }
    if (pkt->group == GRP_PHY_OPS && pkt->cmd == NTF_CT_KILL) {
        klog("iwl: the card is too hot (%u C): the firmware stopped transmitting\n",
             len >= 2 ? le16(pkt->data) : 0);
        return;
    }
    if ((pkt->group == GRP_LEGACY || pkt->group == GRP_LONG) && pkt->cmd == NTF_MCC_CHUB_UPDATE) {
        klog("iwl: the firmware suggests regulatory domain %c%c (ignored)\n",
             len >= 2 ? pkt->data[1] : '?', len >= 2 ? pkt->data[0] : '?');
        return;
    }
    iwl_sta_rx(pkt);
}
