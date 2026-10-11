// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_server_hardening.c — server admission, auth and backpressure
 * regressions, driven over a real loopback QUIC handshake.
 *
 * Like test_server_double_connectip.c, a REAL mqvpn server on a loopback UDP
 * socket is driven by a RAW xquic H3 client (several connections from one
 * engine), so each scenario observes what goes on the wire:
 *
 *   auth      a server started with a user stays auth-required after
 *             remove_user deletes it (403, not an open server); the Bearer
 *             scheme matches case-insensitively; a connection is closed
 *             after 3 failed auth attempts.
 *   pool      a client of a server without auth gets connected_at_us set;
 *             an exhausted address pool answers 503 and keeps the
 *             connection usable; an IPv6 ADDRESS_REQUEST on a server
 *             without IPv6 gets the ::/128 rejection, not the IPv4 /32.
 *   addrreq   ADDRESS_REQUEST is answered per requested family.
 *   pinned    a pinned user reconnecting at max_clients supersedes its old
 *             tunnel instead of getting 403.
 *   backpr    one session's datagram backpressure does not stop the
 *             downlink of another; TUN reading pauses only when every
 *             session is blocked.
 *
 * No root, no TUN device: loopback UDP sockets only.
 */

#undef NDEBUG

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#include "libmqvpn.h"
#include "mqvpn_bind_posix.h"

#include <xquic/xquic.h>
#include <xquic/xquic_typedef.h>
#include <xquic/xqc_http3.h>

/* xquic's internal clock; see test_server_double_connectip.c for why the
 * client's recv_time must come from it. */
extern xqc_usec_t xqc_now(void);

static int g_failed = 0;

#define CHECK(cond, what)                                               \
    do {                                                                \
        if (cond) {                                                     \
            printf("  %-62s PASS\n", what);                             \
        } else {                                                        \
            printf("  %-62s FAIL (%s:%d)\n", what, __FILE__, __LINE__); \
            g_failed++;                                                 \
        }                                                               \
    } while (0)

/* ─── raw xquic client ─── */

typedef struct {
    xqc_cid_t cid;
    xqc_h3_conn_t *h3;
    int handshake_done;
    int closed;
} cli_conn_t;

typedef struct {
    xqc_h3_request_t *req;
    int status; /* -1 until a :status is seen */
    int closed;
    uint8_t body[8192];
    size_t body_len;
} cli_req_t;

static int g_cli_fd = -1;
static xqc_engine_t *g_cli_engine = NULL;

static ssize_t
cli_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer,
                 socklen_t peerlen, void *conn_user_data)
{
    (void)conn_user_data;
    ssize_t n = sendto(g_cli_fd, buf, size, 0, peer, peerlen);
    return n < 0 ? XQC_SOCKET_ERROR : n;
}

static ssize_t
cli_write_socket_ex(uint64_t path_id, const unsigned char *buf, size_t size,
                    const struct sockaddr *peer, socklen_t peerlen, void *conn_user_data)
{
    (void)path_id;
    return cli_write_socket(buf, size, peer, peerlen, conn_user_data);
}

static void
cli_set_event_timer(xqc_usec_t wake_after, void *ud)
{
    (void)wake_after;
    (void)ud;
}

static void
cli_log_write(xqc_log_level_t lvl, const void *buf, size_t size, void *ud)
{
    (void)lvl;
    (void)buf;
    (void)size;
    (void)ud;
}

static int
cli_cert_verify(const unsigned char *certs[], const size_t cert_len[], size_t certs_len,
                void *ud)
{
    (void)certs;
    (void)cert_len;
    (void)certs_len;
    (void)ud;
    return 0;
}

static void
cli_save_token(const unsigned char *t, uint32_t l, void *ud)
{
    (void)t;
    (void)l;
    (void)ud;
}

static void
cli_save_session(const char *d, size_t l, void *ud)
{
    (void)d;
    (void)l;
    (void)ud;
}

static void
cli_save_tp(const char *d, size_t l, void *ud)
{
    (void)d;
    (void)l;
    (void)ud;
}

static int
cli_h3_conn_create(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid, void *user_data)
{
    (void)cid;
    cli_conn_t *c = (cli_conn_t *)user_data;
    if (c) c->h3 = h3_conn;
    return 0;
}

