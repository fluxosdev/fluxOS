/* IEEE 802.11 frame layouts and numbers: what a station needs. */
#ifndef WIFI_IEEE80211_H
#define WIFI_IEEE80211_H

#include <stdint.h>

/* Frame control, first byte: type and subtype */
#define FC_TYPE_MASK    0x0C
#define FC_TYPE_MGMT    0x00
#define FC_TYPE_CTRL    0x04
#define FC_TYPE_DATA    0x08
#define FC_STYPE_MASK   0xF0

#define FC_ASSOC_REQ    0x00
#define FC_ASSOC_RESP   0x10
#define FC_REASSOC_RESP 0x30
#define FC_PROBE_REQ    0x40
#define FC_PROBE_RESP   0x50
#define FC_BEACON       0x80
#define FC_DISASSOC     0xA0
#define FC_AUTH         0xB0
#define FC_DEAUTH       0xC0
#define FC_ACTION       0xD0

#define FC_DATA         0x08
#define FC_QOS_DATA     0x88
#define FC_QOS_BIT      0x80            /* in a data subtype */
#define FC_NULLFUNC_BIT 0x40            /* a data subtype without a body */

/* Frame control, second byte: flags */
#define FC_TODS         0x01
#define FC_FROMDS       0x02
#define FC_MOREFRAG     0x04
#define FC_RETRY        0x08
#define FC_PROTECTED    0x40
#define FC_ORDER        0x80

struct ieee80211_hdr {
    uint8_t fc[2];
    uint16_t duration;
    uint8_t addr1[6], addr2[6], addr3[6];
    uint16_t seq;
} __attribute__((packed));

#define HDR_LEN      24
#define QOS_CTL_LEN  2
#define HT_CTL_LEN   4
#define CCMP_HDR_LEN 8
#define CCMP_MIC_LEN 8

/* Capability information */
#define CAP_ESS            0x0001
#define CAP_PRIVACY        0x0010
#define CAP_SHORT_PREAMBLE 0x0020
#define CAP_SHORT_SLOT     0x0400

/* Information elements */
#define IE_SSID       0
#define IE_RATES      1
#define IE_DS_PARAM   3
#define IE_TIM        5
#define IE_HT_OPER    61
#define IE_RSN        48
#define IE_EXT_RATES  50
#define IE_VENDOR     221

#define RATE_BASIC 0x80                 /* in a rates element: the network requires it */

/* Status and reason codes */
#define STATUS_SUCCESS 0
#define REASON_UNSPECIFIED   1
#define REASON_DEAUTH_LEAVING 3
#define REASON_4WAY_TIMEOUT  15

/* LLC/SNAP before the EtherType in a data frame */
#define LLC_SNAP_LEN 8
#define ETH_P_EAPOL  0x888E

#endif
