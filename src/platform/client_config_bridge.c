// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * CLI → library config bridge, shared by every platform run loop.
 *
 * Single source of truth for forwarding the platform-independent
 * mqvpn_client_cfg_t fields into a mqvpn_config_t. Keep ALL common setters
 * here: when this block lived as three per-platform copies, a knob added to
 * one copy could be silently dropped by the others (InitMaxPathId never
 * reached the wire on Windows until the copies were re-synced by hand).
 * tests/test_config_bridge.c pins the forwarding of every field.
 *
 * Platform-only knobs stay at the call sites (Linux: UdpGso — a Linux
 * sendmsg-offload toggle; the other platforms deliberately leave the
 * library default untouched — and the multipath override for backup paths
 * and the control API, both Linux-only).
 */

#include "vpn_client.h"
#include "mqvpn_internal.h" /* mqvpn_config_apply_reorder / _apply_hybrid */

void
mqvpn_platform_apply_client_config(mqvpn_config_t *lib_cfg, const mqvpn_client_cfg_t *cfg)
{
    mqvpn_config_set_server(lib_cfg, cfg->server_addr, cfg->server_port);
    if (cfg->tls_server_name)
        mqvpn_config_set_tls_server_name(lib_cfg, cfg->tls_server_name);
    if (cfg->auth_key) mqvpn_config_set_auth_key(lib_cfg, cfg->auth_key);
    if (cfg->auth_username && cfg->auth_username[0])
        mqvpn_config_set_auth_username(lib_cfg, cfg->auth_username);
    if (cfg->tls_ciphers && cfg->tls_ciphers[0])
        mqvpn_config_set_tls_ciphers(lib_cfg, cfg->tls_ciphers);
    mqvpn_config_set_insecure(lib_cfg, cfg->insecure);
    mqvpn_config_set_multipath(lib_cfg, cfg->n_paths > 1 ? 1 : 0);
    mqvpn_config_set_reconnect(lib_cfg, cfg->reconnect,
                               cfg->reconnect_interval > 0 ? cfg->reconnect_interval : 5);
    mqvpn_config_set_killswitch_hint(lib_cfg, cfg->kill_switch);

    mqvpn_config_set_log_level(lib_cfg, (mqvpn_log_level_t)cfg->log_level);

    /* cfg->scheduler carries mqvpn_scheduler_t values (main.c resolves names
     * through mqvpn_sched_names.h); anything unknown falls back to MinRTT. */
    mqvpn_scheduler_t sched = MQVPN_SCHED_MINRTT;
    switch (cfg->scheduler) {
    case MQVPN_SCHED_WLB: sched = MQVPN_SCHED_WLB; break;
    case MQVPN_SCHED_BACKUP: sched = MQVPN_SCHED_BACKUP; break;
    case MQVPN_SCHED_BACKUP_FEC: sched = MQVPN_SCHED_BACKUP_FEC; break;
    case MQVPN_SCHED_RAP: sched = MQVPN_SCHED_RAP; break;
    case MQVPN_SCHED_WLB_UDP_PIN: sched = MQVPN_SCHED_WLB_UDP_PIN; break;
    case MQVPN_SCHED_WRTT: sched = MQVPN_SCHED_WRTT; break;
    case MQVPN_SCHED_WRR: sched = MQVPN_SCHED_WRR; break;
    case MQVPN_SCHED_REDUNDANT: sched = MQVPN_SCHED_REDUNDANT; break;
    case MQVPN_SCHED_DSCP: sched = MQVPN_SCHED_DSCP; break;
    default: break;
    }
    mqvpn_config_set_scheduler(lib_cfg, sched);
    mqvpn_config_set_cc(lib_cfg, (mqvpn_cc_t)cfg->cc);
    mqvpn_config_set_reinjection(lib_cfg, (mqvpn_reinjection_t)cfg->reinjection);
    mqvpn_config_set_reinjection_deadline_params(lib_cfg, cfg->reinj_srtt_factor_pct,
                                                 cfg->reinj_hard_deadline_ms,
                                                 cfg->reinj_deadline_lower_bound_ms);
    mqvpn_config_set_init_max_path_id(lib_cfg, cfg->init_max_path_id);
    mqvpn_config_set_tun_mtu(lib_cfg, cfg->tun_mtu);
    mqvpn_config_set_fec(lib_cfg, cfg->fec_enable);
    mqvpn_config_set_fec_scheme(lib_cfg, (mqvpn_fec_scheme_t)cfg->fec_scheme);
    mqvpn_config_apply_reorder(lib_cfg,
                               &cfg->reorder); /* INI [Reorder]/[ReorderRule] bridge */
    mqvpn_config_apply_hybrid(lib_cfg, &cfg->hybrid); /* INI [Hybrid] bridge */
    if (cfg->recv_rate_limit)
        mqvpn_config_set_recv_rate_limit(lib_cfg, cfg->recv_rate_limit);
    mqvpn_config_set_sync_path_labels(lib_cfg, cfg->sync_path_labels);
    mqvpn_config_set_push_path_labels(lib_cfg, cfg->push_path_labels);
}
