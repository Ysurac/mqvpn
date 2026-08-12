// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#ifndef MQVPN_VPN_SERVER_H
#define MQVPN_VPN_SERVER_H

#include <stdint.h>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netinet/in.h>
#endif

#include "reorder.h"           /* mqvpn_reorder_config_t (INI [Reorder] bridge) */
#include "hybrid/classifier.h" /* mqvpn_hybrid_config_t (INI [Hybrid] bridge) */

/* Persisted per-(user,iface) downlink weight/dscp_mask override — bridge
 * copy of config.h's mqvpn_path_policy_t. Kept as its own type (rather than
 * including config.h here) so this header never pulls in the INI/JSON
 * file-config parsing layer; main.c copies field-by-field from
 * mqvpn_file_config_t.path_policy[] into this array. iface[16] mirrors
 * MQVPN_PATH_LABEL_IFACE_MAX(15)+1 from mqvpn_path_label.h. */
typedef struct {
    char     user[64];
    char     iface[16];
    int      has_weight;
    uint32_t weight;
    int      has_dscp_mask;
    uint64_t dscp_mask;
} mqvpn_path_policy_entry_t;

typedef struct mqvpn_server_cfg_s {
    const char *listen_addr;        /* bind address (e.g. "0.0.0.0") */
    int listen_port;                /* bind port (e.g. 443) */
    const char *subnet;             /* client IP pool CIDR (e.g. "10.0.0.0/24") */
    const char *subnet6;            /* IPv6 client pool CIDR (NULL = disabled) */
    const char *tun_name;           /* TUN device name */
    const char *cert_file;          /* TLS certificate path */
    const char *key_file;           /* TLS private key path */
    const char *tls_ciphers;        /* TLS cipher suites list */
    int log_level;                  /* mqvpn_log_level_t */
    int scheduler;                  /* 0=minrtt, 1=wlb, 2=backup, 3=backup_fec, 4=rap, 5=wlb_udp_pin, 6=wrtt, 7=wrr, 8=redundant, 9=dscp */
    int fec_enable;                 /* 1=enable FEC */
    int fec_scheme;                 /* 0=reed_solomon, 1=xor, 2=packet_mask, 3=galois_calculation */
    const char *auth_key;           /* PSK for client authentication (NULL = no auth) */
    const char *user_names[64];
    const char *user_keys[64];
    const char *user_fixed_ips[64]; /* NULL or "" = dynamic, "x.x.x.x" = pinned */
    int n_users;
    int max_clients;           /* max concurrent clients (default 64) */
    const char *control_addr;  /* bind address for JSON control API (default 127.0.0.1) */
    int control_port;          /* TCP port for JSON control API (0 = disabled) */
    uint64_t init_max_path_id; /* draft-21 §4.6 TP cap, 0=use xquic default 8 */
    int tun_mtu;               /* 0=auto (1382 at startup), >0=override (floor 1280) */
    int cc;                    /* mqvpn_cc_t: congestion control algorithm */
    int reinjection;           /* mqvpn_reinjection_t; 0=off (default) */
    int reinj_srtt_factor_pct; /* deadline mode; percent, e.g. 110 = 1.10x srtt */
    int reinj_hard_deadline_ms;        /* deadline mode */
    int reinj_deadline_lower_bound_ms; /* deadline mode */
    mqvpn_reorder_config_t
        reorder;                  /* INI [Reorder]/[ReorderRule] (mode OFF by default) */
    mqvpn_hybrid_config_t hybrid; /* INI [Hybrid] (disabled by default) */
    int udp_gso;                  /* [Advanced] UdpGso; default 1 */
    int udp_gro;                  /* [Advanced] UdpGro; default 1 */
    int sync_path_labels;         /* [Multipath] SyncPathLabels; default 1 — auto-adopt
                                    * client-announced weight/dscp_mask (mqvpn_path_label.h);
                                    * 0 = client/server weight/dscp_mask configured
                                    * independently */
    int push_path_labels;         /* [Multipath] PushPathLabels; default 0 — push an
                                    * operator-pinned per-(user,iface) weight/dscp_mask down
                                    * to that user's client (MQVPN_CAPSULE_PATH_LABEL_PUSH);
                                    * 0 = never push (pre-existing behavior) */
    /* [Multipath] persisted per-(user,iface) weight/dscp_mask overrides;
     * JSON "path_policy" array only (config.h), no CLI/INI equivalent.
     * Matches MQVPN_CONFIG_MAX_PATH_POLICY (config.h), hardcoded here the
     * same way user_names[64]/user_keys[64] above mirror MQVPN_MAX_USERS
     * without including that header. */
    mqvpn_path_policy_entry_t path_policy[128];
    int n_path_policy;
} mqvpn_server_cfg_t;

#endif /* MQVPN_VPN_SERVER_H */
