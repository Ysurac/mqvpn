// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#ifndef MQVPN_VPN_CLIENT_H
#define MQVPN_VPN_CLIENT_H

#include <stdint.h>

#include "libmqvpn.h" /* for MQVPN_MAX_PATHS */
#include "reorder.h"  /* mqvpn_reorder_config_t (INI [Reorder] bridge) */

#define MQVPN_MAX_PATH_IFACES 8
_Static_assert(MQVPN_MAX_PATH_IFACES == MQVPN_MAX_PATHS,
               "CLI path cap must equal library cap (libmqvpn.h)");

typedef struct mqvpn_client_cfg_s {
    const char *server_addr;     /* server address (e.g. "1.2.3.4") */
    int server_port;             /* server port (e.g. 443) */
    const char *tls_server_name; /* SNI / cert verify name (NULL = use server_addr) */
    const char *tun_name;        /* TUN device name */
    const char *tls_ciphers;     /* TLS cipher suites list */
    int insecure;                /* skip TLS cert verification */
    int log_level;               /* mqvpn_log_level_t */
    const char *path_ifaces[MQVPN_MAX_PATH_IFACES]; /* network interfaces for multipath */
    int n_paths;                /* number of path interfaces (0 = single-path) */
    const char *backup_ifaces[MQVPN_MAX_PATH_IFACES]; /* failover-only interfaces */
    int n_backup_paths;         /* number of backup interfaces */
    int scheduler;              /* 0=minrtt, 1=wlb, 2=backup, 3=backup_fec, 4=rap, 5=wlb_udp_pin, 6=wrtt */
    int reinjection_control;    /* 1=enable reinjection control */
    int reinjection_mode;       /* 0=default, 1=deadline, 2=dgram */
    int fec_enable;             /* 1=enable FEC */
    int fec_scheme;             /* 0=reed_solomon, 1=xor, 2=packet_mask, 3=galois_calculation */
    const char *auth_key;       /* PSK for server authentication (NULL = no auth) */
    const char *auth_username;  /* client username sent to server via x-user header (NULL = none) */
    const char *dns_servers[4]; /* DNS servers to configure (NULL = no DNS override) */
    int n_dns;                  /* number of DNS servers */
    int reconnect;              /* 1=auto-reconnect on disconnect (default 1) */
    int reconnect_interval;     /* base reconnect interval in seconds (default 5) */
    int kill_switch;            /* 1=block traffic outside tunnel (default 0) */
    int route_via_server;       /* 1=default via server tunnel IP instead of /1 trick (default 0) */
    int no_routes;              /* 1=skip automatic route setup entirely (default 0) */
    int control_port;           /* TCP port for JSON control API (0 = disabled) */
    const char *control_addr;   /* bind address for control API (NULL = "127.0.0.1") */
    uint64_t init_max_path_id;  /* draft-21 §4.6 TP cap, 0=use xquic default 8 */
    int tun_mtu;                /* TUN MTU override, 0=auto (derived from MASQUE MSS) */
    int cc;                     /* mqvpn_cc_t: congestion control algorithm */
    mqvpn_reorder_config_t
        reorder; /* INI [Reorder]/[ReorderRule] (mode OFF by default) */
} mqvpn_client_cfg_t;

#endif /* MQVPN_VPN_CLIENT_H */