static int
cli_h3_conn_close(xqc_h3_conn_t *h3_conn, const xqc_cid_t *cid, void *user_data)
{
    (void)h3_conn;
    (void)cid;
    cli_conn_t *c = (cli_conn_t *)user_data;
    if (c) {
        c->closed = 1;
        c->h3 = NULL;
    }
    return 0;
}

static void
cli_h3_handshake_finished(xqc_h3_conn_t *h3_conn, void *user_data)
{
    (void)h3_conn;
    cli_conn_t *c = (cli_conn_t *)user_data;
    if (c) c->handshake_done = 1;
}

static int
cli_h3_request_close(xqc_h3_request_t *h3_request, void *user_data)
{
    (void)h3_request;
    cli_req_t *r = (cli_req_t *)user_data;
    if (r) {
        r->closed = 1;
        r->req = NULL;
    }
    return 0;
}

static int
cli_h3_request_read(xqc_h3_request_t *h3_request, xqc_request_notify_flag_t flag,
                    void *user_data)
{
    cli_req_t *r = (cli_req_t *)user_data;
    unsigned char fin = 0;
    if (flag & XQC_REQ_NOTIFY_READ_HEADER) {
        xqc_http_headers_t *headers = xqc_h3_request_recv_headers(h3_request, &fin);
        for (size_t i = 0; headers && i < headers->count; i++) {
            xqc_http_header_t *h = &headers->headers[i];
            if (h->name.iov_len == 7 && memcmp(h->name.iov_base, ":status", 7) == 0 &&
                h->value.iov_len == 3 && r) {
                const unsigned char *v = h->value.iov_base;
                r->status = (v[0] - '0') * 100 + (v[1] - '0') * 10 + (v[2] - '0');
            }
        }
    }
    if (flag & XQC_REQ_NOTIFY_READ_BODY) {
        unsigned char buf[4096];
        ssize_t n;
        while ((n = xqc_h3_request_recv_body(h3_request, buf, sizeof(buf), &fin)) > 0) {
            if (r && r->body_len + (size_t)n <= sizeof(r->body)) {
                memcpy(r->body + r->body_len, buf, (size_t)n);
                r->body_len += (size_t)n;
            }
        }
    }
    return 0;
}

static int
cli_engine_new(void)
{
    xqc_engine_ssl_config_t engine_ssl;
    memset(&engine_ssl, 0, sizeof(engine_ssl));
    engine_ssl.ciphers = XQC_TLS_CIPHERS;
    engine_ssl.groups = XQC_TLS_GROUPS;
    xqc_engine_callback_t engine_cbs = {
        .set_event_timer = cli_set_event_timer,
        .log_callbacks = {.xqc_log_write_err = cli_log_write,
                          .xqc_log_write_stat = cli_log_write},
    };
    xqc_transport_callbacks_t tcbs = {
        .write_socket = cli_write_socket,
        .write_socket_ex = cli_write_socket_ex,
        .save_token = cli_save_token,
        .save_session_cb = cli_save_session,
        .save_tp_cb = cli_save_tp,
        .cert_verify_cb = cli_cert_verify,
    };
    xqc_config_t xconfig;
    if (xqc_engine_get_default_config(&xconfig, XQC_ENGINE_CLIENT) < 0) return -1;
    xconfig.cfg_log_level = XQC_LOG_ERROR;
    g_cli_engine = xqc_engine_create(XQC_ENGINE_CLIENT, &xconfig, &engine_ssl,
                                     &engine_cbs, &tcbs, NULL);
    if (!g_cli_engine) return -1;
    xqc_h3_callbacks_t h3_cbs = {
        .h3c_cbs = {.h3_conn_create_notify = cli_h3_conn_create,
                    .h3_conn_close_notify = cli_h3_conn_close,
                    .h3_conn_handshake_finished = cli_h3_handshake_finished},
        .h3r_cbs = {.h3_request_close_notify = cli_h3_request_close,
                    .h3_request_read_notify = cli_h3_request_read},
    };
    if (xqc_h3_ctx_init(g_cli_engine, &h3_cbs) != XQC_OK) return -1;
    xqc_h3_conn_settings_t h3s = {
        .max_field_section_size = 32 * 1024,
        .qpack_blocked_streams = 64,
        .qpack_enc_max_table_capacity = 16 * 1024,
        .qpack_dec_max_table_capacity = 16 * 1024,
        .enable_connect_protocol = 1,
        .h3_datagram = 1,
    };
    xqc_h3_engine_set_local_settings(g_cli_engine, &h3s);
    return 0;
}

