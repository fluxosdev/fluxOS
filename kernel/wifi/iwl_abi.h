/* SPDX-License-Identifier: BSD-3-Clause
 * Copyright (C) 2012-2014, 2018-2024 Intel Corporation
 * Copyright (C) 2013-2015 Intel Mobile Communications GmbH
 * Copyright (C) 2016-2017 Intel Deutschland GmbH
 *
 * Wire structures extracted from Linux v6.12 iwlwifi fw/api headers.
 * Changes: fixed-width C types and numeric array dimensions for fluxOS.
 * See IWL-LICENSE for the BSD license chosen from GPL-2.0 OR BSD-3-Clause.
 */
#ifndef IWL_ABI_H
#define IWL_ABI_H
#include <stdint.h>

struct iwl_ac_qos {
	uint16_t cw_min;
	uint16_t cw_max;
	uint8_t aifsn;
	uint8_t fifos_mask;
	uint16_t edca_txop;
} __attribute__((packed));

struct iwl_he_backoff_conf {
	uint16_t cwmin;
	uint16_t cwmax;
	uint16_t aifsn;
	uint16_t mu_time;
} __attribute__((packed));

struct iwl_he_pkt_ext_v2 {
	uint8_t pkt_ext_qam_th[2][5][2];
} __attribute__((packed));

struct iwl_mac_client_data {
	uint8_t is_assoc;
	uint8_t esr_transition_timeout;
	uint16_t medium_sync_delay;
	uint16_t assoc_id;
	uint16_t reserved1;
	uint16_t data_policy;
	uint16_t reserved2;
	uint32_t ctwin;
} __attribute__((packed));

struct iwl_mac_p2p_dev_data {
	uint32_t is_disc_extended;
} __attribute__((packed));

struct iwl_mac_config_cmd {
	uint32_t id_and_color;
	uint32_t action;
	uint32_t mac_type;
	uint8_t local_mld_addr[6];
	uint16_t reserved_for_local_mld_addr;
	uint32_t filter_flags;
	uint16_t he_support;
	uint16_t he_ap_support;
	uint32_t eht_support;
	uint32_t nic_not_ack_enabled;
	union {
		struct iwl_mac_client_data client;
		struct iwl_mac_p2p_dev_data p2p_dev;
	};
} __attribute__((packed));

struct iwl_link_config_cmd {
	uint32_t action;
	uint32_t link_id;
	uint32_t mac_id;
	uint32_t phy_id;
	uint8_t local_link_addr[6];
	uint16_t reserved_for_local_link_addr;
	uint32_t modify_mask;
	uint32_t active;
	union {
		uint32_t listen_lmac;
		struct {
			uint8_t block_tx;
			uint8_t reserved1[3];
		};
	};
	uint32_t cck_rates;
	uint32_t ofdm_rates;
	uint32_t cck_short_preamble;
	uint32_t short_slot;
	uint32_t protection_flags;
	uint32_t qos_flags;
	struct iwl_ac_qos ac[4 + 1];
	uint8_t htc_trig_based_pkt_ext;
	uint8_t rand_alloc_ecwmin;
	uint8_t rand_alloc_ecwmax;
	uint8_t ndp_fdbk_buff_th_exp;
	struct iwl_he_backoff_conf trig_based_txf[4];
	uint32_t bi;
	uint32_t dtim_interval;
	uint16_t puncture_mask; 
	uint16_t frame_time_rts_th;
	uint32_t flags;
	uint32_t flags_mask;
	uint8_t ref_bssid_addr[6];
	uint16_t reserved_for_ref_bssid_addr;
	uint8_t bssid_index;
	uint8_t bss_color;
	uint8_t spec_link_id;
	uint8_t ul_mu_data_disable;
	uint8_t ibss_bssid_addr[6];
	uint16_t reserved_for_ibss_bssid_addr;
	uint32_t reserved3[8];
} __attribute__((packed));

struct iwl_sta_cfg_cmd {
	uint32_t sta_id;
	uint32_t link_id;
	uint8_t peer_mld_address[6];
	uint16_t reserved_for_peer_mld_address;
	uint8_t peer_link_address[6];
	uint16_t reserved_for_peer_link_address;
	uint32_t station_type;
	uint32_t assoc_id;
	uint32_t beamform_flags;
	uint32_t mfp;
	uint32_t mimo;
	uint32_t mimo_protection;
	uint32_t ack_enabled;
	uint32_t trig_rnd_alloc;
	uint32_t tx_ampdu_spacing;
	uint32_t tx_ampdu_max_size;
	uint32_t sp_length;
	uint32_t uapsd_acs;
	struct iwl_he_pkt_ext_v2 pkt_ext;
	uint32_t htc_flags;
} __attribute__((packed));

