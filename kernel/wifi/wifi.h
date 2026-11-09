/*
 * The wireless station layer (wifi.c) and what a card driver gives it.
 *
 * wifi.c knows 802.11 and WPA2 but no hardware: it keeps the networks
 * heard in scans, runs the connection (authentication, association, the
 * key handshake, reconnecting), turns data frames into Ethernet frames
 * for the network stack and back, and answers SYS_wifi. A driver (iwl*.c
 * for Intel's AX210) supplies struct wifi_hw: starting the card, scans,
 * the firmware's view of the network joined, sending frames and keys.
 *
 * Everything but tx_data runs on one kernel thread, kwifid, which also
 * calls the driver's service() whenever the card interrupts (wifi_wake)
 * and ten times a second. Received management and EAPOL frames are
 * queued and handled in the thread's loop, never inside a driver call,
 * so a driver may hand frames over while it waits for a command.
 */
#ifndef WIFI_H
#define WIFI_H

#include <stdint.h>
#include "wpa.h"

#define WIFI_SSID_MAX 32
#define WIFI_RATES_MAX 16

#define BAND_2GHZ 0
#define BAND_5GHZ 1

/* A network (one access point) heard in a scan. */
struct wifi_bss {
    uint8_t bssid[6];
    uint8_t ssid[WIFI_SSID_MAX];
    uint8_t ssid_len;
    uint8_t channel;
    uint8_t band;                       /* BAND_* */
    int8_t rssi;                        /* dBm */
    uint16_t capab;                     /* CAP_* */
    uint16_t beacon_int;                /* TUs */
    uint8_t dtim_period;
    uint8_t security;                   /* SEC_* (wpa.h) */
    uint8_t rates[WIFI_RATES_MAX];      /* 500 kbit/s units, RATE_BASIC where required */
    uint8_t nrates;
    uint8_t rsn[WPA_IE_MAX];            /* its RSN element, whole */
    uint8_t rsn_len;
    uint32_t seen;                      /* tick it was last heard */
};

struct wifi_hw {
    const char *name;                   /* "iwlwifi AX210" */
    /* Bring the card up (firmware, calibration): in kwifid, may take seconds.
     * Fills mac and firmware. 0, or -errno with error set. */
    int (*start)(void);
    void (*service)(void);              /* take what the card has finished */
    int (*scan)(void);                  /* start one; wifi_scan_done() when over */
    /* Get ready to exchange frames with bss: channel, the firmware's MAC
     * context and station for the AP, and staying on the channel for a while. */
    int (*join)(const struct wifi_bss *bss);
    int (*associated)(const struct wifi_bss *bss, uint16_t aid);
    int (*authorized)(void);            /* keys in place (or an open network): data may flow */
    void (*leave)(void);                /* back to idle, after a failure or a disconnect */
    int (*tx_mgmt)(const uint8_t *frame, uint32_t len);
    /* A data frame, from any context with interrupts off (the network
     * stack's lock): must not sleep. encrypt: with the pairwise key. */
    int (*tx_data)(const uint8_t *frame, uint32_t len, int encrypt);
    /* pairwise: the TK as key 0; else the GTK as key idx. rsc: receive PN. */
    int (*set_key)(int pairwise, int idx, const uint8_t *key, uint32_t len, const uint8_t rsc[8]);
    uint8_t mac[6];
    char firmware[48];
    char error[64];
};

/* From the driver */
void wifi_register(struct wifi_hw *hw);    /* card found (at boot, before processes) */
void wifi_wake(void);                       /* from the interrupt handler */
/* A received 802.11 frame (from service()), header first. decrypted: the
 * hardware removed the protection: no CCMP header or MIC left, though the
 * Protected bit may still be set. */
void wifi_rx(const uint8_t *frame, uint32_t len, int rssi, int channel, int band, int decrypted);
void wifi_scan_done(void);
void wifi_hw_failed(const char *why);       /* the firmware died */

/* From the kernel */
void wifi_start(void);                      /* once processes run: the thread */
int wifi_syscall(uint32_t op, uint32_t arg);   /* SYS_wifi; arg checked here */
int iwl_probe(void);                        /* look for an Intel card: 0 if found */

/* Channel numbers and frequencies */
uint32_t wifi_channel_freq(int channel, int band);

#endif
