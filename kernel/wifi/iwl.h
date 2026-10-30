/*
 * Intel AX210 (iwlwifi "TY" MAC, "GF" radio) driver, internal interface.
 *
 *   iwl_trans.c  the PCIe transport: power-up, loading the firmware
 *                through the context info, the RX queue and the TX
 *                queues, host commands and their responses, interrupts
 *   iwl_mvm.c    the firmware: bring-up after ALIVE, the PNVM, the NVM,
 *                regulatory setup, crash dumps, and the station
 *                operations struct wifi_hw needs (scan, join, TX, keys)
 *   iwl_fw.c     the firmware file
 *
 * Register layouts, command formats and the bring-up sequence follow
 * Linux's iwlwifi (GPL-2.0 OR BSD-3-Clause) for this device and firmware
 * API 89 (iwlwifi-ty-a0-gf-a0-89.ucode).
 */
#ifndef IWL_H
#define IWL_H

#include <stdint.h>
#include "iwl_fw.h"

/* ---- command groups and IDs ---- */
#define GRP_LEGACY      0x00
#define GRP_LONG        0x01
#define GRP_SYSTEM      0x02
#define GRP_MAC_CONF    0x03
#define GRP_PHY_OPS     0x04
#define GRP_DATA_PATH   0x05
#define GRP_SCAN        0x06
#define GRP_REG_NVM     0x0C

/* notifications */
#define NTF_ALIVE           0x01        /* group 0 or 1 */
#define NTF_REPLY_ERROR     0x02
#define NTF_INIT_COMPLETE   0x04
#define NTF_PNVM_COMPLETE   0xFE        /* GRP_REG_NVM */
#define NTF_MCC_CHUB_UPDATE 0xC9
#define NTF_CT_KILL         0xFE        /* GRP_PHY_OPS */

/* bring-up commands */
#define CMD_INIT_EXTENDED_CFG   0x03    /* GRP_SYSTEM */
#define CMD_SOC_CONFIGURATION   0x01    /* GRP_SYSTEM */
#define CMD_NVM_ACCESS_COMPLETE 0x00    /* GRP_REG_NVM */
#define CMD_LARI_CONFIG_CHANGE  0x01    /* GRP_REG_NVM */
#define CMD_NVM_GET_INFO        0x02    /* GRP_REG_NVM */
#define CMD_LDBG_CONFIG         0xF6    /* GRP_LONG */
#define CMD_TX_ANT_CONFIG       0x98
#define CMD_BT_CONFIG           0x9B
#define CMD_POWER_TABLE         0x77
#define CMD_MCC_UPDATE          0xC8
#define CMD_SCAN_CFG            0x0C
#define CMD_TEMP_REPORTING_THS  0x04    /* GRP_PHY_OPS */

/* ---- registers the firmware layer also uses ---- */
#define CSR_HW_REV          0x028
#define CSR_HW_RF_ID        0x09C
#define CSR_MAC_ADDR0_OTP   0x380
#define CSR_MAC_ADDR1_OTP   0x384
#define CSR_MAC_ADDR0_STRAP 0x388
#define CSR_MAC_ADDR1_STRAP 0x38C

#define UMAC_PRPH_OFFSET          0x300000
#define UREG_DOORBELL_TO_ISR6     0xA05C04
#define UREG_DOORBELL_TO_ISR6_PNVM 0x00100000
#define UMAG_SB_CPU_1_STATUS      0xA038C0
#define UMAG_SB_CPU_2_STATUS      0xA038C4
#define SB_MODIFY_CFG_FLAG        0xA03088
#define FSEQ_ERROR_CODE           0xA340C8
#define FW_ADDR_CACHE_CONTROL_MASK 0xC0000000u

/* ---- a received packet: a notification or a command's response ---- */
struct iwl_rx_packet {
    uint32_t len_n_flags;               /* bits 0-13: the header and payload's length */
    uint8_t cmd;
    uint8_t group;
    uint16_t sequence;                  /* bit 15: not a response to us */
    uint8_t data[];
} __attribute__((packed));

#define RX_LEN_MASK     0x3FFF
#define RX_CMD_HDR_LEN  4
#define SEQ_RX_FRAME    0x8000

