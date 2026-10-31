// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * control_socket.c — TCP control API for mqvpn server
 *
 * Supported JSON commands:
 *
 *   {"cmd":"add_user",    "name":"alice","key":"alice-secret"[,"fixed_ip":"10.0.0.5"]}
 *   {"cmd":"set_user_fixed_ip", "name":"alice","fixed_ip":"10.0.0.5"}  ("" clears)
 *   {"cmd":"remove_user", "name":"alice"}
 *   {"cmd":"list_users"}
 *   {"cmd":"get_stats"}
 *   {"cmd":"get_status"}
 *   {"cmd":"get_build_info"}
 *   {"cmd":"get_fec_stats","user":"alice"}
 *   {"cmd":"get_all_fec_stats"}
 *   {"cmd":"get_reorder_stats"}
 *
 * Client-only commands (not available in server mode) — keyed by local
 * interface name:
 *   {"cmd":"add_path",        "iface":"eth0"}
 *   {"cmd":"remove_path",     "iface":"eth0"}
 *   {"cmd":"list_paths"}
 *   {"cmd":"set_path_weight", "iface":"eth0","weight":3}
 *   {"cmd":"set_path_dscp_mask", "iface":"eth0","dscp_mask":70368744177664}
 *
 * set_path_weight / set_path_dscp_mask are ALSO available in server mode
 * — but usually you don't need them: the client already reports its own
 * weight/dscp_mask to the server via a PATH_LABEL capsule (see
 * mqvpn_path_label.h) every time it (re)activates a path, and the server
 * adopts that value for its own downlink scheduling BY DEFAULT. Calling
 * set_path_weight/set_path_dscp_mask on the client side alone is enough
 * for both directions to agree. The server-mode form below exists to
 * PIN the server to a *different* value than the client (asymmetric
 * control) — an explicit call here always overrides the client's report,
 * from then on, including across future reconnects. A server has no
 * local interface for a path, so it takes EITHER `iface` (the same name
 * the client uses — persists across a full path teardown/recreate,
 * same as the client's own auto-reported value) OR the raw `path_id`
 * (one-shot, see get_status's per-path output; does NOT persist —
 * path_id can change on reconnect):
 *   {"cmd":"set_path_weight",    "user":"alice","iface":"wlan0","weight":3}
 *   {"cmd":"set_path_dscp_mask",
 * "user":"alice","iface":"wlan0","dscp_mask":70368744177664}
 *   {"cmd":"set_path_weight",    "user":"alice","path_id":2,"weight":3}
 *   {"cmd":"set_path_dscp_mask", "user":"alice","path_id":2,"dscp_mask":70368744177664}
 * `iface` takes priority if both are given. The iface form can be issued
 * before that path has even come up — the value is stored and applied the
 * moment the PATH_LABEL announcement for it arrives.
 *
 * Responses:
 *   {"ok":true}
 *   {"ok":false,"error":"<reason>"}
 *   {"ok":true,"users":["alice","bob"]}
 *   {"ok":true,"n_clients":N,"bytes_tx":X,"bytes_rx":Y,
 *    "dgram_sent":S,"dgram_recv":R,"dgram_lost":L,"dgram_acked":A,
 *    "uptime_sec":U}
 *   {"ok":true,"version":"0.7.0","scheduler":"backup_fec","fec_enabled":1}
 *   {"ok":true,"user":"alice","enable_fec":1,"mp_state":1,
 *    "mp_state_label":"active_with_standby",
 *    "fec_send_cnt":142,"fec_recover_cnt":17,"lost_dgram_cnt":23,
 *    "total_app_bytes":9123456,"standby_app_bytes":421337}
 *   {"ok":true,"n_clients":N,"clients":[{"user":"alice","enable_fec":1,
 *    "mp_state":1,"mp_state_label":"active_with_standby", ...}, ...]}
 *   {"ok":true,"reorder":{"gap_count":N,"gap_filled_count":N,
 *    "gap_timeout_count":N,"gap_overflow_count":N,"gap_demote_count":N,
 *    "gap_reset_count":N,"ack_demote_count":N,"too_late_drop_count":N,
 *    "too_far_ahead_drop_count":N,"duplicate_drop_count":N,"pool_drop_count":N,
 *    "per_flow_limit_drop_count":N,"reset_discard_count":N,"delivered_count":N,
 *    "added_latency_p99_ms":F,"added_latency_max_ms":F,
 *    "added_latency_buffered_p99_ms":F}}
 */

#include "control_socket.h"
#include "platform_internal.h"
#include "json_util.h"
#include "log.h"

#include <event2/event.h>
#include "mqvpn_internal.h" /* mqvpn_server_scheduler_label,
                               mqvpn_path_state_label,
                               mqvpn_internal_fec_stats_t (carries mp_state_label),
                               mqvpn_server_get_client_fec_stats,
                               mqvpn_server_get_all_fec_stats,
                               mqvpn_server_get_reorder_stats,
                               mqvpn_reorder_stats_t (via reorder.h),
                               mqvpn_internal_client_reinject_t,
                               mqvpn_server_get_client_reinject,
                               mqvpn_internal_client_sched_t,
                               mqvpn_server_get_client_sched */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <inttypes.h>
#include <sys/time.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>

#define CTRL_MAX_REQ           4096 /* per-connection request buffer */
#define CTRL_MAX_CONNS         8    /* max concurrent control connections */
#define CTRL_MAX_REJECTS       8    /* refused connections lingering at once */
#define CTRL_REJECT_LINGER_MS  1000 /* how long a refused one may linger */
#define CTRL_CONN_DEADLINE_SEC 5    /* whole request+reply must finish within 5s */
/* CTRL_MAX_RESP_BYTES is in control_socket.h; test_control_socket checks that
 * the worst-case get_status fits. */

#ifndef MSG_NOSIGNAL
#  define MSG_NOSIGNAL 0 /* platforms without it rely on SO_NOSIGPIPE/SIG_IGN */
#endif

/* ── Per-connection state ────────────────────────────────────────────────── */

typedef struct ctrl_conn_s {
    int fd;
    struct event *ev;       /* EV_READ while reading, then EV_WRITE for the reply */
    struct event *deadline; /* one-shot: absolute per-connection deadline */
    char req[CTRL_MAX_REQ + 1];
    size_t req_len;
    char *out; /* pending reply (heap), NULL while still reading */
    size_t out_len;
    size_t out_off;
    ctrl_socket_t *cs;
    struct ctrl_conn_s *prev, *next; /* cs->conns list, for destroy */
} ctrl_conn_t;

/* ── Server handle ───────────────────────────────────────────────────────── */

struct ctrl_socket_s {
    int listen_fd;
    struct event *ev_accept;
    struct event_base *eb;
    mqvpn_server_t *server;
    platform_ctx_t *cli_ctx;
    /* Getter into the platform's transport ctx for the RX offload counters;
     * see ctrl_socket_create's doc for why these do not travel through
     * mqvpn_stats_t. NULL fn = report 0. */
    ctrl_rx_stats_fn rx_stats;
    void *rx_ctx;
    int n_conns;        /* active control connections */
    ctrl_conn_t *conns; /* the active connections themselves */
    /* Connections refused for "too many connections", held open until the
     * peer's request is drained (see ctrl_reject). */
    struct ctrl_reject {
        int fd;
        struct event *ev; /* NULL = free slot */
    } rejects[CTRL_MAX_REJECTS];
};

/* ── Response building ───────────────────────────────────────────────────── */

/* Writes doc into resp as compact JSON and frees it. A NULL doc (a handler's
 * malloc failure) or a response that does not fit resp_len gives the fixed
 * envelope, never a cut-off prefix. Invalid UTF-8 passes through byte for
 * byte: intake does not validate it, and name[64] can cut a character. */
static int
ctrl_reply(yyjson_mut_doc *doc, char *resp, size_t resp_len)
{
    size_t n = yyjson_mut_write_buf(resp, resp_len, doc,
                                    YYJSON_WRITE_ALLOW_INVALID_UNICODE, NULL);
    yyjson_mut_doc_free(doc);
    if (n == 0)
        return snprintf(resp, resp_len,
                        "{\"ok\":false,\"error\":\"response too large\"}");
    return (int)n;
}

/* {"ok":false,"error":msg} */
static int
ctrl_error(char *resp, size_t resp_len, const char *msg)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", false);
    ok &= yyjson_mut_obj_add_strcpy(doc, root, "error", msg);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

/* {"ok":true} */
static int
ctrl_ok(char *resp, size_t resp_len)
{
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    if (!yyjson_mut_obj_add_bool(doc, root, "ok", true)) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

/* A latency in ms as a number with at most 3 decimals (the old "%.3f"). */
static bool
ctrl_add_ms(yyjson_mut_doc *doc, yyjson_mut_val *obj, const char *key, double ms)
{
    yyjson_mut_val *v = yyjson_mut_real(doc, ms);
    return yyjson_mut_set_fp_to_fixed(v, 3) && yyjson_mut_obj_add_val(doc, obj, key, v);
}

/* ── Request fields ──────────────────────────────────────────────────────── */

/* A string field that fits out (out_len - 1 bytes), copied into it: 0.
 * Absent, not a string, with an embedded NUL, or too long: -1 (an over-long
 * value is refused, never used shortened). */
static int
ctrl_get_str(yyjson_val *req, const char *key, char *out, size_t out_len)
{
    yyjson_val *v = yyjson_obj_get(req, key);
    if (yyjson_get_len(v) >= out_len) return -1;
    return json_copy_str(v, out, out_len);
}

/* Requests are parsed with numbers kept as their raw text (see dispatch), so
 * the numeric fields get exactly these rules: a bare unsigned decimal, no
 * sign, fraction, exponent or overflow; with allow_hex also a 0x-prefixed
 * hexadecimal token (a documented extension for dscp_mask: masks built from
 * MQVPN_DSCP_BIT() read more naturally in hex). Anything else: -1. */
static int
ctrl_get_u64(yyjson_val *v, int allow_hex, uint64_t *out)
{
    const char *s = yyjson_get_raw(v);
    if (!s) return -1;
    int base = 10;
    if (allow_hex && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s += 2;
        base = 16;
        if (!isxdigit((unsigned char)*s)) return -1;
    } else if (!isdigit((unsigned char)*s)) {
        return -1;
    }
    errno = 0;
    char *end = NULL;
    unsigned long long m = strtoull(s, &end, base);
    if (errno == ERANGE || *end != '\0') return -1;
    *out = (uint64_t)m;
    return 0;
}

/* "weight": a JSON number 0..65535 — no string, sign, fraction or junk. */
static int
ctrl_read_weight(yyjson_val *v, uint32_t *out)
{
    uint64_t w = 0;
    if (ctrl_get_u64(v, 0, &w) < 0 || w > 65535) return -1;
    *out = (uint32_t)w;
    return 0;
}

/* "dscp_mask": a non-negative JSON number, or a bare 0x-prefixed hex token. */
static int
ctrl_read_dscp_mask(yyjson_val *v, uint64_t *out)
{
    return ctrl_get_u64(v, 1, out);
}

/* ── Command dispatch ────────────────────────────────────────────────────── */

/* Each handler creates a doc for its reply, adds the keys in wire order (keys,
 * order and JSON types are a compatibility surface), and hands the doc to
 * ctrl_reply, which frees it. Every add result is folded into `ok`; a failed
 * add (malloc) turns the reply into the envelope. */

static int
ctrl_cmd_add_user(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    char name[64] = {0}, key[256] = {0}, fixed_ip[20] = {0};
    yyjson_val *name_val = yyjson_obj_get(req, "name");
    if (json_copy_str(name_val, name, sizeof(name)) < 0 ||
        ctrl_get_str(req, "key", key, sizeof(key)) < 0)
        return ctrl_error(resp, resp_len, "name and key required");
    if (yyjson_get_len(name_val) >= sizeof(name))
        return ctrl_error(resp, resp_len, "name exceeds 63 bytes");

    if (yyjson_obj_get(req, "fixed_ip") &&
        ctrl_get_str(req, "fixed_ip", fixed_ip, sizeof(fixed_ip)) < 0)
        return ctrl_error(resp, resp_len, "fixed_ip invalid or unavailable");

    /* Atomic: with a fixed_ip, nothing is committed unless the address is
     * accepted. An existing user gets the fixed_ip applied first, so a
     * refused address leaves its key untouched; a new user is added, and
     * removed again if the address is refused (it cannot have a session yet:
     * the loop is single-threaded). */
    int existed = 0;
    if (fixed_ip[0]) {
        char unames[MQVPN_MAX_USERS][64];
        int n = mqvpn_server_list_users(server, unames, MQVPN_MAX_USERS);
        for (int i = 0; i < n && !existed; i++)
            existed = strcmp(unames[i], name) == 0;
        if (existed && mqvpn_server_set_user_fixed_ip(server, name, fixed_ip) != MQVPN_OK)
            return ctrl_error(resp, resp_len, "fixed_ip invalid or unavailable");
    }

    int rc = mqvpn_server_add_user(server, name, key);
    if (rc != MQVPN_OK) {
        char m[48];
        snprintf(m, sizeof(m), "add_user failed (%d)", rc);
        return ctrl_error(resp, resp_len, m);
    }
    if (fixed_ip[0] && !existed) {
        rc = mqvpn_server_set_user_fixed_ip(server, name, fixed_ip);
        if (rc != MQVPN_OK) {
            (void)mqvpn_server_remove_user(server, name);
            return ctrl_error(resp, resp_len, "fixed_ip invalid or unavailable");
        }
    }
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_set_user_fixed_ip(yyjson_val *req, char *resp, size_t resp_len,
                           ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    char name[64] = {0}, fixed_ip[20] = {0};
    yyjson_val *name_val = yyjson_obj_get(req, "name");
    if (json_copy_str(name_val, name, sizeof(name)) < 0)
        return ctrl_error(resp, resp_len, "name required");
    if (yyjson_get_len(name_val) >= sizeof(name))
        return ctrl_error(resp, resp_len, "name exceeds 63 bytes");
    /* A fixed_ip that is not a string or does not fit is refused rather than
     * applied shortened (absent still means "clear"). */
    int rc = MQVPN_ERR_INVALID_ARG;
    if (!yyjson_obj_get(req, "fixed_ip") ||
        ctrl_get_str(req, "fixed_ip", fixed_ip, sizeof(fixed_ip)) == 0)
        rc = mqvpn_server_set_user_fixed_ip(server, name, fixed_ip);
    if (rc != MQVPN_OK) {
        char m[48];
        snprintf(m, sizeof(m), "set_user_fixed_ip failed (%d)", rc);
        return ctrl_error(resp, resp_len, m);
    }
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_remove_user(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    char name[64] = {0};
    yyjson_val *name_val = yyjson_obj_get(req, "name");
    if (json_copy_str(name_val, name, sizeof(name)) < 0)
        return ctrl_error(resp, resp_len, "name required");
    if (yyjson_get_len(name_val) >= sizeof(name))
        return ctrl_error(resp, resp_len, "name exceeds 63 bytes");

    int rc = mqvpn_server_remove_user(server, name);
    if (rc != MQVPN_OK) return ctrl_error(resp, resp_len, "user not found");
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_list_users(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    (void)req;
    char unames[MQVPN_MAX_USERS][64];
    int n_users = mqvpn_server_list_users(server, unames, MQVPN_MAX_USERS);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    yyjson_mut_val *users = yyjson_mut_obj_add_arr(doc, root, "users");
    ok &= users != NULL;
    for (int i = 0; i < n_users; i++)
        ok &= yyjson_mut_arr_add_strcpy(doc, users, unames[i]);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

static int
ctrl_cmd_get_stats(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    (void)req;
    mqvpn_stats_t st = {0};
    st.struct_size = sizeof(st);
    int nc;
    uint64_t uptime;

    /* Client mode: cs->server is NULL by design (platform_linux.c passes
     * NULL there, non-NULL cli_ctx — see the add_path gate in dispatch).
     * mqvpn_server_get_stats/get_n_clients/uptime_seconds all read ONLY the
     * (NULL) server pointer, so this branch is required, not optional --
     * without it every field below silently reads 0 in client mode despite
     * a live, traffic-carrying tunnel (found live on an OpenMPTCProuter
     * bench: real udp_rx_* counters alongside all-zero bytes_tx/rx,
     * dgram_*, uptime_sec). n_clients here means "is the tunnel
     * established" (0 or 1), matching the server's "how many peers are
     * connected to me" semantics from this side's point of view. */
    if (cli_ctx) {
        mqvpn_client_get_stats(cli_ctx->client, &st);
        nc = (mqvpn_client_get_state(cli_ctx->client) == MQVPN_STATE_ESTABLISHED) ? 1 : 0;
        uptime = mqvpn_client_uptime_seconds(cli_ctx->client);
    } else {
        mqvpn_server_get_stats(server, &st);
        nc = mqvpn_server_get_n_clients(server);
        uptime = mqvpn_server_uptime_seconds(server);
    }
    uint64_t rx_receives = 0, rx_datagrams = 0;
    if (cs->rx_stats) cs->rx_stats(cs->rx_ctx, &rx_receives, &rx_datagrams);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    ok &= yyjson_mut_obj_add_int(doc, root, "n_clients", nc);
    ok &= yyjson_mut_obj_add_uint(doc, root, "bytes_tx", st.bytes_tx);
    ok &= yyjson_mut_obj_add_uint(doc, root, "bytes_rx", st.bytes_rx);
    ok &= yyjson_mut_obj_add_uint(doc, root, "dgram_sent", st.dgram_sent);
    ok &= yyjson_mut_obj_add_uint(doc, root, "dgram_recv", st.dgram_recv);
    ok &= yyjson_mut_obj_add_uint(doc, root, "dgram_lost", st.dgram_lost);
    ok &= yyjson_mut_obj_add_uint(doc, root, "dgram_acked", st.dgram_acked);
    ok &= yyjson_mut_obj_add_uint(doc, root, "pkts_lane_tcp", st.pkts_lane_tcp);
    ok &= yyjson_mut_obj_add_uint(doc, root, "pkts_lane_dgram", st.pkts_lane_dgram);
    ok &= yyjson_mut_obj_add_uint(doc, root, "pkts_lane_raw", st.pkts_lane_raw);
    ok &= yyjson_mut_obj_add_uint(doc, root, "pkts_lane_tcp_dropped",
                                  st.pkts_lane_tcp_dropped);
    ok &= yyjson_mut_obj_add_uint(doc, root, "tcp_flows_active", st.tcp_flows_active);
    ok &= yyjson_mut_obj_add_uint(doc, root, "tcp_flows_total", st.tcp_flows_total);
    ok &= yyjson_mut_obj_add_uint(doc, root, "tcp_flows_rejected", st.tcp_flows_rejected);
    ok &= yyjson_mut_obj_add_uint(doc, root, "raw_markers_active", st.raw_markers_active);
    /* udp_tx_* from the library (it issues those sends); udp_rx_* from the
     * platform's transport getter (RX offload never crosses the library
     * ABI). datagrams/sends and datagrams/receives are the achieved
     * batching and coalescing factors — 1.0 means every datagram cost
     * its own syscall, which the one-shot "udp-gso:"/"udp-gro:" startup
     * markers cannot distinguish because they only report the kernel
     * capability probe. */
    ok &= yyjson_mut_obj_add_uint(doc, root, "udp_tx_sends", st.udp_tx_sends);
    ok &= yyjson_mut_obj_add_uint(doc, root, "udp_tx_datagrams", st.udp_tx_datagrams);
    ok &= yyjson_mut_obj_add_uint(doc, root, "udp_rx_receives", rx_receives);
    ok &= yyjson_mut_obj_add_uint(doc, root, "udp_rx_datagrams", rx_datagrams);
    ok &= yyjson_mut_obj_add_uint(doc, root, "uptime_sec", uptime);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

static int
ctrl_cmd_get_status(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    (void)req;
    mqvpn_client_info_t clients[MQVPN_MAX_USERS];
    int n_clients = 0;

    /* Index-aligned with clients[] above — contract is on
     * mqvpn_server_get_client_reinject() in mqvpn_internal.h. Matched by
     * path_id below anyway, not by array position alone. */
    mqvpn_internal_client_reinject_t reinj[MQVPN_MAX_USERS];
    int n_reinj = 0;

    /* Client mode: mqvpn_server_get_client_info(NULL, ...) always yields
     * n_clients=0 -- found live on a bench router genuinely mid-tunnel
     * (real traffic flowing) yet get_status reported an empty client list.
     * mqvpn_client_get_info()/get_reinject() fill the exact same public
     * mqvpn_client_info_t/mqvpn_internal_client_reinject_t shapes for this
     * client's own upstream connection (one synthetic "client" entry, or
     * zero while not yet connected), so the serialization loop below is
     * unchanged either way. */
    if (cli_ctx) {
        n_clients = (mqvpn_client_get_info(cli_ctx->client, &clients[0]) > 0) ? 1 : 0;
        if (n_clients) {
            n_reinj = mqvpn_client_get_reinject(cli_ctx->client, &reinj[0]);
            if (n_reinj < 0) n_reinj = 0;
        }
    } else {
        mqvpn_server_get_client_info(server, clients, MQVPN_MAX_USERS, &n_clients);
        n_reinj = mqvpn_server_get_client_reinject(server, reinj, MQVPN_MAX_USERS);
        if (n_reinj < 0) n_reinj = 0;
    }

    /* Same index alignment, see mqvpn_server_get_client_sched(). */
    mqvpn_internal_client_sched_t sched[MQVPN_MAX_USERS];
    int n_sched = mqvpn_server_get_client_sched(server, sched, MQVPN_MAX_USERS);
    if (n_sched < 0) n_sched = 0;

    uint64_t now = 0;
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0)
        now = (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    ok &= yyjson_mut_obj_add_int(doc, root, "n_clients", n_clients);
    yyjson_mut_val *jclients = yyjson_mut_obj_add_arr(doc, root, "clients");
    ok &= jclients != NULL;

    for (int i = 0; i < n_clients; i++) {
        mqvpn_client_info_t *ci = &clients[i];
        uint64_t conn_sec = (ci->connected_at_us > 0 && now > ci->connected_at_us)
                                ? (now - ci->connected_at_us) / 1000000
                                : 0;

        yyjson_mut_val *jc = yyjson_mut_arr_add_obj(doc, jclients);
        ok &= yyjson_mut_obj_add_strcpy(doc, jc, "user", ci->username);
        ok &= yyjson_mut_obj_add_strcpy(doc, jc, "endpoint", ci->endpoint);
        ok &= yyjson_mut_obj_add_strcpy(doc, jc, "scheduler",
                                        i < n_sched ? sched[i].scheduler : "unknown");
        ok &= yyjson_mut_obj_add_uint(doc, jc, "connected_sec", conn_sec);
        ok &= yyjson_mut_obj_add_uint(doc, jc, "bytes_tx", ci->bytes_tx);
        ok &= yyjson_mut_obj_add_uint(doc, jc, "bytes_rx", ci->bytes_rx);
        ok &= yyjson_mut_obj_add_int(doc, jc, "n_paths", ci->n_paths);
        yyjson_mut_val *jpaths = yyjson_mut_obj_add_arr(doc, jc, "paths");
        ok &= jpaths != NULL;

        for (int p = 0; p < ci->n_paths; p++) {
            mqvpn_path_stats_t *ps = &ci->paths[p];

            /* Emit 0 on mismatch/absence (index out of range, or no path_id
             * match within reinj[i]) rather than skipping the field — every
             * path object keeps the same shape. */
            uint64_t reinject_tx_bytes = 0;
            if (i < n_reinj) {
                mqvpn_internal_client_reinject_t *re = &reinj[i];
                for (int rp = 0; rp < re->n_paths; rp++) {
                    if (re->paths[rp].path_id == ps->path_id) {
                        reinject_tx_bytes = re->paths[rp].reinject_tx_bytes;
                        break;
                    }
                }
            }

            yyjson_mut_val *jp = yyjson_mut_arr_add_obj(doc, jpaths);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "path_id", ps->path_id);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "srtt_ms", ps->srtt_us / 1000);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "min_rtt_ms", ps->min_rtt_us / 1000);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "cwnd", ps->cwnd);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "in_flight", ps->bytes_in_flight);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "bytes_tx", ps->bytes_tx);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "bytes_rx", ps->bytes_rx);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "pkt_sent", ps->pkt_sent);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "pkt_recv", ps->pkt_recv);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "pkt_lost", ps->pkt_lost);
            ok &= yyjson_mut_obj_add_uint(doc, jp, "state", ps->state);
            ok &= yyjson_mut_obj_add_strcpy(doc, jp, "state_label",
                                            mqvpn_path_state_label(ps->state));
            ok &=
                yyjson_mut_obj_add_uint(doc, jp, "reinject_tx_bytes", reinject_tx_bytes);
        }
    }
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

