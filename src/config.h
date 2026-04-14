/*
 * config.h — INI-style configuration file parser for mqvpn
 *
 * Sections: [Interface], [Server], [TLS], [Auth], [Multipath]
 * Mode is inferred from keys:
 *   [Interface] Listen → server mode
 *   [Server] Address   → client mode
 */
#ifndef MQVPN_CONFIG_H
#define MQVPN_CONFIG_H

#define MQVPN_CONFIG_MAX_PATHS   4
#define MQVPN_CONFIG_MAX_DNS     4
#define MQVPN_CONFIG_MAX_USERS   64

typedef struct mqvpn_file_config_s {
    /* [Interface] — common */
    char tun_name[32];
    char log_level[16];

    /* [Interface] — server */
    char listen[280]; /* "bind:port" */
    char subnet[32];
    char subnet6[64]; /* IPv6 tunnel subnet CIDR (e.g. "2001:db8:1::/112") */

    /* [Interface] — client */
    char dns_servers[MQVPN_CONFIG_MAX_DNS][64];
    int n_dns;

    /* [Server] — client */
    char server_addr[280]; /* "host:port" */
    int insecure;

    /* [Auth] — client */
    char auth_key[256];

    /* [TLS] — server */
    char cert_file[256];
    char key_file[256];
    char tls_ciphers[256]; /* TLS cipher suites list */

    /* [Auth] — server */
    char server_auth_key[256];
    char user_names[MQVPN_CONFIG_MAX_USERS][64];
    char user_keys[MQVPN_CONFIG_MAX_USERS][256];
    int  n_users;
    int  max_clients;

    /* [Multipath] */
    char paths[MQVPN_CONFIG_MAX_PATHS][32];
    int n_paths;
    char backup_paths[MQVPN_CONFIG_MAX_PATHS][32]; /* failover-only interfaces */
    int n_backup_paths;
    char scheduler[16];
    int reinjection_control; /* 1=enable reinjection, 0=off */
    char reinjection_mode[16]; /* default|deadline|dgram */
    int fec_enable; /* 1=enable FEC, 0=off */
    char fec_scheme[32]; /* galois_calculation|packet_mask|reed_solomon|xor */
    char cc[16];

    /* [Interface] — client reconnection */
    int reconnect;          /* 1=auto-reconnect (default), 0=exit on disconnect */
    int reconnect_interval; /* base interval in seconds (default 5) */
    int kill_switch;        /* 1=block traffic outside tunnel, 0=off (default) */
    int route_via_server;   /* 1=default via server tunnel IP, 0=0/1+128/1 trick (default) */
    int no_routes;          /* 1=skip automatic route setup entirely, 0=auto (default) */

    /* [Control] — server */
    int control_port;           /* TCP port for JSON control API (0 = disabled) */
    char control_addr[64];      /* bind address for control API (default "127.0.0.1") */

    /* Inferred mode: 1=server, 0=client */
    int is_server;
} mqvpn_file_config_t;

/* Fill cfg with default values */
void mqvpn_config_defaults(mqvpn_file_config_t *cfg);

/* Parse INI file at path into cfg. Returns 0 on success, -1 on error. */
int  mqvpn_config_load(mqvpn_file_config_t *cfg, const char *path);

/* Parse JSON text into CLI cfg. Returns 0 on success, -1 on error. */
int  mqvpn_config_load_json_filecfg(mqvpn_file_config_t *cfg, const char *json_text);

#endif /* MQVPN_CONFIG_H */