static inline uint32_t iwl_rx_payload_len(const struct iwl_rx_packet *p)
{
    uint32_t len = p->len_n_flags & RX_LEN_MASK;
    return len > RX_CMD_HDR_LEN ? len - RX_CMD_HDR_LEN : 0;
}

/* A wait for one notification, armed before whatever triggers it. */
struct iwl_notif_wait {
    uint8_t group, cmd;
    int any_legacy;                     /* group 0 or 1 */
    volatile int done;
    uint8_t *buf;                       /* where its payload goes (may be NULL) */
    uint32_t size, len;
    struct iwl_notif_wait *next;
};

/* ---- the device ---- */
struct iwl_dev {
    volatile uint8_t *regs;
    int irq;
    uint32_t hw_rev, hw_rf_id;
    struct iwl_fw fw;
    uint8_t *fw_file;                   /* the .ucode in memory while sections are copied */
    uint32_t fw_file_pages;
    int dead;                           /* the firmware asserted, or the card went away */
    /* from ALIVE */
    uint32_t sku_id[3];
    uint32_t lmac_err, lmac2_err, umac_err;
    /* from the NVM */
    uint8_t mac[6];
    uint8_t tx_ant, rx_ant;
    int band_2ghz, band_5ghz, lar;
};

extern struct iwl_dev iwl;

/* ---- transport (iwl_trans.c) ---- */
int iwl_trans_probe(void);              /* find the card, map it: 0, or -1 */
int iwl_trans_power_up(void);           /* to "ready to load firmware" */
int iwl_trans_start_fw(void);           /* hand the firmware over (context info) and run it */
void iwl_trans_fw_alive(void);          /* after ALIVE: free what is no longer needed */
void iwl_trans_stop(void);
void iwl_trans_service(void);           /* reap RX and TX completions; dispatch packets */
/* A host command; waits for its response (resp may be NULL). 0, or -errno. */
int iwl_send_cmd(uint8_t group, uint8_t cmd, const void *data, uint32_t len, void *resp,
                 uint32_t resp_size, uint32_t *resp_len);
int iwl_send_cmd_async(uint8_t group, uint8_t cmd, const void *data, uint32_t len);
/* TX queues for a firmware station's TID (SCD_QUEUE_CONFIG): 0-7 for QoS
 * data, 15 for management and non-QoS frames. A handle (0 or 1), or -errno. Freed by iwl_trans_free_txq() before the station goes. */
int iwl_trans_alloc_txq(int sta_id, int tid);
void iwl_trans_free_txq(int txq);
/* A frame: the 28-byte TX command (its len set), then the 802.11 frame,
 * whose header is hdr_len bytes. Any context, never sleeps. 0, or -errno. */
int iwl_trans_tx(int txq, const void *tx_cmd, const uint8_t *frame, uint32_t hdr_len, uint32_t len);
/* The firmware is done with a queue's frames up to (not including) ssn. */
void iwl_trans_reclaim(int fw_queue, uint16_t ssn);
int iwl_trans_rfkill(void);             /* the hardware switch has the radio off */
void iwl_trans_dump_queues(void);       /* command and TX ring pointers, for a crash dump */
/* Periphery registers and device memory (grabbing NIC access as needed). */
uint32_t iwl_read_prph(uint32_t addr);
void iwl_write_prph(uint32_t addr, uint32_t val);
uint32_t iwl_read32(uint32_t reg);
void iwl_write32(uint32_t reg, uint32_t val);
int iwl_read_mem(uint32_t addr, uint32_t *buf, uint32_t dwords);
/* PNVM: hand chunks to the firmware through the prph scratch area. */
int iwl_trans_set_pnvm(const uint8_t *const chunks[], const uint32_t lens[], int n);

void iwl_notif_arm(struct iwl_notif_wait *w, uint8_t group, uint8_t cmd, void *buf, uint32_t size);
int iwl_notif_wait(struct iwl_notif_wait *w, uint32_t timeout_ms);    /* 0, or -ETIMEDOUT/-EIO */
void iwl_notif_cancel(struct iwl_notif_wait *w);

/* ---- firmware layer (iwl_mvm.c), called by the transport ---- */
void iwl_mvm_rx(const struct iwl_rx_packet *pkt);  /* a notification nobody waits for */
void iwl_mvm_fw_error(int hw_error);                /* the SW/HW error interrupt */
void iwl_mvm_tx_done(int queue, int index);         /* reclaim hook */

#endif