static int
ctrl_cmd_get_build_info(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    (void)req;
    const char *ver = mqvpn_version_string();
    /* Client mode: mqvpn_server_scheduler_label(NULL) degrades to the
     * generic "unknown" fallback rather than an error, which is how this
     * was found live -- the scheduler always read "unknown" on a
     * client-mode router regardless of its real configured scheduler. */
    const char *sched = cli_ctx ? mqvpn_client_scheduler_label(cli_ctx->client)
                                : mqvpn_server_scheduler_label(server);
#ifdef XQC_ENABLE_FEC
    int fec_enabled = 1;
#else
    int fec_enabled = 0;
#endif
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    ok &= yyjson_mut_obj_add_strcpy(doc, root, "version", ver ? ver : "unknown");
    ok &= yyjson_mut_obj_add_strcpy(doc, root, "scheduler", sched);
    ok &= yyjson_mut_obj_add_int(doc, root, "fec_enabled", fec_enabled);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

/* One FEC stats object's fields after "user", shared by get_fec_stats and
 * get_all_fec_stats (same keys, same order). */
static bool
ctrl_add_fec_fields(yyjson_mut_doc *doc, yyjson_mut_val *o,
                    const mqvpn_internal_fec_stats_t *fs)
{
    bool ok = yyjson_mut_obj_add_uint(doc, o, "enable_fec", fs->enable_fec);
    ok &= yyjson_mut_obj_add_uint(doc, o, "mp_state", fs->mp_state);
    ok &= yyjson_mut_obj_add_strcpy(doc, o, "mp_state_label",
                                    fs->mp_state_label ? fs->mp_state_label : "unknown");
    ok &= yyjson_mut_obj_add_uint(doc, o, "fec_send_cnt", fs->fec_send_cnt);
    ok &= yyjson_mut_obj_add_uint(doc, o, "fec_recover_cnt", fs->fec_recover_cnt);
    ok &= yyjson_mut_obj_add_uint(doc, o, "lost_dgram_cnt", fs->lost_dgram_cnt);
    ok &= yyjson_mut_obj_add_uint(doc, o, "total_app_bytes", fs->total_app_bytes);
    ok &= yyjson_mut_obj_add_uint(doc, o, "standby_app_bytes", fs->standby_app_bytes);
    return ok;
}

static int
ctrl_cmd_get_fec_stats(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    char user[64] = {0};
    yyjson_val *user_val = yyjson_obj_get(req, "user");
    if (json_copy_str(user_val, user, sizeof(user)) < 0)
        return ctrl_error(resp, resp_len, "user required");
    if (yyjson_get_len(user_val) >= sizeof(user))
        return ctrl_error(resp, resp_len, "name exceeds 63 bytes");

    mqvpn_internal_fec_stats_t fs;
    int rc;

    /* Client mode: there is only ever one possible identity (this client's
     * own upstream connection, not one of many server-side users), so
     * "user not found" here means either not connected at all, or the
     * caller asked for a username that isn't this client's own
     * configured auth_username -- checked via mqvpn_client_get_info()'s
     * username field rather than adding a separate getter. */
    if (cli_ctx) {
        mqvpn_client_info_t info;
        int got = mqvpn_client_get_info(cli_ctx->client, &info);
        if (got <= 0 || strncmp(info.username, user, sizeof(info.username)) != 0)
            return ctrl_error(resp, resp_len, "user not found");
        rc = mqvpn_client_get_fec_stats(cli_ctx->client, &fs);
    } else {
        rc = mqvpn_server_get_client_fec_stats(server, user, &fs);
    }
    if (rc < 0) return ctrl_error(resp, resp_len, "fec not built");
    if (rc == 0) return ctrl_error(resp, resp_len, "user not found");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    ok &= yyjson_mut_obj_add_strcpy(doc, root, "user", user);
    ok &= ctrl_add_fec_fields(doc, root, &fs);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

static int
ctrl_cmd_get_all_fec_stats(yyjson_val *req, char *resp, size_t resp_len,
                           ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    (void)req;
    /* Bulk variant collapsing the per-user N+1 RPC pattern in scrapers
     * (Prometheus exporter) to a single call. Same XQC_ENABLE_FEC guard
     * as get_fec_stats — we surface "fec not built" so the consumer can
     * stop probing for the rest of the scrape. */
    mqvpn_internal_fec_entry_t entries[MQVPN_MAX_USERS];
    int n;

    /* Client mode: at most one entry, this client's own connection. */
    if (cli_ctx) {
        mqvpn_internal_fec_stats_t fs;
        int rc = mqvpn_client_get_fec_stats(cli_ctx->client, &fs);
        if (rc < 0) return ctrl_error(resp, resp_len, "fec not built");
        if (rc == 1) {
            mqvpn_client_info_t info;
            if (mqvpn_client_get_info(cli_ctx->client, &info) > 0)
                snprintf(entries[0].user, sizeof(entries[0].user), "%s", info.username);
            else
                entries[0].user[0] = '\0';
            entries[0].stats = fs;
            n = 1;
        } else {
            n = 0;
        }
    } else {
        n = mqvpn_server_get_all_fec_stats(server, entries, MQVPN_MAX_USERS);
        if (n < 0) return ctrl_error(resp, resp_len, "fec not built");
    }

    /* Field name parity with get_status: "n_clients" + "clients[]". A
     * connected user IS a client in mqvpn nomenclature; "users" is used
     * by list_users for the registered auth-table users (a superset). */
    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    ok &= yyjson_mut_obj_add_int(doc, root, "n_clients", n);
    yyjson_mut_val *jclients = yyjson_mut_obj_add_arr(doc, root, "clients");
    ok &= jclients != NULL;
    for (int i = 0; i < n; i++) {
        yyjson_mut_val *jc = yyjson_mut_arr_add_obj(doc, jclients);
        ok &= yyjson_mut_obj_add_strcpy(doc, jc, "user", entries[i].user);
        ok &= ctrl_add_fec_fields(doc, jc, &entries[i].stats);
    }
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

static int
ctrl_cmd_get_reorder_stats(yyjson_val *req, char *resp, size_t resp_len,
                           ctrl_socket_t *cs)
{
    mqvpn_server_t *server = cs->server;
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    (void)req;
    /* Aggregate reorder-shim RX counters across all live conns (§17). One
     * fixed-shape object, no per-conn array. The getter zero-fills when no
     * conn has reorder enabled, so the JSON is always well-formed (all-zero
     * counters).
     *
     * Client mode: mqvpn_server_get_reorder_stats(NULL, ...) unconditionally
     * fails (-1 on a NULL server), which is exactly what this command did
     * on every client-mode router before this branch existed --
     * mqvpn_client_get_reorder_stats() already existed (wired into the
     * Android/iOS bridges) but was never connected to the Linux control
     * socket, a merge-artifact gap confirmed via git history (b88642c's
     * pre-cli_ctx origin, merged in bcce90c without adding the branch
     * add_path/list_paths already gate on). */
    mqvpn_reorder_stats_t rs;
    int rc = cli_ctx ? mqvpn_client_get_reorder_stats(cli_ctx->client, &rs)
                     : mqvpn_server_get_reorder_stats(server, &rs);
    if (rc < 0) return ctrl_error(resp, resp_len, "internal error");

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    yyjson_mut_val *r = yyjson_mut_obj_add_obj(doc, root, "reorder");
    ok &= r != NULL;
    ok &= yyjson_mut_obj_add_uint(doc, r, "gap_count", rs.gap_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "gap_filled_count", rs.gap_filled_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "gap_timeout_count", rs.gap_timeout_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "gap_overflow_count", rs.gap_overflow_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "gap_demote_count", rs.gap_demote_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "gap_reset_count", rs.gap_reset_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "ack_demote_count", rs.ack_demote_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "too_late_drop_count", rs.too_late_drop_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "too_far_ahead_drop_count",
                                  rs.too_far_ahead_drop_count);
    ok &=
        yyjson_mut_obj_add_uint(doc, r, "duplicate_drop_count", rs.duplicate_drop_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "pool_drop_count", rs.pool_drop_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "per_flow_limit_drop_count",
                                  rs.per_flow_limit_drop_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "reset_discard_count", rs.reset_discard_count);
    ok &= yyjson_mut_obj_add_uint(doc, r, "delivered_count", rs.delivered_count);
    ok &= ctrl_add_ms(doc, r, "added_latency_p99_ms",
                      mqvpn_reorder_latency_percentile(&rs, 0.99));
    ok &=
        ctrl_add_ms(doc, r, "added_latency_max_ms", (double)rs.residence_max_us / 1000.0);
    ok &= ctrl_add_ms(doc, r, "added_latency_buffered_p99_ms",
                      mqvpn_reorder_latency_buffered_percentile(&rs, 0.99));
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

/* "iface" of a path command: a non-empty string that fits an interface name. */
static int
ctrl_get_iface(yyjson_val *req, char iface[IFNAMSIZ])
{
    return ctrl_get_str(req, "iface", iface, IFNAMSIZ) == 0 && iface[0] != '\0' ? 0 : -1;
}

static int
ctrl_cmd_add_path(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    if (!cli_ctx) return ctrl_error(resp, resp_len, "not supported in server mode");
    char iface[IFNAMSIZ] = {0};
    if (ctrl_get_iface(req, iface) < 0)
        return ctrl_error(resp, resp_len, "iface required");
    /* "backup": true (or 1) makes it a failover-only path. */
    yyjson_val *bv = yyjson_obj_get(req, "backup");
    const char *braw = yyjson_get_raw(bv);
    int backup = yyjson_is_true(bv) || (braw && strcmp(braw, "1") == 0);
    if (platform_add_path(cli_ctx, iface, backup) < 0)
        return ctrl_error(resp, resp_len, "add_path failed");
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_remove_path(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    if (!cli_ctx) return ctrl_error(resp, resp_len, "not supported in server mode");
    char iface[IFNAMSIZ] = {0};
    if (ctrl_get_iface(req, iface) < 0)
        return ctrl_error(resp, resp_len, "iface required");
    if (platform_remove_path(cli_ctx, iface) < 0)
        return ctrl_error(resp, resp_len, "path not found or last path");
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_list_paths(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    platform_ctx_t *cli_ctx = cs->cli_ctx;
    (void)req;
    if (!cli_ctx) return ctrl_error(resp, resp_len, "not supported in server mode");
    char names[MQVPN_MAX_PATHS][IFNAMSIZ];
    int n = platform_list_paths(cli_ctx, names, MQVPN_MAX_PATHS);

    yyjson_mut_doc *doc = yyjson_mut_doc_new(NULL);
    yyjson_mut_val *root = yyjson_mut_obj(doc);
    yyjson_mut_doc_set_root(doc, root);
    bool ok = yyjson_mut_obj_add_bool(doc, root, "ok", true);
    yyjson_mut_val *paths = yyjson_mut_obj_add_arr(doc, root, "paths");
    ok &= paths != NULL;
    for (int i = 0; i < n; i++)
        ok &= yyjson_mut_arr_add_strcpy(doc, paths, names[i]);
    if (!ok) {
        yyjson_mut_doc_free(doc);
        return ctrl_reply(NULL, resp, resp_len);
    }
    return ctrl_reply(doc, resp, resp_len);
}

/* Server mode of set_path_weight / set_path_dscp_mask: no local-interface
 * concept for a server-side path. Two ways to identify one: `iface`
 * (persists across a path teardown/recreate — see
 * mqvpn_server_set_path_weight_by_iface(), requires the client to have
 * announced that iface at least once) or the raw QUIC `path_id` (one-shot,
 * does not persist — see get_status's per-path output for the current
 * path_id). Exactly one of weight / dscp_mask is applied (is_weight). */
static int
ctrl_server_set_path(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs,
                     int is_weight, uint32_t w, uint64_t m)
{
    mqvpn_server_t *server = cs->server;
    char user[64] = {0};
    if (ctrl_get_str(req, "user", user, sizeof(user)) < 0 || user[0] == '\0')
        return ctrl_error(resp, resp_len, "user required");

    int sret;
    char iface[IFNAMSIZ] = {0};
    if (ctrl_get_iface(req, iface) == 0) {
        sret = is_weight
                   ? mqvpn_server_set_path_weight_by_iface(server, user, iface, w)
                   : mqvpn_server_set_path_dscp_mask_by_iface(server, user, iface, m);
    } else {
        uint64_t path_id;
        if (ctrl_get_u64(yyjson_obj_get(req, "path_id"), 0, &path_id) < 0)
            return ctrl_error(resp, resp_len, "iface or path_id required");
        sret = is_weight ? mqvpn_server_set_path_weight(server, user, path_id, w)
                         : mqvpn_server_set_path_dscp_mask(server, user, path_id, m);
    }
    if (sret == MQVPN_ERR_INVALID_ARG)
        return ctrl_error(resp, resp_len, "user not found");
    if (sret != MQVPN_OK) return ctrl_error(resp, resp_len, "path not found");
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_set_path_weight(yyjson_val *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    yyjson_val *wv = yyjson_obj_get(req, "weight");
    if (!wv) return ctrl_error(resp, resp_len, "weight required");
    uint32_t w = 0;
    if (ctrl_read_weight(wv, &w) < 0)
        return ctrl_error(resp, resp_len, "weight must be 0-65535");

    if (!cs->cli_ctx) return ctrl_server_set_path(req, resp, resp_len, cs, 1, w, 0);

    /* Client mode: keyed by local interface name. */
    char iface[IFNAMSIZ] = {0};
    if (ctrl_get_iface(req, iface) < 0)
        return ctrl_error(resp, resp_len, "iface required");
    if (platform_set_path_weight(cs->cli_ctx, iface, w) < 0)
        return ctrl_error(resp, resp_len, "path not found");
    return ctrl_ok(resp, resp_len);
}

static int
ctrl_cmd_set_path_dscp_mask(yyjson_val *req, char *resp, size_t resp_len,
                            ctrl_socket_t *cs)
{
    yyjson_val *mv = yyjson_obj_get(req, "dscp_mask");
    if (!mv) return ctrl_error(resp, resp_len, "dscp_mask required");
    uint64_t m = 0;
    if (ctrl_read_dscp_mask(mv, &m) < 0)
        return ctrl_error(resp, resp_len, "dscp_mask must be a non-negative integer");

    if (!cs->cli_ctx) return ctrl_server_set_path(req, resp, resp_len, cs, 0, 0, m);

    /* Client mode: keyed by local interface name. */
    char iface[IFNAMSIZ] = {0};
    if (ctrl_get_iface(req, iface) < 0)
        return ctrl_error(resp, resp_len, "iface required");
    if (platform_set_path_dscp_mask(cs->cli_ctx, iface, m) < 0)
        return ctrl_error(resp, resp_len, "path not found");
    return ctrl_ok(resp, resp_len);
}

/* Commands take the whole socket context, not just the server handle: some
 * answers (get_stats' udp_rx_* pair) come from platform-owned state that
 * never crosses the library ABI, and client mode has no server handle at
 * all. Handlers that only need the server open with
 * `mqvpn_server_t *server = cs->server;`. */
typedef int (*ctrl_cmd_fn)(yyjson_val *req, char *resp, size_t resp_len,
                           ctrl_socket_t *cs);

/* Keep in sync (and in order) with the file-header command list. */
static const struct {
    const char *name;
    ctrl_cmd_fn fn;
} ctrl_cmds[] = {
    {"add_user", ctrl_cmd_add_user},
    {"set_user_fixed_ip", ctrl_cmd_set_user_fixed_ip},
    {"remove_user", ctrl_cmd_remove_user},
    {"list_users", ctrl_cmd_list_users},
    {"get_stats", ctrl_cmd_get_stats},
    {"get_status", ctrl_cmd_get_status},
    {"get_build_info", ctrl_cmd_get_build_info},
    {"get_fec_stats", ctrl_cmd_get_fec_stats},
    {"get_all_fec_stats", ctrl_cmd_get_all_fec_stats},
    {"get_reorder_stats", ctrl_cmd_get_reorder_stats},
    {"add_path", ctrl_cmd_add_path},
    {"remove_path", ctrl_cmd_remove_path},
    {"list_paths", ctrl_cmd_list_paths},
    {"set_path_weight", ctrl_cmd_set_path_weight},
    {"set_path_dscp_mask", ctrl_cmd_set_path_dscp_mask},
};

static int
dispatch(const char *req, char *resp, size_t resp_len, ctrl_socket_t *cs)
{
    /* STOP_WHEN_DONE: bytes after the first complete value (read together
     * with the request) are ignored, like the brace framing in ctrl_on_read.
     * NUMBER_AS_RAW + ALLOW_EXT_NUMBER: numbers stay as their text, so the
     * numeric fields can apply their own rules (ctrl_get_u64), including the
     * bare 0x hex extension of dscp_mask; no other field takes a number.
     * Without INSITU yyjson only reads the buffer, hence the cast. */
    yyjson_doc *doc =
        yyjson_read_opts((char *)req, strlen(req),
                         YYJSON_READ_STOP_WHEN_DONE | YYJSON_READ_NUMBER_AS_RAW |
                             YYJSON_READ_ALLOW_EXT_NUMBER,
                         NULL, NULL);
    yyjson_val *root = yyjson_doc_get_root(doc);
    char cmd[32] = {0};
    int rc;

    /* NULL doc or non-object root: yyjson_obj_get returns NULL -> -1. */
    if (json_copy_str(yyjson_obj_get(root, "cmd"), cmd, sizeof(cmd)) < 0) {
        rc = ctrl_error(resp, resp_len, "missing cmd");
    } else {
        ctrl_cmd_fn fn = NULL;
        for (size_t i = 0; i < sizeof(ctrl_cmds) / sizeof(ctrl_cmds[0]); i++)
            if (strcmp(cmd, ctrl_cmds[i].name) == 0) {
                fn = ctrl_cmds[i].fn;
                break;
            }
        rc =
            fn ? fn(root, resp, resp_len, cs) : ctrl_error(resp, resp_len, "unknown cmd");
    }
    yyjson_doc_free(doc); /* handlers neither free nor keep the request doc */
    return rc;
}

/* ── Connection handling ─────────────────────────────────────────────────── */

/* Common per-connection teardown: unhook the libevent events, close the fd,
 * release the connection's slot in the ctrl-socket's active-connection
 * count, unlink it from the connection list and free it. conn->fd is set
 * once in ctrl_on_accept() and never changes. */
static void
ctrl_conn_close(ctrl_conn_t *conn)
{
    if (conn->ev) {
        event_del(conn->ev);
        event_free(conn->ev);
    }
    if (conn->deadline) {
        event_del(conn->deadline);
        event_free(conn->deadline);
    }
    close(conn->fd);
    if (conn->prev)
        conn->prev->next = conn->next;
    else
        conn->cs->conns = conn->next;
    if (conn->next) conn->next->prev = conn->prev;
    conn->cs->n_conns--;
    free(conn->out);
    free(conn);
}

/* Absolute deadline (control-api.md §1): a connection that has not finished
 * its request and reply within CTRL_CONN_DEADLINE_SEC is closed, however
 * slowly it trickles bytes in or reads the reply out. */
static void
ctrl_on_deadline(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    ctrl_conn_close((ctrl_conn_t *)arg);
}

/* Push as much of the pending reply as the socket takes. MSG_NOSIGNAL: a
 * peer that already closed must not raise SIGPIPE. Returns 1 when the whole
 * reply is out (or the peer is gone), 0 when the rest must wait for EV_WRITE. */
static int
ctrl_conn_flush(ctrl_conn_t *conn)
{
    while (conn->out_off < conn->out_len) {
        ssize_t n = send(conn->fd, conn->out + conn->out_off,
                         conn->out_len - conn->out_off, MSG_NOSIGNAL);
        if (n > 0) {
            conn->out_off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            return 0;
        } else {
            return 1; /* peer gone: nothing more to deliver */
        }
    }
    return 1;
}

/* Finish a connection whose reply is fully written: discard whatever request
 * bytes are still queued (closing with unread data would make the kernel
 * send RST, which can destroy the reply in flight), then close. */
static void
ctrl_conn_finish(ctrl_conn_t *conn)
{
    char scratch[512];
    for (int i = 0; i < 8; i++) {
        ssize_t n = recv(conn->fd, scratch, sizeof(scratch), 0);
        if (n <= 0) break;
    }
    ctrl_conn_close(conn);
}

static void
ctrl_on_write(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    (void)what;
    ctrl_conn_t *conn = (ctrl_conn_t *)arg;
    if (ctrl_conn_flush(conn)) ctrl_conn_finish(conn);
}

/* Queue `len` bytes of `buf` (taken over by the connection) as the reply and
 * start writing it: inline if the socket takes it all, else via EV_WRITE. */
static void
ctrl_conn_reply(ctrl_conn_t *conn, char *buf, size_t len)
{
    event_del(conn->ev);
    event_free(conn->ev);
    conn->ev = NULL;
    conn->out = buf;
    conn->out_len = len;
    conn->out_off = 0;

    if (ctrl_conn_flush(conn)) {
        ctrl_conn_finish(conn);
        return;
    }
    conn->ev =
        event_new(conn->cs->eb, conn->fd, EV_WRITE | EV_PERSIST, ctrl_on_write, conn);
    if (!conn->ev || event_add(conn->ev, NULL) < 0) ctrl_conn_close(conn);
}

static void
ctrl_conn_reply_str(ctrl_conn_t *conn, const char *msg)
{
    size_t len = strlen(msg);
    char *buf = malloc(len + 1);
    if (!buf) {
        ctrl_conn_close(conn);
        return;
    }
    memcpy(buf, msg, len + 1);
    ctrl_conn_reply(conn, buf, len);
}

/* Request framing. Returns:
 *   1  a complete request: the first non-whitespace byte is '{' and its
 *      matching '}' has arrived; the buffer is cut right after that '}' so
 *      only this object is ever parsed (trailing bytes are ignored);
 *   0  need more bytes;
 *  -1  not a JSON object (the first non-whitespace byte is not '{'). */
static int
ctrl_frame_request(ctrl_conn_t *conn)
{
    size_t i = 0;
    while (i < conn->req_len && isspace((unsigned char)conn->req[i]))
        i++;
    if (i == conn->req_len) return 0;
    if (conn->req[i] != '{') return -1;

    int depth = 0, in_str = 0;
    for (; i < conn->req_len; i++) {
        char c = conn->req[i];
        if (in_str) {
            if (c == '\\') {
                i++;
                continue;
            }
            if (c == '"') in_str = 0;
        } else if (c == '"') {
            in_str = 1;
        } else if (c == '{') {
            depth++;
        } else if (c == '}' && --depth == 0) {
            conn->req[i + 1] = '\0';
            return 1;
        }
    }
    return 0;
}

static void
ctrl_conn_dispatch(ctrl_conn_t *conn)
{
    char *resp = malloc(CTRL_MAX_RESP_BYTES);
    if (!resp) {
        ctrl_conn_close(conn);
        return;
    }
    int rlen = dispatch(conn->req, resp, CTRL_MAX_RESP_BYTES - 2, conn->cs);
    if (rlen <= 0) {
        /* dispatch failed to format anything — close silently. */
        free(resp);
        ctrl_conn_close(conn);
        return;
    }
    if ((size_t)rlen >= CTRL_MAX_RESP_BYTES - 2) {
        /* The reply did not fit in resp. Send a small error JSON instead so the
         * client doesn't see a malformed body, and emit a warning. */
        free(resp);
        LOG_WRN(
            "control: dispatch response truncated (would have been %d bytes, max %zu)",
            rlen, (size_t)CTRL_MAX_RESP_BYTES - 2);
        ctrl_conn_reply_str(conn, "{\"ok\":false,\"error\":\"response too large\"}\n");
        return;
    }
    resp[rlen] = '\n';
    resp[rlen + 1] = '\0';
    char *shrunk = realloc(resp, (size_t)rlen + 2);
    if (shrunk) resp = shrunk;
    ctrl_conn_reply(conn, resp, (size_t)rlen + 1);
}

static void
ctrl_on_read(evutil_socket_t fd, short what, void *arg)
{
    (void)what;
    ctrl_conn_t *conn = (ctrl_conn_t *)arg;

    for (;;) {
        if (conn->req_len >= CTRL_MAX_REQ) {
            /* Buffer full and still no complete object. */
            ctrl_conn_reply_str(conn, "{\"ok\":false,\"error\":\"request too large\"}\n");
            return;
        }
        ssize_t n = recv(fd, conn->req + conn->req_len, CTRL_MAX_REQ - conn->req_len, 0);
        if (n > 0) {
            conn->req_len += (size_t)n;
            conn->req[conn->req_len] = '\0';
            int fr = ctrl_frame_request(conn);
            if (fr < 0) {
                /* Not a JSON object — e.g. an HTTP request from a browser or
                 * a cross-site POST. Refuse it before parsing anything. */
                ctrl_conn_reply_str(conn,
                                    "{\"ok\":false,\"error\":\"invalid request\"}\n");
                return;
            }
            if (fr > 0) break;
        } else if (n == 0) {
            /* EOF — process whatever we have: a truncated object still gets a
             * per-command error ("... required"), as before. */
            conn->req[conn->req_len] = '\0';
            break;
        } else if (errno == EINTR) {
            continue;
        } else if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return; /* wait for more data */
        } else {
            /* read error — close connection */
            ctrl_conn_close(conn);
            return;
        }
    }

    ctrl_conn_dispatch(conn);
}

/* ── Refused connections ────────────────────────────────────────────────── */

static void
ctrl_reject_free(struct ctrl_reject *r)
{
    if (!r->ev) return;
    event_free(r->ev);
    close(r->fd);
    r->ev = NULL;
}

static void
ctrl_on_reject_event(evutil_socket_t fd, short what, void *arg)
{
    (void)fd;
    struct ctrl_reject *r = arg;
    if (what & EV_READ) {
        char junk[512];
        ssize_t n;
        while ((n = recv(r->fd, junk, sizeof(junk), MSG_DONTWAIT)) > 0) {}
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
            return; /* more may follow; EV_PERSIST keeps waiting until EOF */
    }
    ctrl_reject_free(r); /* EOF, error or linger timeout */
}

/* Answer "too many connections" without losing the answer. Closing right
 * after the send, with the peer's request still unread in the socket, makes
 * the kernel reset the connection, and a reset can discard the reply before
 * the client reads it. So the write side is shut down and the connection
 * lingers (bounded in count and time) until the client's request is drained
 * and it closes. Past CTRL_MAX_REJECTS lingering ones it is closed at once. */
static void
ctrl_reject(ctrl_socket_t *cs, int cfd)
{
    const char *msg = "{\"ok\":false,\"error\":\"too many connections\"}\n";
    (void)send(cfd, msg, strlen(msg), MSG_NOSIGNAL | MSG_DONTWAIT);
    (void)shutdown(cfd, SHUT_WR);

    struct ctrl_reject *r = NULL;
    for (int i = 0; i < CTRL_MAX_REJECTS; i++) {
        if (!cs->rejects[i].ev) {
            r = &cs->rejects[i];
            break;
        }
    }
    int flags = fcntl(cfd, F_GETFL, 0);
    if (!r || flags < 0 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(cfd);
        return;
    }
    r->fd = cfd;
    r->ev = event_new(cs->eb, cfd, EV_READ | EV_PERSIST, ctrl_on_reject_event, r);
    if (!r->ev) {
        close(cfd);
        return;
    }
    struct timeval tv = {CTRL_REJECT_LINGER_MS / 1000,
                         (CTRL_REJECT_LINGER_MS % 1000) * 1000};
    if (event_add(r->ev, &tv) < 0) ctrl_reject_free(r);
}

/* ── Accept handler ──────────────────────────────────────────────────────── */

static void
ctrl_on_accept(evutil_socket_t fd, short what, void *arg)
{
    (void)what;
    ctrl_socket_t *cs = (ctrl_socket_t *)arg;

    if (cs->n_conns >= CTRL_MAX_CONNS) {
        int cfd = accept(fd, NULL, NULL);
        if (cfd >= 0) ctrl_reject(cs, cfd);
        return;
    }

    int cfd = accept(fd, NULL, NULL);
    if (cfd < 0) return;

    int flags = fcntl(cfd, F_GETFL, 0);
    if (flags < 0 || fcntl(cfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close(cfd);
        return;
    }

    ctrl_conn_t *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        close(cfd);
        return;
    }

    conn->fd = cfd;
    conn->cs = cs;
    conn->ev = event_new(cs->eb, cfd, EV_READ | EV_PERSIST, ctrl_on_read, conn);
    conn->deadline = evtimer_new(cs->eb, ctrl_on_deadline, conn);
    struct timeval tv = {.tv_sec = CTRL_CONN_DEADLINE_SEC};
    if (!conn->ev || !conn->deadline || event_add(conn->ev, NULL) < 0 ||
        evtimer_add(conn->deadline, &tv) < 0) {
        if (conn->ev) event_free(conn->ev);
        if (conn->deadline) event_free(conn->deadline);
        free(conn);
        close(cfd);
        return;
    }
    conn->next = cs->conns;
    if (cs->conns) cs->conns->prev = conn;
    cs->conns = conn;
    cs->n_conns++;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

ctrl_socket_t *
ctrl_socket_create(struct event_base *eb, const char *addr, int port,
                   mqvpn_server_t *server, void *cli_ctx, ctrl_rx_stats_fn rx_stats,
                   void *rx_ctx)
{
    if (!eb || port <= 0 || port > 65535 || (!server && !cli_ctx)) return NULL;

    if (!addr || addr[0] == '\0') addr = "127.0.0.1";

    /* Warn if exposed beyond loopback — the control API has no auth */
    if (strcmp(addr, "127.0.0.1") != 0 && strcmp(addr, "::1") != 0)
        LOG_WRN("control API: binding to non-loopback address %s — "
                "the control API has no authentication",
                addr);

    ctrl_socket_t *cs = calloc(1, sizeof(*cs));
    if (!cs) return NULL;
    cs->eb = eb;
    cs->server = server;
    cs->cli_ctx = (platform_ctx_t *)cli_ctx;
    /* Borrowed getter — the platform ctx outlives this socket. */
    cs->rx_stats = rx_stats;
    cs->rx_ctx = rx_ctx;

    /* Determine address family */
    struct sockaddr_in sin4;
    struct sockaddr_in6 sin6;
    struct sockaddr *sa;
    socklen_t sa_len;

    memset(&sin4, 0, sizeof(sin4));
    memset(&sin6, 0, sizeof(sin6));

    if (inet_pton(AF_INET6, addr, &sin6.sin6_addr) == 1) {
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons((uint16_t)port);
        sa = (struct sockaddr *)&sin6;
        sa_len = sizeof(sin6);
        cs->listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    } else {
        if (inet_pton(AF_INET, addr, &sin4.sin_addr) != 1) {
            LOG_ERR("control API: invalid address '%s'", addr);
            free(cs);
            return NULL;
        }
        sin4.sin_family = AF_INET;
        sin4.sin_port = htons((uint16_t)port);
        sa = (struct sockaddr *)&sin4;
        sa_len = sizeof(sin4);
        cs->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    }

    if (cs->listen_fd < 0) {
        LOG_ERR("control API: socket(): %s", strerror(errno));
        free(cs);
        return NULL;
    }

    int opt = 1;
    setsockopt(cs->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    if (bind(cs->listen_fd, sa, sa_len) < 0) {
        LOG_ERR("control API: bind(%s:%d): %s", addr, port, strerror(errno));
        close(cs->listen_fd);
        free(cs);
        return NULL;
    }

    if (listen(cs->listen_fd, 8) < 0) {
        LOG_ERR("control API: listen(): %s", strerror(errno));
        close(cs->listen_fd);
        free(cs);
        return NULL;
    }

    int flags = fcntl(cs->listen_fd, F_GETFL, 0);
    fcntl(cs->listen_fd, F_SETFL, flags | O_NONBLOCK);

    cs->ev_accept =
        event_new(eb, cs->listen_fd, EV_READ | EV_PERSIST, ctrl_on_accept, cs);
    if (!cs->ev_accept) {
        close(cs->listen_fd);
        free(cs);
        return NULL;
    }
    event_add(cs->ev_accept, NULL);

    LOG_INF("control API listening on %s:%d", addr, port);
    return cs;
}

void
ctrl_socket_destroy(ctrl_socket_t *cs)
{
    if (!cs) return;
    if (cs->ev_accept) {
        event_del(cs->ev_accept);
        event_free(cs->ev_accept);
    }
    if (cs->listen_fd >= 0) close(cs->listen_fd);
    /* In-flight connections hold events on cs->eb and a pointer back to cs. */
    while (cs->conns)
        ctrl_conn_close(cs->conns);
    for (int i = 0; i < CTRL_MAX_REJECTS; i++)
        ctrl_reject_free(&cs->rejects[i]);
    free(cs);
}