struct iwl_fw_channel_info {
	uint32_t channel;
	uint8_t band;
	uint8_t width;
	uint8_t ctrl_pos;
	uint8_t reserved;
} __attribute__((packed));

struct iwl_phy_context_cmd {
	uint32_t id_and_color;
	uint32_t action;
	struct iwl_fw_channel_info ci;
	uint32_t lmac_id;
	union {
		uint32_t rxchain_info; 
		struct {             
			uint8_t sbb_bandwidth;
			uint8_t sbb_ctrl_channel_loc;
			uint16_t puncture_mask; 
		};
	};
	uint32_t dsp_cfg_flags;
	uint32_t reserved;
} __attribute__((packed));

struct iwl_mvm_session_prot_cmd {
	uint32_t id_and_color;
	uint32_t action;
	uint32_t conf_id;
	uint32_t duration_tu;
	uint32_t repetition_count;
	uint32_t interval;
} __attribute__((packed));

struct iwl_sec_key_cmd {
	uint32_t action;
	union {
		struct {
			uint32_t sta_mask;
			uint32_t key_id;
			uint32_t key_flags;
			uint8_t key[32];
			uint8_t tkip_mic_rx_key[8];
			uint8_t tkip_mic_tx_key[8];
			uint64_t rx_seq;
			uint64_t tx_seq;
		} __attribute__((packed)) add; 
		struct {
			uint32_t old_sta_mask;
			uint32_t new_sta_mask;
			uint32_t key_id;
			uint32_t key_flags;
		} __attribute__((packed)) modify; 
		struct {
			uint32_t sta_mask;
			uint32_t key_id;
			uint32_t key_flags;
		} __attribute__((packed)) remove; 
	} __attribute__((packed)) u; 
} __attribute__((packed));

struct iwl_ssid_ie {
	uint8_t id;
	uint8_t len;
	uint8_t ssid[32];
} __attribute__((packed));

struct iwl_scan_probe_segment {
	uint16_t offset;
	uint16_t len;
} __attribute__((packed));

struct iwl_scan_probe_req {
	struct iwl_scan_probe_segment mac_header;
	struct iwl_scan_probe_segment band_data[3];
	struct iwl_scan_probe_segment common_data;
	uint8_t buf[512];
} __attribute__((packed));

struct  iwl_scan_channel_cfg_umac {
	uint32_t flags;
	uint8_t channel_num;
	union {
		struct {
			uint8_t iter_count;
			uint16_t iter_interval;
		} __attribute__((packed)) v1;  
		struct {
			uint8_t band;
			uint8_t iter_count;
			uint8_t iter_interval;
		} __attribute__((packed)) v2; 
		struct {
			uint8_t psd_20;
			uint8_t iter_count;
			uint8_t iter_interval;
		} __attribute__((packed)) v5;  
	} __attribute__((packed));
} __attribute__((packed));

struct iwl_scan_umac_schedule {
	uint16_t interval;
	uint8_t iter_count;
	uint8_t reserved;
} __attribute__((packed));

struct iwl_scan_general_params_v11 {
	uint16_t flags;
	uint8_t reserved;
	uint8_t scan_start_mac_or_link_id;
	uint8_t active_dwell[2];
	uint8_t adwell_default_2g;
	uint8_t adwell_default_5g;
	uint8_t adwell_default_social_chn;
	uint8_t flags2;
	uint16_t adwell_max_budget;
	uint32_t max_out_of_time[2];
	uint32_t suspend_time[2];
	uint32_t scan_priority;
	uint8_t passive_dwell[2];
	uint8_t num_of_fragments[2];
} __attribute__((packed));

struct iwl_scan_channel_params_v7 {
	uint8_t flags;
	uint8_t count;
	uint8_t n_aps_override[2];
	struct iwl_scan_channel_cfg_umac channel_config[67];
} __attribute__((packed));

struct iwl_scan_periodic_parms_v1 {
	struct iwl_scan_umac_schedule schedule[2];
	uint16_t delay;
	uint16_t reserved;
} __attribute__((packed));

struct iwl_scan_probe_params_v4 {
	struct iwl_scan_probe_req preq;
	uint8_t short_ssid_num;
	uint8_t bssid_num;
	uint16_t reserved;
	struct iwl_ssid_ie direct_scan[20];
	uint32_t short_ssid[8];
	uint8_t bssid_array[16][6];
} __attribute__((packed));

struct iwl_scan_req_params_v17 {
	struct iwl_scan_general_params_v11 general_params;
	struct iwl_scan_channel_params_v7 channel_params;
	struct iwl_scan_periodic_parms_v1 periodic_params;
	struct iwl_scan_probe_params_v4 probe_params;
} __attribute__((packed));

struct iwl_scan_req_umac_v17 {
	uint32_t uid;
	uint32_t ooc_priority;
	struct iwl_scan_req_params_v17 scan_params;
} __attribute__((packed));

#endif