/* ─── server side ─── */

typedef struct {
    int svr_fd, cli_fd;
    struct sockaddr_in svr_addr, cli_addr;
    mqvpn_server_t *svr;
    void *tctx;
} env_t;

static int g_connected = 0;
static uint8_t g_last_ip[4];
static uint8_t g_last_ip6[16];
static int g_last_has_v6 = 0;

static void
svr_tun_output(const uint8_t *pkt, size_t len, void *ud)
{
    (void)pkt;
    (void)len;
    (void)ud;
}

static void
svr_tunnel_config_ready(const mqvpn_tunnel_info_t *info, void *ud)
{
    (void)info;
    (void)ud;
}

static void
svr_on_client_connected(const mqvpn_tunnel_info_t *info, uint32_t sid, void *ud)
{
    (void)sid;
    (void)ud;
    memcpy(g_last_ip, info->assigned_ip, 4);
    memcpy(g_last_ip6, info->assigned_ip6, 16);
    g_last_has_v6 = info->has_v6;
    g_connected++;
}

static int
udp_loopback(struct sockaddr_in *out)
{
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_in a = {.sin_family = AF_INET,
                            .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t alen = sizeof(*out);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(fd, (struct sockaddr *)out, &alen) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Takes ownership of cfg. */
static int
env_start(env_t *e, mqvpn_config_t *cfg)
{
    memset(e, 0, sizeof(*e));
    g_connected = 0;
    e->svr_fd = udp_loopback(&e->svr_addr);
    e->cli_fd = udp_loopback(&e->cli_addr);
    if (e->svr_fd < 0 || e->cli_fd < 0) return -1;
    g_cli_fd = e->cli_fd;

    mqvpn_config_set_listen(cfg, "127.0.0.1", ntohs(e->svr_addr.sin_port));
    mqvpn_config_set_tls_cert(cfg, TEST_CERT_FILE, TEST_KEY_FILE);
    mqvpn_config_set_log_level(cfg, MQVPN_LOG_ERROR);
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = svr_tun_output;
    cbs.tunnel_config_ready = svr_tunnel_config_ready;
    cbs.on_client_connected = svr_on_client_connected;
    e->svr = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    if (!e->svr) return -1;

    mqvpn_bind_posix_opts_t bo = {0};
    bo.struct_size = sizeof(bo);
    bo.udp_gso = 1;
    bo.socket_buf_bytes = -1;
    snprintf(bo.tag, sizeof(bo.tag), "server");
    if (mqvpn_bind_posix_server_new(e->svr_fd, &bo, &e->tctx) != MQVPN_OK) return -1;
    if (mqvpn_server_set_transport(e->svr, mqvpn_bind_posix_server_ops(), e->tctx,
                                   (struct sockaddr *)&e->svr_addr,
                                   sizeof(e->svr_addr)) != MQVPN_OK) {
        mqvpn_bind_posix_server_free(e->tctx);
        return -1;
    }
    if (mqvpn_server_start(e->svr) != MQVPN_OK) return -1;
    return cli_engine_new();
}

static void
env_stop(env_t *e)
{
    if (g_cli_engine) xqc_engine_destroy(g_cli_engine);
    g_cli_engine = NULL;
    if (e->svr) mqvpn_server_destroy(e->svr);
    if (e->cli_fd >= 0) close(e->cli_fd);
    if (e->svr_fd >= 0) close(e->svr_fd);
}

static void
pump_once(env_t *e)
{
    uint8_t buf[65536];
    xqc_engine_main_logic(g_cli_engine);
    mqvpn_bind_posix_server_drain(e->tctx, e->svr, 64);
    mqvpn_server_tick(e->svr);
    for (;;) {
        struct sockaddr_storage from;
        socklen_t flen = sizeof(from);
        ssize_t n = recvfrom(e->cli_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &flen);
        if (n <= 0) break;
        xqc_engine_packet_process(g_cli_engine, buf, (size_t)n,
                                  (struct sockaddr *)&e->cli_addr, sizeof(e->cli_addr),
                                  (struct sockaddr *)&from, flen, xqc_now(), NULL);
    }
    xqc_engine_main_logic(g_cli_engine);
}

typedef int (*cond_fn)(env_t *e, void *ctx);

/* Pump until cond holds (checked before and after each pump), at most ~7.5 s. */
static int
pump_until(env_t *e, cond_fn cond, void *ctx)
{
    for (int i = 0; i < 1500; i++) {
        if (cond(e, ctx)) return 1;
        pump_once(e);
        if (cond(e, ctx)) return 1;
        struct pollfd p[2] = {{.fd = e->svr_fd, .events = POLLIN},
                              {.fd = e->cli_fd, .events = POLLIN}};
        poll(p, 2, 5);
    }
    return cond(e, ctx);
}

static int
c_handshake(env_t *e, void *ctx)
{
    (void)e;
    cli_conn_t *c = ctx;
    return c->handshake_done || c->closed;
}

static int
c_req_answered(env_t *e, void *ctx)
{
    (void)e;
    cli_req_t *r = ctx;
    return r->status != -1 || r->closed;
}

static int
c_conn_closed(env_t *e, void *ctx)
{
    (void)e;
    return ((cli_conn_t *)ctx)->closed;
}

static int
c_n_clients(env_t *e, void *ctx)
{
    return mqvpn_server_get_n_clients(e->svr) == *(int *)ctx;
}

static int
cli_connect(env_t *e, cli_conn_t *c)
{
    memset(c, 0, sizeof(*c));
    xqc_conn_settings_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.proto_version = XQC_VERSION_V1;
    cs.max_datagram_frame_size = 65535;
    xqc_conn_ssl_config_t ssl_cfg;
    memset(&ssl_cfg, 0, sizeof(ssl_cfg));
    ssl_cfg.cert_verify_flag = XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;
    const xqc_cid_t *cid =
        xqc_h3_connect(g_cli_engine, &cs, NULL, 0, "mqvpn-test.invalid", 0, &ssl_cfg,
                       (struct sockaddr *)&e->svr_addr, sizeof(e->svr_addr), c);
    if (!cid) return -1;
    memcpy(&c->cid, (const void *)cid, sizeof(c->cid));
    pump_until(e, c_handshake, c);
    return c->handshake_done && !c->closed ? 0 : -1;
}

/* Send a CONNECT-IP request; auth = full authorization header value or NULL. */
static int
cli_connect_ip(env_t *e, cli_conn_t *c, cli_req_t *r, const char *auth)
{
    memset(r, 0, sizeof(*r));
    r->status = -1;
    r->req = xqc_h3_request_create(g_cli_engine, &c->cid, NULL, r);
    if (!r->req) return -1;
    char authority[64];
    snprintf(authority, sizeof(authority), "127.0.0.1:%d", ntohs(e->svr_addr.sin_port));
    xqc_http_header_t hdrs[7] = {
        {.name = {":method", 7}, .value = {"CONNECT", 7}},
        {.name = {":protocol", 9}, .value = {"connect-ip", 10}},
        {.name = {":scheme", 7}, .value = {"https", 5}},
        {.name = {":authority", 10}, .value = {authority, strlen(authority)}},
        {.name = {":path", 5}, .value = {"/.well-known/masque/ip/*/*/", 27}},
        {.name = {"capsule-protocol", 16}, .value = {"?1", 2}},
    };
    size_t n = 6;
    if (auth) {
        hdrs[6].name = (struct iovec){"authorization", 13};
        hdrs[6].value = (struct iovec){(void *)auth, strlen(auth)};
        n = 7;
    }
    xqc_http_headers_t headers = {.headers = hdrs, .count = n, .capacity = 7};
    if (xqc_h3_request_send_headers(r->req, &headers, 0) < 0) return -1;
    pump_until(e, c_req_answered, r);
    return r->status;
}

/* ─── ADDRESS_REQUEST helpers ─── */

typedef struct {
    uint64_t req_id;
    uint8_t ver;
    uint8_t addr[16];
    uint8_t prefix;
    int found;
} assigned_t;

/* Scan every ADDRESS_ASSIGN capsule received on r for an entry with `want`'s
 * req_id. */
static int
find_assigned(const cli_req_t *r, assigned_t *want)
{
    size_t off = 0;
    want->found = 0;
    while (off < r->body_len) {
        uint64_t type;
        const uint8_t *payload;
        size_t plen, consumed;
        if (xqc_h3_ext_capsule_decode(r->body + off, r->body_len - off, &type, &payload,
                                      &plen, &consumed) != XQC_OK)
            break;
        off += consumed;
        if (type != XQC_H3_CAPSULE_ADDRESS_ASSIGN) continue;
        size_t p = 0;
        while (p < plen) {
            uint64_t id;
            uint8_t ver, addr[16], prefix;
            size_t alen = sizeof(addr), used = 0;
            if (xqc_h3_ext_connectip_parse_address_assign(payload + p, plen - p, &id,
                                                          &ver, addr, &alen, &prefix,
                                                          &used) != XQC_OK ||
                used == 0)
                break;
            p += used;
            if (id == want->req_id) {
                want->ver = ver;
                memset(want->addr, 0, sizeof(want->addr));
                memcpy(want->addr, addr, alen);
                want->prefix = prefix;
                want->found = 1;
            }
        }
    }
    return want->found;
}

typedef struct {
    cli_req_t *r;
    uint64_t req_id;
} c_assigned_ctx_t;

static int
c_assigned(env_t *e, void *ctx)
{
    (void)e;
    c_assigned_ctx_t *a = ctx;
    assigned_t w = {.req_id = a->req_id};
    return find_assigned(a->r, &w);
}

/* One ADDRESS_REQUEST capsule carrying the given (req_id, version) entries. */
static int
send_address_request(cli_req_t *r, const uint64_t *ids, const uint8_t *vers, int n)
{
    uint8_t payload[128];
    size_t len = 0;
    for (int i = 0; i < n; i++) {
        uint8_t zero[16] = {0};
        size_t w = 0;
        if (xqc_h3_ext_connectip_build_address_request(payload + len,
                                                       sizeof(payload) - len, &w, ids[i],
                                                       vers[i], zero, 0) != XQC_OK)
            return -1;
        len += w;
    }
    uint8_t cap[160];
    size_t cw = 0;
    if (xqc_h3_ext_capsule_encode(cap, sizeof(cap), &cw, XQC_H3_CAPSULE_ADDRESS_REQUEST,
                                  payload, len) != XQC_OK)
        return -1;
    return xqc_h3_request_send_body(r->req, cap, cw, 0) == (ssize_t)cw ? 0 : -1;
}

/* ─── scenarios ─── */

static void
scenario_auth(void)
{
    printf("auth:\n");
    env_t e;
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_subnet(cfg, "10.0.0.0/24");
    mqvpn_config_add_user(cfg, "alice", "alicekey");
    if (env_start(&e, cfg) != 0) {
        CHECK(0, "server start");
        env_stop(&e);
        return;
    }

    cli_conn_t c1;
    cli_req_t r1;
    CHECK(cli_connect(&e, &c1) == 0, "handshake 1");
    CHECK(cli_connect_ip(&e, &c1, &r1, "bearer   alicekey") == 200,
          "lowercase 'bearer' scheme with extra spaces accepted (200)");

    /* Remove the only credential: the server must not become open. */
    CHECK(mqvpn_server_remove_user(e.svr, "alice") == MQVPN_OK, "remove_user(alice)");
    pump_until(&e, c_conn_closed, &c1);
    CHECK(c1.closed, "removed user's connection closed");

    cli_conn_t c2;
    cli_req_t r2;
    CHECK(cli_connect(&e, &c2) == 0, "handshake 2");
    CHECK(cli_connect_ip(&e, &c2, &r2, NULL) == 403,
          "no credential left: unauthenticated request refused (403)");
    CHECK(mqvpn_server_get_n_clients(e.svr) == 0, "no session admitted");

    /* Two more failures on the same connection: the third closes it. */
    cli_req_t r3, r4;
    CHECK(cli_connect_ip(&e, &c2, &r3, "Bearer wrong") == 403, "second failure 403");
    cli_connect_ip(&e, &c2, &r4, "Bearer wrong");
    pump_until(&e, c_conn_closed, &c2);
    CHECK(c2.closed, "connection closed after 3 auth failures");

    env_stop(&e);
}

static void
scenario_pool(void)
{
    printf("pool:\n");
    env_t e;
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_subnet(cfg, "10.0.0.0/30"); /* one client address (.2) */
    if (env_start(&e, cfg) != 0) {
        CHECK(0, "server start");
        env_stop(&e);
        return;
    }

    cli_conn_t c1, c2;
    cli_req_t r1, r2, r3;
    CHECK(cli_connect(&e, &c1) == 0, "handshake 1");
    CHECK(cli_connect_ip(&e, &c1, &r1, NULL) == 200, "first client 200 (no auth)");

    mqvpn_client_info_t ci[2];
    int n = 0;
    mqvpn_server_get_client_info(e.svr, ci, 2, &n);
    CHECK(n == 1 && ci[0].connected_at_us > 0, "connected_at_us set without auth");

    /* IPv6 request on a server without IPv6: ::/128 rejection, not the /32. */
    uint64_t id = 9;
    uint8_t ver = 6;
    CHECK(send_address_request(&r1, &id, &ver, 1) == 0, "send ADDRESS_REQUEST v6");
    c_assigned_ctx_t ac = {&r1, 9};
    pump_until(&e, c_assigned, &ac);
    assigned_t a = {.req_id = 9};
    static const uint8_t zero16[16] = {0};
    CHECK(find_assigned(&r1, &a) && a.ver == 6 && a.prefix == 128 &&
              memcmp(a.addr, zero16, 16) == 0,
          "v6 request without v6 pool -> ::/128 rejection");

    CHECK(cli_connect(&e, &c2) == 0, "handshake 2");
    CHECK(cli_connect_ip(&e, &c2, &r2, NULL) == 503, "pool exhausted -> 503");
    CHECK(!c2.closed, "connection kept open after 503");

    /* Free the address; the same connection can then establish. */
    xqc_h3_request_close(r1.req);
    int zero = 0;
    pump_until(&e, c_n_clients, &zero);
    CHECK(cli_connect_ip(&e, &c2, &r3, NULL) == 200, "retry on same connection 200");

    env_stop(&e);
}

static void
scenario_addrreq(void)
{
    printf("addrreq:\n");
    env_t e;
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_subnet(cfg, "10.0.0.0/24");
    mqvpn_config_set_subnet6(cfg, "fd00:abcd::/112");
    if (env_start(&e, cfg) != 0) {
        CHECK(0, "server start");
        env_stop(&e);
        return;
    }
    cli_conn_t c;
    cli_req_t r;
    CHECK(cli_connect(&e, &c) == 0, "handshake");
    CHECK(cli_connect_ip(&e, &c, &r, NULL) == 200 && g_last_has_v6, "200 with IPv6");

    uint64_t ids[2] = {7, 8};
    uint8_t vers[2] = {6, 4};
    CHECK(send_address_request(&r, ids, vers, 2) == 0, "send ADDRESS_REQUEST v6+v4");
    c_assigned_ctx_t ac = {&r, 8};
    pump_until(&e, c_assigned, &ac);
    assigned_t a6 = {.req_id = 7}, a4 = {.req_id = 8};
    CHECK(find_assigned(&r, &a6) && a6.ver == 6 && a6.prefix == 128 &&
              memcmp(a6.addr, g_last_ip6, 16) == 0,
          "v6 request answered with the client's /128");
    CHECK(find_assigned(&r, &a4) && a4.ver == 4 && a4.prefix == 32 &&
              memcmp(a4.addr, g_last_ip, 4) == 0,
          "v4 request answered with the client's /32");
    env_stop(&e);
}

static void
scenario_pinned(void)
{
    printf("pinned:\n");
    env_t e;
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_subnet(cfg, "10.0.0.0/24");
    mqvpn_config_add_user(cfg, "alice", "alicekey");
    mqvpn_config_set_user_fixed_ip(cfg, "alice", "10.0.0.9");
    mqvpn_config_set_max_clients(cfg, 1);
    if (env_start(&e, cfg) != 0) {
        CHECK(0, "server start");
        env_stop(&e);
        return;
    }
    cli_conn_t c1, c2;
    cli_req_t r1, r2;
    CHECK(cli_connect(&e, &c1) == 0 && cli_connect(&e, &c2) == 0, "two handshakes");
    CHECK(cli_connect_ip(&e, &c1, &r1, "Bearer alicekey") == 200, "first tunnel 200");
    CHECK(cli_connect_ip(&e, &c2, &r2, "Bearer alicekey") == 200,
          "pinned user reconnecting at max_clients=1 supersedes (200, not 403)");
    CHECK(mqvpn_server_get_n_clients(e.svr) == 1 && g_last_ip[3] == 9,
          "one session, on the pinned address");
    env_stop(&e);
}

/* Downlink packet to dst, 1000 bytes so each datagram fills a QUIC packet. */
static int
tun_send(env_t *e, const uint8_t dst[4])
{
    uint8_t pkt[1000];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x45;
    pkt[2] = sizeof(pkt) >> 8;
    pkt[3] = sizeof(pkt) & 0xFF;
    pkt[8] = 64;
    pkt[9] = 17;
    pkt[12] = 192;
    pkt[13] = 0;
    pkt[14] = 2;
    pkt[15] = 1;
    memcpy(pkt + 16, dst, 4);
    return mqvpn_server_on_tun_packet(e->svr, pkt, sizeof(pkt));
}

static uint64_t
dgram_sent(env_t *e)
{
    mqvpn_stats_t st;
    mqvpn_server_get_stats(e->svr, &st);
    return st.dgram_sent;
}

static int
tun_readable(env_t *e)
{
    mqvpn_interest_t in;
    mqvpn_server_get_interest(e->svr, &in);
    return in.tun_readable;
}

static void
scenario_backpressure(void)
{
    printf("backpressure:\n");
    env_t e;
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_subnet(cfg, "10.0.0.0/24");
    if (env_start(&e, cfg) != 0) {
        CHECK(0, "server start");
        env_stop(&e);
        return;
    }
    cli_conn_t ca, cb;
    cli_req_t ra, rb;
    uint8_t ip_a[4], ip_b[4];
    CHECK(cli_connect(&e, &ca) == 0 && cli_connect(&e, &cb) == 0, "two handshakes");
    CHECK(cli_connect_ip(&e, &ca, &ra, NULL) == 200, "tunnel A");
    memcpy(ip_a, g_last_ip, 4);
    CHECK(cli_connect_ip(&e, &cb, &rb, NULL) == 200, "tunnel B");
    memcpy(ip_b, g_last_ip, 4);
    /* Let the datagram MSS settle before flooding. */
    for (int i = 0; i < 20; i++)
        pump_once(&e);

    /* Flood A without pumping until its send queue is full (sends stop). */
    int again = 0, stalled = 0;
    uint64_t prev = dgram_sent(&e);
    for (int i = 0; i < 200000 && stalled < 200; i++) {
        if (tun_send(&e, ip_a) == MQVPN_ERR_AGAIN) {
            again = 1;
            break;
        }
        uint64_t now = dgram_sent(&e);
        stalled = (now == prev) ? stalled + 1 : 0;
        prev = now;
    }
    CHECK(stalled >= 200 || again, "A's datagram queue fills");
    CHECK(!again && tun_readable(&e) == 1,
          "A blocked alone: no TUN pause (others keep flowing)");

    uint64_t before = dgram_sent(&e);
    CHECK(tun_send(&e, ip_b) == MQVPN_OK && dgram_sent(&e) == before + 1,
          "B's downlink still forwarded while A is blocked");

    /* Block B as well: now every session is blocked, TUN reading pauses. */
    again = 0;
    for (int i = 0; i < 200000 && !again; i++)
        again = (tun_send(&e, ip_b) == MQVPN_ERR_AGAIN);
    CHECK(again && tun_readable(&e) == 0, "all sessions blocked -> TUN paused");

    env_stop(&e);
}

int
main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("test_server_hardening\n");
    scenario_auth();
    scenario_pool();
    scenario_addrreq();
    scenario_pinned();
    scenario_backpressure();
    if (g_failed)
        printf("test_server_hardening: %d FAILED\n", g_failed);
    else
        printf("test_server_hardening: PASS\n");
    return g_failed ? 1 : 0;
}
