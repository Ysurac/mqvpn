/*
 * test_server.c — libmqvpn server API lifecycle tests (M1-5)
 *
 * Tests per impl_plan:
 *   test_server_lifecycle:
 *     - server_new(config, callbacks) → handle
 *     - server_start() → MQVPN_OK
 *     - server_tick() → MQVPN_OK
 *     - server_get_interest() → valid values
 *     - server_destroy() → valgrind leak-free
 *
 *   test_server_session:
 *     - on_socket_recv() でクライアント接続
 *     - tunnel_config_ready callback 発火
 *     - set_tun_active → tun_output でパケット出力
 *     - client 切断 → セッション解放
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <poll.h>

#include "libmqvpn.h"
#include "mqvpn_internal.h"
#include <xquic/xquic.h>
#include <time.h>

/* ── Test infrastructure ── */

static int g_tests_run = 0;
static int g_tests_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void)   \
    {                              \
        g_tests_run++;             \
        printf("  %-50s ", #name); \
        test_##name();             \
        g_tests_passed++;          \
        printf("PASS\n");          \
    }                              \
    static void test_##name(void)

#define ASSERT_EQ(a, b)                                                                \
    do {                                                                               \
        if ((a) != (b)) {                                                              \
            printf("FAIL\n    %s:%d: %s == %lld, expected %lld\n", __FILE__, __LINE__, \
                   #a, (long long)(a), (long long)(b));                                \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define ASSERT_NE(a, b)                                                                \
    do {                                                                               \
        if ((a) == (b)) {                                                              \
            printf("FAIL\n    %s:%d: %s == %s (unexpected)\n", __FILE__, __LINE__, #a, \
                   #b);                                                                \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define ASSERT_NULL(a)                                                           \
    do {                                                                         \
        if ((a) != NULL) {                                                       \
            printf("FAIL\n    %s:%d: %s is not NULL\n", __FILE__, __LINE__, #a); \
            exit(1);                                                             \
        }                                                                        \
    } while (0)

#define ASSERT_NOT_NULL(a)                                                   \
    do {                                                                     \
        if ((a) == NULL) {                                                   \
            printf("FAIL\n    %s:%d: %s is NULL\n", __FILE__, __LINE__, #a); \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

/* ── Mock callback state ── */

static int g_tun_output_called = 0;
static int g_tunnel_config_ready_called = 0;
static mqvpn_tunnel_info_t g_last_tunnel_info;
static int g_log_called = 0;

static void
mock_tun_output(const uint8_t *pkt, size_t len, void *user_ctx)
{
    (void)pkt;
    (void)len;
    (void)user_ctx;
    g_tun_output_called++;
}

static void
mock_tunnel_config_ready(const mqvpn_tunnel_info_t *info, void *user_ctx)
{
    (void)user_ctx;
    g_tunnel_config_ready_called++;
    if (info) memcpy(&g_last_tunnel_info, info, sizeof(g_last_tunnel_info));
}

static void
mock_log(mqvpn_log_level_t level, const char *msg, void *user_ctx)
{
    (void)level;
    (void)msg;
    (void)user_ctx;
    g_log_called++;
}

static void
reset_mocks(void)
{
    g_tun_output_called = 0;
    g_tunnel_config_ready_called = 0;
    memset(&g_last_tunnel_info, 0, sizeof(g_last_tunnel_info));
    g_log_called = 0;
}

/* ── Helper: create a valid server config ── */

static mqvpn_config_t *
make_server_config(void)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    if (!cfg) return NULL;
    mqvpn_config_set_listen(cfg, "0.0.0.0", 443);
    mqvpn_config_set_subnet(cfg, "10.0.0.0/24");
    mqvpn_config_set_tls_cert(cfg, TEST_CERT_FILE, TEST_KEY_FILE);
    mqvpn_config_set_log_level(cfg, MQVPN_LOG_ERROR);
    return cfg;
}

/* ── server_new tests ── */

TEST(server_new_null_config)
{
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    mqvpn_server_t *s = mqvpn_server_new(NULL, &cbs, NULL);
    ASSERT_NULL(s);
}

TEST(server_new_null_callbacks)
{
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_t *s = mqvpn_server_new(cfg, NULL, NULL);
    ASSERT_NULL(s);
    mqvpn_config_free(cfg);
}

TEST(server_new_bad_abi)
{
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.abi_version = 999;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NULL(s);
    mqvpn_config_free(cfg);
}

TEST(server_new_missing_tun_output)
{
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    /* tun_output = NULL */
    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NULL(s);
    mqvpn_config_free(cfg);
}

TEST(server_new_missing_tunnel_config_ready)
{
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    /* tunnel_config_ready = NULL */
    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NULL(s);
    mqvpn_config_free(cfg);
}

TEST(server_new_destroy)
{
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    cbs.log = mock_log;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NOT_NULL(s);
    mqvpn_config_free(cfg);

    mqvpn_server_destroy(s);
}

TEST(server_destroy_null)
{
    /* Must not crash */
    mqvpn_server_destroy(NULL);
}

/* ── Lifecycle tests ── */

TEST(server_lifecycle)
{
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    cbs.log = mock_log;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NOT_NULL(s);
    mqvpn_config_free(cfg);

    /* start() should trigger tunnel_config_ready */
    ASSERT_EQ(g_tunnel_config_ready_called, 0);
    ASSERT_EQ(mqvpn_server_start(s), MQVPN_OK);
    ASSERT_EQ(g_tunnel_config_ready_called, 1);

    /* Verify tunnel info: server gets .1 address in 10.0.0.0/24 */
    ASSERT_EQ(g_last_tunnel_info.assigned_ip[0], 10);
    ASSERT_EQ(g_last_tunnel_info.assigned_ip[1], 0);
    ASSERT_EQ(g_last_tunnel_info.assigned_ip[2], 0);
    ASSERT_EQ(g_last_tunnel_info.assigned_ip[3], 1);
    ASSERT_EQ(g_last_tunnel_info.mtu, 1280);

    /* tick() should succeed */
    ASSERT_EQ(mqvpn_server_tick(s), MQVPN_OK);

    /* get_interest() should return valid values */
    mqvpn_interest_t interest;
    ASSERT_EQ(mqvpn_server_get_interest(s, &interest), MQVPN_OK);
    ASSERT_NE(interest.next_timer_ms, 0);
    ASSERT_EQ(interest.tun_readable, 1);

    /* get_stats() should work */
    mqvpn_stats_t stats;
    ASSERT_EQ(mqvpn_server_get_stats(s, &stats), MQVPN_OK);
    ASSERT_EQ(stats.bytes_tx, 0);
    ASSERT_EQ(stats.bytes_rx, 0);

    /* stop and destroy */
    ASSERT_EQ(mqvpn_server_stop(s), MQVPN_OK);
    mqvpn_server_destroy(s);
}

TEST(server_lifecycle_with_v6)
{
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_config_set_subnet6(cfg, "fd00::/112");

    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    cbs.log = mock_log;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NOT_NULL(s);
    mqvpn_config_free(cfg);

    ASSERT_EQ(mqvpn_server_start(s), MQVPN_OK);
    ASSERT_EQ(g_tunnel_config_ready_called, 1);
    ASSERT_EQ(g_last_tunnel_info.has_v6, 1);

    mqvpn_server_destroy(s);
}

TEST(server_double_start)
{
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);

    ASSERT_EQ(mqvpn_server_start(s), MQVPN_OK);
    /* Second start should fail */
    ASSERT_EQ(mqvpn_server_start(s), MQVPN_ERR_INVALID_ARG);

    mqvpn_server_destroy(s);
}

/* ── set_socket_fd tests ── */

TEST(server_set_socket_fd)
{
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);

    struct sockaddr_in laddr = {.sin_family = AF_INET};
    ASSERT_EQ(mqvpn_server_set_socket_fd(s, 42, (struct sockaddr *)&laddr, sizeof(laddr)),
              MQVPN_OK);
    ASSERT_EQ(mqvpn_server_set_socket_fd(s, -1, NULL, 0), MQVPN_ERR_INVALID_ARG);
    ASSERT_EQ(mqvpn_server_set_socket_fd(NULL, 42, NULL, 0), MQVPN_ERR_INVALID_ARG);

    mqvpn_server_destroy(s);
}

/* ── Query function null-safety tests ── */

TEST(server_get_stats_null)
{
    mqvpn_stats_t stats;
    ASSERT_EQ(mqvpn_server_get_stats(NULL, &stats), MQVPN_ERR_INVALID_ARG);
}

TEST(server_get_interest_null)
{
    mqvpn_interest_t interest;
    ASSERT_EQ(mqvpn_server_get_interest(NULL, &interest), MQVPN_ERR_INVALID_ARG);
}

TEST(server_tick_null)
{
    ASSERT_EQ(mqvpn_server_tick(NULL), MQVPN_ERR_INVALID_ARG);
}

TEST(server_on_tun_packet_null)
{
    uint8_t pkt[20] = {0x45};
    ASSERT_EQ(mqvpn_server_on_tun_packet(NULL, pkt, 20), MQVPN_ERR_INVALID_ARG);
}

TEST(server_on_socket_recv_null)
{
    uint8_t pkt[20];
    struct sockaddr_in addr = {.sin_family = AF_INET};
    ASSERT_EQ(mqvpn_server_on_socket_recv(NULL, pkt, 20, (struct sockaddr *)&addr,
                                          sizeof(addr)),
              MQVPN_ERR_INVALID_ARG);
}

/* ── on_tun_packet with no sessions ── */

TEST(server_on_tun_packet_no_sessions)
{
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    mqvpn_server_start(s);

    /* With no sessions, on_tun_packet should return OK (early return, no ICMP) */
    uint8_t pkt[40];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0x45; /* IPv4 */
    ASSERT_EQ(mqvpn_server_on_tun_packet(s, pkt, 40), MQVPN_OK);

    mqvpn_server_destroy(s);
}

/* ── test_server_session: session lifecycle callbacks ── */

static int g_client_connected_called = 0;
static uint32_t g_last_session_id = 0;
static int g_client_disconnected_called = 0;
static uint32_t g_last_disconnected_session_id = 0;

static void
mock_on_client_connected(const mqvpn_tunnel_info_t *info, uint32_t session_id,
                         void *user_ctx)
{
    (void)user_ctx;
    g_client_connected_called++;
    g_last_session_id = session_id;
    if (info) {
        memcpy(&g_last_tunnel_info, info, sizeof(g_last_tunnel_info));
    }
}

static void
mock_on_client_disconnected(uint32_t session_id, mqvpn_error_t reason, void *user_ctx)
{
    (void)reason;
    (void)user_ctx;
    g_client_disconnected_called++;
    g_last_disconnected_session_id = session_id;
}

/* ── Client mock callbacks for loopback test ── */

static int g_cli_tun_output_called = 0;
static int g_cli_tunnel_ready_called = 0;
static mqvpn_tunnel_info_t g_cli_tunnel_info;

static void
mock_cli_tun_output(const uint8_t *pkt, size_t len, void *user_ctx)
{
    (void)pkt;
    (void)len;
    (void)user_ctx;
    g_cli_tun_output_called++;
}

static void
mock_cli_tunnel_ready(const mqvpn_tunnel_info_t *info, void *user_ctx)
{
    (void)user_ctx;
    g_cli_tunnel_ready_called++;
    if (info) memcpy(&g_cli_tunnel_info, info, sizeof(g_cli_tunnel_info));
}

/* ── Packet relay helper: drain sockets and tick both engines ── */

static void
drain_and_tick(mqvpn_server_t *svr, int svr_fd, mqvpn_client_t *cli, int cli_fd,
               mqvpn_path_handle_t path_h)
{
    uint8_t buf[65536];
    struct sockaddr_storage from;
    socklen_t from_len;

    /* Drain server socket (packets from client) */
    for (;;) {
        from_len = sizeof(from);
        // codeql[cpp/uncontrolled-allocation-size] buf bounded by sizeof(buf); xquic
        // validates internally
        ssize_t n = recvfrom(svr_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        mqvpn_server_on_socket_recv(svr, buf, (size_t)n, (struct sockaddr *)&from,
                                    from_len);
    }

    /* Drain client socket (packets from server) */
    for (;;) {
        from_len = sizeof(from);
        // codeql[cpp/uncontrolled-allocation-size] buf bounded by sizeof(buf); xquic
        // validates internally
        ssize_t n = recvfrom(cli_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        mqvpn_client_on_socket_recv(cli, path_h, buf, (size_t)n, (struct sockaddr *)&from,
                                    from_len);
    }

    mqvpn_server_tick(svr);
    mqvpn_client_tick(cli);
}

static void
drain_and_tick_two_paths(mqvpn_server_t *svr, int svr_fd, mqvpn_client_t *cli,
                         int cli_fd0, mqvpn_path_handle_t path_h0, int cli_fd1,
                         mqvpn_path_handle_t path_h1)
{
    uint8_t buf[65536];
    struct sockaddr_storage from;
    socklen_t from_len;

    for (;;) {
        from_len = sizeof(from);
        ssize_t n = recvfrom(svr_fd, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        mqvpn_server_on_socket_recv(svr, buf, (size_t)n, (struct sockaddr *)&from,
                                    from_len);
    }

    for (;;) {
        from_len = sizeof(from);
        ssize_t n = recvfrom(cli_fd0, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        mqvpn_client_on_socket_recv(cli, path_h0, buf, (size_t)n,
                                    (struct sockaddr *)&from, from_len);
    }

    for (;;) {
        from_len = sizeof(from);
        ssize_t n = recvfrom(cli_fd1, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &from_len);
        if (n <= 0) break;
        mqvpn_client_on_socket_recv(cli, path_h1, buf, (size_t)n,
                                    (struct sockaddr *)&from, from_len);
    }

    mqvpn_server_tick(svr);
    mqvpn_client_tick(cli);
}

static mqvpn_path_status_t
get_path_status_or_invalid(mqvpn_client_t *cli, mqvpn_path_handle_t h)
{
    mqvpn_path_info_t infos[MQVPN_MAX_PATHS];
    int n = 0;
    if (mqvpn_client_get_paths(cli, infos, MQVPN_MAX_PATHS, &n) != MQVPN_OK)
        return (mqvpn_path_status_t)-1;
    for (int i = 0; i < n; i++)
        if (infos[i].handle == h) return infos[i].status;
    return (mqvpn_path_status_t)-1;
}

/* Note: all pump loops below use poll() instead of usleep() for CI robustness.
 * This avoids timing issues on slow CI runners where QUIC PTO (1s+) can expire. */

/* ── test_server_session tests ── */

TEST(server_session_callbacks_registered)
{
    /* Verify that on_client_connected/disconnected callbacks are accepted */
    reset_mocks();
    g_client_connected_called = 0;
    g_client_disconnected_called = 0;

    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    cbs.log = mock_log;
    cbs.on_client_connected = mock_on_client_connected;
    cbs.on_client_disconnected = mock_on_client_disconnected;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NOT_NULL(s);
    mqvpn_config_free(cfg);

    ASSERT_EQ(mqvpn_server_start(s), MQVPN_OK);

    /* No clients connected yet */
    ASSERT_EQ(g_client_connected_called, 0);
    ASSERT_EQ(g_client_disconnected_called, 0);

    /* Stats should show zero */
    mqvpn_stats_t stats;
    ASSERT_EQ(mqvpn_server_get_stats(s, &stats), MQVPN_OK);
    ASSERT_EQ(stats.bytes_tx, 0);
    ASSERT_EQ(stats.bytes_rx, 0);

    mqvpn_server_destroy(s);
}

TEST(server_session_set_socket_with_addr)
{
    /* Verify set_socket_fd stores local address */
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);

    struct sockaddr_in laddr;
    memset(&laddr, 0, sizeof(laddr));
    laddr.sin_family = AF_INET;
    laddr.sin_port = htons(443);
    laddr.sin_addr.s_addr = htonl(INADDR_ANY);

    ASSERT_EQ(mqvpn_server_set_socket_fd(s, 42, (struct sockaddr *)&laddr, sizeof(laddr)),
              MQVPN_OK);

    /* Verify NULL local_addr is also accepted */
    ASSERT_EQ(mqvpn_server_set_socket_fd(s, 43, NULL, 0), MQVPN_OK);

    mqvpn_server_destroy(s);
}

TEST(server_session_on_tun_v6_no_sessions)
{
    /* IPv6 packet with no sessions → early return, no ICMP */
    reset_mocks();
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_config_set_subnet6(cfg, "fd00::/112");

    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    mqvpn_server_start(s);

    int baseline = g_tun_output_called;

    /* IPv6 packet to unknown dest within pool */
    uint8_t pkt6[60];
    memset(pkt6, 0, sizeof(pkt6));
    pkt6[0] = 0x60; /* IPv6 */
    pkt6[4] = 0;
    pkt6[5] = 20; /* payload length */
    pkt6[6] = 59; /* next header: no next */
    pkt6[7] = 64; /* hop limit */
    /* src: fd00::100 */
    pkt6[8] = 0xfd;
    pkt6[23] = 0x01;
    /* dst: fd00::50 (no session) */
    pkt6[24] = 0xfd;
    pkt6[39] = 0x32;

    /* n_sessions == 0 → early return, no ICMP generated */
    ASSERT_EQ(mqvpn_server_on_tun_packet(s, pkt6, 60), MQVPN_OK);
    ASSERT_EQ(g_tun_output_called, baseline);

    mqvpn_server_destroy(s);
}

/* ── test_server_session: QUIC loopback integration test ──
 *
 * Per impl_plan M1-5:
 *   - on_socket_recv() でクライアント接続
 *   - tunnel_config_ready callback 発火
 *   - set_tun_active → tun_output でパケット出力
 *   - client 切断 → セッション解放
 */
TEST(server_session_quic_loopback)
{
    /* Reset all mocks */
    reset_mocks();
    g_client_connected_called = 0;
    g_client_disconnected_called = 0;
    g_cli_tun_output_called = 0;
    g_cli_tunnel_ready_called = 0;
    memset(&g_cli_tunnel_info, 0, sizeof(g_cli_tunnel_info));

    /* ── Create UDP sockets ── */
    int svr_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(svr_fd, -1);
    int cli_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(cli_fd, -1);

    struct sockaddr_in svr_addr, cli_addr;
    memset(&svr_addr, 0, sizeof(svr_addr));
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    svr_addr.sin_port = htons(0); /* OS picks port */
    /* Do NOT use assert() for calls with side effects — NDEBUG removes them */
    ASSERT_EQ(bind(svr_fd, (struct sockaddr *)&svr_addr, sizeof(svr_addr)), 0);

    memset(&cli_addr, 0, sizeof(cli_addr));
    cli_addr.sin_family = AF_INET;
    cli_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cli_addr.sin_port = htons(0);
    ASSERT_EQ(bind(cli_fd, (struct sockaddr *)&cli_addr, sizeof(cli_addr)), 0);

    /* Get actual bound addresses */
    socklen_t alen = sizeof(svr_addr);
    getsockname(svr_fd, (struct sockaddr *)&svr_addr, &alen);
    alen = sizeof(cli_addr);
    getsockname(cli_fd, (struct sockaddr *)&cli_addr, &alen);

    /* ── Server setup ── */
    mqvpn_config_t *svr_cfg = make_server_config();
    mqvpn_server_callbacks_t svr_cbs = MQVPN_SERVER_CALLBACKS_INIT;
    svr_cbs.tun_output = mock_tun_output;
    svr_cbs.tunnel_config_ready = mock_tunnel_config_ready;
    svr_cbs.on_client_connected = mock_on_client_connected;
    svr_cbs.on_client_disconnected = mock_on_client_disconnected;

    mqvpn_server_t *svr = mqvpn_server_new(svr_cfg, &svr_cbs, NULL);
    ASSERT_NOT_NULL(svr);
    mqvpn_config_free(svr_cfg);

    ASSERT_EQ(mqvpn_server_set_socket_fd(svr, svr_fd, (struct sockaddr *)&svr_addr,
                                         sizeof(svr_addr)),
              MQVPN_OK);
    ASSERT_EQ(mqvpn_server_start(svr), MQVPN_OK);

    /* ── Client setup ── */
    mqvpn_config_t *cli_cfg = mqvpn_config_new();
    mqvpn_config_set_server(cli_cfg, "127.0.0.1", ntohs(svr_addr.sin_port));
    mqvpn_config_set_insecure(cli_cfg, 1);
    mqvpn_config_set_log_level(cli_cfg, MQVPN_LOG_ERROR);

    mqvpn_client_callbacks_t cli_cbs = MQVPN_CLIENT_CALLBACKS_INIT;
    cli_cbs.tun_output = mock_cli_tun_output;
    cli_cbs.tunnel_config_ready = mock_cli_tunnel_ready;
    /* send_packet = NULL → fd-only mode */

    mqvpn_client_t *cli = mqvpn_client_new(cli_cfg, &cli_cbs, NULL);
    ASSERT_NOT_NULL(cli);
    mqvpn_config_free(cli_cfg);

    /* Add path with client socket */
    mqvpn_path_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    memcpy(desc.local_addr, &cli_addr, sizeof(cli_addr));
    desc.local_addr_len = sizeof(cli_addr);

    mqvpn_path_handle_t path_h = mqvpn_client_add_path_fd(cli, cli_fd, &desc);
    ASSERT_NE(path_h, (mqvpn_path_handle_t)-1);

    /* Set server address and connect */
    mqvpn_client_set_server_addr(cli, (struct sockaddr *)&svr_addr, sizeof(svr_addr));
    ASSERT_EQ(mqvpn_client_connect(cli), MQVPN_OK);

    /* ── Phase 1: QUIC handshake + MASQUE tunnel setup ── */
    /* Use poll-based pump with 10s timeout for slow CI runners.
     * QUIC retransmission PTO can be 1s+, so 500ms was too tight. */
    for (int elapsed = 0; elapsed < 10000; elapsed++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd, path_h);
        if (g_client_connected_called > 0 && g_cli_tunnel_ready_called > 0) break;

        mqvpn_interest_t svr_int = {0}, cli_int = {0};
        mqvpn_server_get_interest(svr, &svr_int);
        mqvpn_client_get_interest(cli, &cli_int);
        int wait_ms = 50;
        if (svr_int.next_timer_ms > 0 && svr_int.next_timer_ms < wait_ms)
            wait_ms = svr_int.next_timer_ms;
        if (cli_int.next_timer_ms > 0 && cli_int.next_timer_ms < wait_ms)
            wait_ms = cli_int.next_timer_ms;
        if (wait_ms < 1) wait_ms = 1;

        struct pollfd pfds[2] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd, .events = POLLIN},
        };
        poll(pfds, 2, wait_ms);
        elapsed += wait_ms;
    }

    /* Verify: on_socket_recv() でクライアント接続 */
    ASSERT_EQ(g_client_connected_called, 1);
    /* Verify: tunnel_config_ready callback 発火 */
    ASSERT_EQ(g_cli_tunnel_ready_called, 1);
    /* Client assigned IP should be 10.0.0.2 (first allocation in /24) */
    ASSERT_EQ(g_cli_tunnel_info.assigned_ip[0], 10);
    ASSERT_EQ(g_cli_tunnel_info.assigned_ip[1], 0);
    ASSERT_EQ(g_cli_tunnel_info.assigned_ip[2], 0);
    ASSERT_EQ(g_cli_tunnel_info.assigned_ip[3], 2);

    /* Activate TUN → ESTABLISHED */
    mqvpn_client_set_tun_active(cli, 1, -1);
    ASSERT_EQ(mqvpn_client_get_state(cli), MQVPN_STATE_ESTABLISHED);

    /* ── Phase 2: set_tun_active → tun_output でパケット出力 ── */
    /* Build IPv4 packet destined for client's assigned IP */
    uint8_t tun_pkt[40];
    memset(tun_pkt, 0, sizeof(tun_pkt));
    tun_pkt[0] = 0x45; /* IPv4, IHL=5 */
    tun_pkt[2] = 0;
    tun_pkt[3] = 40; /* total length = 40 */
    tun_pkt[8] = 64; /* TTL */
    tun_pkt[9] = 17; /* UDP */
    /* Source: 8.8.8.8 */
    tun_pkt[12] = 8;
    tun_pkt[13] = 8;
    tun_pkt[14] = 8;
    tun_pkt[15] = 8;
    /* Destination: client's assigned IP */
    memcpy(tun_pkt + 16, g_cli_tunnel_info.assigned_ip, 4);

    int baseline = g_cli_tun_output_called;
    ASSERT_EQ(mqvpn_server_on_tun_packet(svr, tun_pkt, sizeof(tun_pkt)), MQVPN_OK);

    /* Pump to deliver the MASQUE DATAGRAM */
    for (int i = 0; i < 5000; i++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd, path_h);
        if (g_cli_tun_output_called > baseline) break;
        struct pollfd pfds[2] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd, .events = POLLIN},
        };
        int w = poll(pfds, 2, 5);
        i += (w == 0) ? 5 : 1;
    }
    ASSERT_EQ(g_cli_tun_output_called, baseline + 1);

    /* ── Phase 2b: DL TTL=1 → dropped, ICMP Time Exceeded via tun_output ── */
    uint8_t ttl1_pkt[40];
    memset(ttl1_pkt, 0, sizeof(ttl1_pkt));
    ttl1_pkt[0] = 0x45;
    ttl1_pkt[2] = 0;
    ttl1_pkt[3] = 40;
    ttl1_pkt[8] = 1; /* TTL = 1 → expires */
    ttl1_pkt[9] = 17;
    ttl1_pkt[12] = 8;
    ttl1_pkt[13] = 8;
    ttl1_pkt[14] = 8;
    ttl1_pkt[15] = 8;
    memcpy(ttl1_pkt + 16, g_cli_tunnel_info.assigned_ip, 4);

    int tun_baseline = g_tun_output_called;
    int cli_baseline = g_cli_tun_output_called;
    ASSERT_EQ(mqvpn_server_on_tun_packet(svr, ttl1_pkt, sizeof(ttl1_pkt)), MQVPN_OK);
    /* ICMP Time Exceeded should be sent via tun_output (not to client) */
    ASSERT_EQ(g_tun_output_called, tun_baseline + 1);
    /* Client should NOT receive the expired packet */
    for (int i = 0; i < 30; i++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd, path_h);
        struct pollfd pfds[2] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd, .events = POLLIN},
        };
        poll(pfds, 2, 2);
    }
    ASSERT_EQ(g_cli_tun_output_called, cli_baseline);

    /* ── Phase 3: client 切断 → セッション解放 ── */
    mqvpn_client_disconnect(cli);

    /* Pump to deliver CONNECTION_CLOSE to server */
    for (int i = 0; i < 5000; i++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd, path_h);
        if (g_client_disconnected_called > 0) break;
        struct pollfd pfds[2] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd, .events = POLLIN},
        };
        int w = poll(pfds, 2, 5);
        i += (w == 0) ? 5 : 1;
    }
    ASSERT_EQ(g_client_disconnected_called, 1);
    ASSERT_EQ(g_last_disconnected_session_id, g_last_session_id);

    /* ── Cleanup ── */
    mqvpn_client_destroy(cli);
    mqvpn_server_destroy(svr);
    close(svr_fd);
    close(cli_fd);
}

/*
 * Regression for issue #4273:
 * runtime-added secondary path must not remain PENDING after tunnel is up.
 */
TEST(server_runtime_added_path_not_stuck_pending)
{
    reset_mocks();
    g_client_connected_called = 0;
    g_cli_tunnel_ready_called = 0;

    int svr_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(svr_fd, -1);
    int cli_fd0 = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(cli_fd0, -1);
    int cli_fd1 = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(cli_fd1, -1);

    struct sockaddr_in svr_addr, cli_addr0, cli_addr1;
    memset(&svr_addr, 0, sizeof(svr_addr));
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    svr_addr.sin_port = htons(0);
    ASSERT_EQ(bind(svr_fd, (struct sockaddr *)&svr_addr, sizeof(svr_addr)), 0);

    memset(&cli_addr0, 0, sizeof(cli_addr0));
    cli_addr0.sin_family = AF_INET;
    cli_addr0.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cli_addr0.sin_port = htons(0);
    ASSERT_EQ(bind(cli_fd0, (struct sockaddr *)&cli_addr0, sizeof(cli_addr0)), 0);

    memset(&cli_addr1, 0, sizeof(cli_addr1));
    cli_addr1.sin_family = AF_INET;
    cli_addr1.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cli_addr1.sin_port = htons(0);
    ASSERT_EQ(bind(cli_fd1, (struct sockaddr *)&cli_addr1, sizeof(cli_addr1)), 0);

    socklen_t alen = sizeof(svr_addr);
    getsockname(svr_fd, (struct sockaddr *)&svr_addr, &alen);
    alen = sizeof(cli_addr0);
    getsockname(cli_fd0, (struct sockaddr *)&cli_addr0, &alen);
    alen = sizeof(cli_addr1);
    getsockname(cli_fd1, (struct sockaddr *)&cli_addr1, &alen);

    mqvpn_config_t *svr_cfg = make_server_config();
    mqvpn_server_callbacks_t svr_cbs = MQVPN_SERVER_CALLBACKS_INIT;
    svr_cbs.tun_output = mock_tun_output;
    svr_cbs.tunnel_config_ready = mock_tunnel_config_ready;
    svr_cbs.on_client_connected = mock_on_client_connected;
    mqvpn_server_t *svr = mqvpn_server_new(svr_cfg, &svr_cbs, NULL);
    ASSERT_NOT_NULL(svr);
    mqvpn_config_free(svr_cfg);

    ASSERT_EQ(mqvpn_server_set_socket_fd(svr, svr_fd, (struct sockaddr *)&svr_addr,
                                         sizeof(svr_addr)),
              MQVPN_OK);
    ASSERT_EQ(mqvpn_server_start(svr), MQVPN_OK);

    mqvpn_config_t *cli_cfg = mqvpn_config_new();
    mqvpn_config_set_server(cli_cfg, "127.0.0.1", ntohs(svr_addr.sin_port));
    mqvpn_config_set_insecure(cli_cfg, 1);
    mqvpn_config_set_multipath(cli_cfg, 1);
    mqvpn_config_set_log_level(cli_cfg, MQVPN_LOG_ERROR);

    mqvpn_client_callbacks_t cli_cbs = MQVPN_CLIENT_CALLBACKS_INIT;
    cli_cbs.tun_output = mock_cli_tun_output;
    cli_cbs.tunnel_config_ready = mock_cli_tunnel_ready;
    mqvpn_client_t *cli = mqvpn_client_new(cli_cfg, &cli_cbs, NULL);
    ASSERT_NOT_NULL(cli);
    mqvpn_config_free(cli_cfg);

    mqvpn_path_desc_t d0;
    memset(&d0, 0, sizeof(d0));
    d0.struct_size = sizeof(d0);
    memcpy(d0.local_addr, &cli_addr0, sizeof(cli_addr0));
    d0.local_addr_len = sizeof(cli_addr0);
    mqvpn_path_handle_t h0 = mqvpn_client_add_path_fd(cli, cli_fd0, &d0);
    ASSERT_NE(h0, (mqvpn_path_handle_t)-1);

    mqvpn_client_set_server_addr(cli, (struct sockaddr *)&svr_addr, sizeof(svr_addr));
    ASSERT_EQ(mqvpn_client_connect(cli), MQVPN_OK);

    for (int elapsed = 0; elapsed < 10000; elapsed++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd0, h0);
        if (g_client_connected_called > 0 && g_cli_tunnel_ready_called > 0) break;

        struct pollfd pfds[2] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd0, .events = POLLIN},
        };
        int w = poll(pfds, 2, 10);
        elapsed += (w == 0) ? 10 : 1;
    }
    ASSERT_EQ(g_client_connected_called, 1);
    ASSERT_EQ(g_cli_tunnel_ready_called, 1);
    mqvpn_client_set_tun_active(cli, 1, -1);
    ASSERT_EQ(mqvpn_client_get_state(cli), MQVPN_STATE_ESTABLISHED);

    mqvpn_path_desc_t d1;
    memset(&d1, 0, sizeof(d1));
    d1.struct_size = sizeof(d1);
    memcpy(d1.local_addr, &cli_addr1, sizeof(cli_addr1));
    d1.local_addr_len = sizeof(cli_addr1);
    snprintf(d1.iface, sizeof(d1.iface), "eth1");
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(cli, cli_fd1, &d1);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);

    mqvpn_path_status_t st = MQVPN_PATH_PENDING;
    for (int elapsed = 0; elapsed < 12000; elapsed++) {
        drain_and_tick_two_paths(svr, svr_fd, cli, cli_fd0, h0, cli_fd1, h1);
        st = get_path_status_or_invalid(cli, h1);
        if (st != MQVPN_PATH_PENDING) break;

        struct pollfd pfds[3] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd0, .events = POLLIN},
            {.fd = cli_fd1, .events = POLLIN},
        };
        int w = poll(pfds, 3, 10);
        elapsed += (w == 0) ? 10 : 1;
    }
    if (st != MQVPN_PATH_ACTIVE && st != MQVPN_PATH_DEGRADED) {
        printf("FAIL\n    %s:%d: runtime-added path status=%d, expected ACTIVE(%d) "
               "or DEGRADED(%d)\n",
               __FILE__, __LINE__, (int)st, (int)MQVPN_PATH_ACTIVE,
               (int)MQVPN_PATH_DEGRADED);
        exit(1);
    }

    mqvpn_client_disconnect(cli);
    mqvpn_client_destroy(cli);
    mqvpn_server_destroy(svr);
    close(svr_fd);
    close(cli_fd0);
    close(cli_fd1);
}

/* ── test_server_get_status: control API status queries ──
 *
 * Verify mqvpn_server_get_client_info() (used by control socket get_status command)
 * returns correct data in both scenarios: no clients and with connected client.
 */

TEST(server_get_status_no_clients)
{
    reset_mocks();

    mqvpn_config_t *cfg = make_server_config();
    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;

    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    ASSERT_NOT_NULL(s);
    mqvpn_config_free(cfg);

    ASSERT_EQ(mqvpn_server_start(s), MQVPN_OK);

    /* Query status with no clients */
    mqvpn_client_info_t clients[MQVPN_MAX_USERS];
    int n_clients = 0;
    ASSERT_EQ(mqvpn_server_get_client_info(s, clients, MQVPN_MAX_USERS, &n_clients),
              MQVPN_OK);

    /* Verify: n_clients should be 0 */
    ASSERT_EQ(n_clients, 0);

    mqvpn_server_destroy(s);
}

TEST(server_get_status_with_client)
{
    reset_mocks();
    g_client_connected_called = 0;
    g_cli_tunnel_ready_called = 0;

    /* Create and set up sockets */
    int svr_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(svr_fd, -1);
    int cli_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(cli_fd, -1);

    struct sockaddr_in svr_addr, cli_addr;
    memset(&svr_addr, 0, sizeof(svr_addr));
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    svr_addr.sin_port = htons(0);
    ASSERT_EQ(bind(svr_fd, (struct sockaddr *)&svr_addr, sizeof(svr_addr)), 0);

    memset(&cli_addr, 0, sizeof(cli_addr));
    cli_addr.sin_family = AF_INET;
    cli_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    cli_addr.sin_port = htons(0);
    ASSERT_EQ(bind(cli_fd, (struct sockaddr *)&cli_addr, sizeof(cli_addr)), 0);

    socklen_t alen = sizeof(svr_addr);
    getsockname(svr_fd, (struct sockaddr *)&svr_addr, &alen);
    alen = sizeof(cli_addr);
    getsockname(cli_fd, (struct sockaddr *)&cli_addr, &alen);

    /* Server setup */
    mqvpn_config_t *svr_cfg = make_server_config();
    mqvpn_server_callbacks_t svr_cbs = MQVPN_SERVER_CALLBACKS_INIT;
    svr_cbs.tun_output = mock_tun_output;
    svr_cbs.tunnel_config_ready = mock_tunnel_config_ready;
    svr_cbs.on_client_connected = mock_on_client_connected;

    mqvpn_server_t *svr = mqvpn_server_new(svr_cfg, &svr_cbs, NULL);
    ASSERT_NOT_NULL(svr);
    mqvpn_config_free(svr_cfg);

    ASSERT_EQ(mqvpn_server_set_socket_fd(svr, svr_fd, (struct sockaddr *)&svr_addr,
                                         sizeof(svr_addr)),
              MQVPN_OK);
    ASSERT_EQ(mqvpn_server_start(svr), MQVPN_OK);

    /* Client setup */
    mqvpn_config_t *cli_cfg = mqvpn_config_new();
    mqvpn_config_set_server(cli_cfg, "127.0.0.1", ntohs(svr_addr.sin_port));
    mqvpn_config_set_insecure(cli_cfg, 1);
    mqvpn_config_set_log_level(cli_cfg, MQVPN_LOG_ERROR);

    mqvpn_client_callbacks_t cli_cbs = MQVPN_CLIENT_CALLBACKS_INIT;
    cli_cbs.tun_output = mock_cli_tun_output;
    cli_cbs.tunnel_config_ready = mock_cli_tunnel_ready;

    mqvpn_client_t *cli = mqvpn_client_new(cli_cfg, &cli_cbs, NULL);
    ASSERT_NOT_NULL(cli);
    mqvpn_config_free(cli_cfg);

    /* Add path and connect */
    mqvpn_path_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.struct_size = sizeof(desc);
    memcpy(desc.local_addr, &cli_addr, sizeof(cli_addr));
    desc.local_addr_len = sizeof(cli_addr);

    mqvpn_path_handle_t path_h = mqvpn_client_add_path_fd(cli, cli_fd, &desc);
    ASSERT_NE(path_h, (mqvpn_path_handle_t)-1);

    mqvpn_client_set_server_addr(cli, (struct sockaddr *)&svr_addr, sizeof(svr_addr));
    ASSERT_EQ(mqvpn_client_connect(cli), MQVPN_OK);

    /* Pump until tunnel is established */
    for (int elapsed = 0; elapsed < 10000; elapsed++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd, path_h);
        if (g_client_connected_called > 0 && g_cli_tunnel_ready_called > 0) break;

        struct pollfd pfds[2] = {
            {.fd = svr_fd, .events = POLLIN},
            {.fd = cli_fd, .events = POLLIN},
        };
        int w = poll(pfds, 2, 10);
        elapsed += (w == 0) ? 10 : 1;
    }

    /* Verify tunnel is established */
    ASSERT_EQ(g_client_connected_called, 1);
    ASSERT_EQ(g_cli_tunnel_ready_called, 1);

    /* Additional drains to ensure state is fully synced */
    for (int i = 0; i < 50; i++) {
        drain_and_tick(svr, svr_fd, cli, cli_fd, path_h);
    }

    /* Query get_status with connected client */
    mqvpn_client_info_t clients[MQVPN_MAX_USERS];
    int n_clients = 0;
    ASSERT_EQ(mqvpn_server_get_client_info(svr, clients, MQVPN_MAX_USERS, &n_clients),
              MQVPN_OK);

    /* Verify: n_clients should be 1, confirming client is visible to API */
    ASSERT_EQ(n_clients, 1);

    /* Verify client info structure is populated */
    mqvpn_client_info_t *ci = &clients[0];
    /* Should have at least one path */
    ASSERT_NE(ci->n_paths, 0);

    /* Cleanup */
    mqvpn_client_disconnect(cli);
    mqvpn_client_destroy(cli);
    mqvpn_server_destroy(svr);
    close(svr_fd);
    close(cli_fd);
}

/* ── Fixed (pinned) IP tests ── */

TEST(server_pinned_ip_preconfig_reserves_pool)
{
    /* A fixed IP set before server_new is pre-reserved in the pool at startup.
     * Attempting to assign the same address to a different user must fail. */
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_config_add_user(cfg, "alice", "alice-key");
    mqvpn_config_set_user_fixed_ip(cfg, "alice", "10.0.0.5");
    mqvpn_config_add_user(cfg, "bob", "bob-key");

    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    ASSERT_NOT_NULL(s);

    /* 10.0.0.5 is reserved for alice — bob cannot take it */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", "10.0.0.5"), MQVPN_ERR_POOL_FULL);

    /* Clearing alice's reservation releases the address */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "alice", ""), MQVPN_OK);

    /* Now bob can claim 10.0.0.5 */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", "10.0.0.5"), MQVPN_OK);

    mqvpn_server_destroy(s);
}

TEST(server_set_user_fixed_ip_api)
{
    /* Exercise the runtime set_user_fixed_ip API: set, update, conflict, clear */
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_config_add_user(cfg, "alice", "alice-key");
    mqvpn_config_add_user(cfg, "bob", "bob-key");

    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    ASSERT_NOT_NULL(s);

    /* Assign a fixed IP to alice */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "alice", "10.0.0.10"), MQVPN_OK);

    /* Same IP for bob must fail */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", "10.0.0.10"), MQVPN_ERR_POOL_FULL);

    /* Update alice to a different IP — old one is freed */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "alice", "10.0.0.11"), MQVPN_OK);

    /* Old IP 10.0.0.10 is now free, bob can take it */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", "10.0.0.10"), MQVPN_OK);

    /* Clear bob's IP */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", ""), MQVPN_OK);

    /* Error cases */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "carol", "10.0.0.5"),
              MQVPN_ERR_INVALID_ARG); /* unknown user */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "alice", "not-an-ip"),
              MQVPN_ERR_INVALID_ARG); /* bad IP string */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "alice", "192.168.1.5"),
              MQVPN_ERR_INVALID_ARG); /* outside subnet */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(NULL, "alice", "10.0.0.5"),
              MQVPN_ERR_INVALID_ARG);

    mqvpn_server_destroy(s);
}

TEST(server_remove_user_releases_pinned_ip)
{
    /* Removing a user with a pinned IP frees that address back to the pool. */
    mqvpn_config_t *cfg = make_server_config();
    mqvpn_config_add_user(cfg, "alice", "alice-key");
    mqvpn_config_add_user(cfg, "bob", "bob-key");

    mqvpn_server_callbacks_t cbs = MQVPN_SERVER_CALLBACKS_INIT;
    cbs.tun_output = mock_tun_output;
    cbs.tunnel_config_ready = mock_tunnel_config_ready;
    mqvpn_server_t *s = mqvpn_server_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    ASSERT_NOT_NULL(s);

    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "alice", "10.0.0.7"), MQVPN_OK);

    /* 10.0.0.7 is reserved — bob can't take it */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", "10.0.0.7"), MQVPN_ERR_POOL_FULL);

    /* Removing alice releases her pinned IP */
    ASSERT_EQ(mqvpn_server_remove_user(s, "alice"), MQVPN_OK);

    /* Now bob can claim 10.0.0.7 */
    ASSERT_EQ(mqvpn_server_set_user_fixed_ip(s, "bob", "10.0.0.7"), MQVPN_OK);

    mqvpn_server_destroy(s);
}

/* ── Regression: issue #4282 — server crash on non-H3 probe client ──
 *
 * Root cause: cb_write_socket, cb_path_created, and cb_path_removed are
 * transport-level callbacks that receive conn->user_data.  On the server,
 * conn->user_data starts as mqvpn_server_t * and is only promoted to
 * svr_conn_t * once cb_h3_conn_create fires (after successful H3/ALPN
 * negotiation).  A probe that sends a QUIC Initial with a non-H3 ALPN
 * causes ALPN selection to return NOACK — H3 is never set up, so
 * conn->user_data stays as mqvpn_server_t *.  When xquic then calls
 * cb_write_socket to send the Server Hello, the old code blindly cast
 * mqvpn_server_t * to svr_conn_t * and read svr_conn_t::server from
 * offset 0 — which is the first 8 bytes of mqvpn_config_t::server_host
 * (a string like "0.0.0.0") treated as a pointer → SIGSEGV.
 *
 * The fix adds a magic tag to svr_conn_t and a server_from_ud() helper
 * that resolves the correct mqvpn_server_t * regardless of which type
 * the conn_user_data pointer actually is.
 */

typedef struct {
    int               fd;
    struct sockaddr_in peer;
    socklen_t          peer_len;
} probe_ctx_t;

static ssize_t
probe_write_socket(const unsigned char *buf, size_t size, const struct sockaddr *peer,
                   socklen_t peerlen, void *ud)
{
    probe_ctx_t *p = (probe_ctx_t *)ud;
    return sendto(p->fd, buf, size, 0, peer, peerlen);
}

static void
probe_set_event_timer(xqc_msec_t wake_after, void *ud) { (void)ud; (void)wake_after; }

static void
probe_log_write(xqc_log_level_t lvl, const void *buf, size_t size, void *ud)
{
    (void)lvl; (void)buf; (void)size; (void)ud;
}

TEST(server_no_crash_on_non_h3_probe)
{
    reset_mocks();

    int svr_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(svr_fd, -1);
    int probe_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, 0);
    ASSERT_NE(probe_fd, -1);

    struct sockaddr_in svr_addr, probe_addr;
    memset(&svr_addr, 0, sizeof(svr_addr));
    svr_addr.sin_family = AF_INET;
    svr_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    svr_addr.sin_port = 0;
    ASSERT_EQ(bind(svr_fd, (struct sockaddr *)&svr_addr, sizeof(svr_addr)), 0);

    memset(&probe_addr, 0, sizeof(probe_addr));
    probe_addr.sin_family = AF_INET;
    probe_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    probe_addr.sin_port = 0;
    ASSERT_EQ(bind(probe_fd, (struct sockaddr *)&probe_addr, sizeof(probe_addr)), 0);

    socklen_t alen = sizeof(svr_addr);
    getsockname(svr_fd, (struct sockaddr *)&svr_addr, &alen);
    alen = sizeof(probe_addr);
    getsockname(probe_fd, (struct sockaddr *)&probe_addr, &alen);

    /* Start the server */
    mqvpn_config_t *svr_cfg = make_server_config();
    mqvpn_server_callbacks_t svr_cbs = MQVPN_SERVER_CALLBACKS_INIT;
    svr_cbs.tun_output = mock_tun_output;
    svr_cbs.tunnel_config_ready = mock_tunnel_config_ready;
    svr_cbs.log = mock_log;

    mqvpn_server_t *svr = mqvpn_server_new(svr_cfg, &svr_cbs, NULL);
    ASSERT_NOT_NULL(svr);
    mqvpn_config_free(svr_cfg);
    ASSERT_EQ(mqvpn_server_set_socket_fd(svr, svr_fd, (struct sockaddr *)&svr_addr,
                                         sizeof(svr_addr)), MQVPN_OK);
    ASSERT_EQ(mqvpn_server_start(svr), MQVPN_OK);

    /* Build a raw xquic client engine using "probe" as the ALPN — a protocol
     * the server does not support.  This exercises the path where the server
     * creates a QUIC connection but H3 is never initialised because ALPN
     * selection returns SSL_TLSEXT_ERR_NOACK instead of matching "h3". */
    probe_ctx_t probe = { .fd = probe_fd };
    memcpy(&probe.peer, &svr_addr, sizeof(svr_addr));
    probe.peer_len = sizeof(svr_addr);

    xqc_engine_ssl_config_t probe_ssl = {0};
    probe_ssl.ciphers = XQC_TLS_CIPHERS;
    probe_ssl.groups  = XQC_TLS_GROUPS;

    xqc_engine_callback_t probe_eng_cbs = {
        .set_event_timer = probe_set_event_timer,
        .log_callbacks   = { .xqc_log_write_err  = probe_log_write,
                             .xqc_log_write_stat = probe_log_write },
    };
    xqc_transport_callbacks_t probe_tcbs = { .write_socket = probe_write_socket };

    xqc_config_t xcfg;
    xqc_engine_get_default_config(&xcfg, XQC_ENGINE_CLIENT);
    xcfg.cfg_log_level = XQC_LOG_ERROR;

    xqc_engine_t *probe_engine = xqc_engine_create(XQC_ENGINE_CLIENT, &xcfg, &probe_ssl,
                                                    &probe_eng_cbs, &probe_tcbs, &probe);
    ASSERT_NOT_NULL(probe_engine);

    /* Register "probe" as a no-op ALPN so xqc_connect accepts it.  The server
     * does not recognise "probe", so its ALPN selection returns NOACK and H3
     * is never initialised — leaving conn->user_data as mqvpn_server_t *. */
    xqc_app_proto_callbacks_t probe_ap;
    memset(&probe_ap, 0, sizeof(probe_ap));
    xqc_engine_register_alpn(probe_engine, "probe", 5, &probe_ap, NULL);

    xqc_conn_settings_t cs;
    memset(&cs, 0, sizeof(cs));
    cs.proto_version = XQC_VERSION_V1;

    xqc_conn_ssl_config_t css;
    memset(&css, 0, sizeof(css));
    css.cert_verify_flag = XQC_TLS_CERT_FLAG_ALLOW_SELF_SIGNED;

    /* Connect with "probe" ALPN — deliberately not "h3" */
    const xqc_cid_t *pcid = xqc_connect(probe_engine, &cs, NULL, 0,
                                         "127.0.0.1", 0, &css,
                                         (struct sockaddr *)&svr_addr, sizeof(svr_addr),
                                         "probe", &probe);
    ASSERT_NOT_NULL(pcid);

    /* Pump: probe → server → server tries to respond via cb_write_socket.
     * Before the fix, the server crashes here (SIGSEGV) because cb_write_socket
     * cast mqvpn_server_t * to svr_conn_t * and dereferenced a garbage pointer.
     * After the fix, server_from_ud() resolves the correct server pointer and
     * the send succeeds.  Reaching the end of this loop means no crash. */
    uint8_t buf[65536];
    struct sockaddr_storage from;
    socklen_t from_len;

    for (int iter = 0; iter < 30; iter++) {
        xqc_engine_main_logic(probe_engine);

        for (;;) {
            from_len = sizeof(from);
            ssize_t n = recvfrom(svr_fd, buf, sizeof(buf), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &from_len);
            if (n <= 0) break;
            mqvpn_server_on_socket_recv(svr, buf, (size_t)n,
                                        (struct sockaddr *)&from, from_len);
        }

        for (;;) {
            from_len = sizeof(from);
            ssize_t n = recvfrom(probe_fd, buf, sizeof(buf), MSG_DONTWAIT,
                                 (struct sockaddr *)&from, &from_len);
            if (n <= 0) break;
            xqc_engine_packet_process(probe_engine, buf, (size_t)n,
                                      (struct sockaddr *)&probe_addr, sizeof(probe_addr),
                                      (struct sockaddr *)&from, from_len,
                                      (xqc_usec_t)(time(NULL)) * 1000000ULL, &probe);
        }

        mqvpn_server_tick(svr);

        struct pollfd pfds[2] = {
            { .fd = svr_fd,   .events = POLLIN },
            { .fd = probe_fd, .events = POLLIN },
        };
        poll(pfds, 2, 5);
    }

    xqc_engine_destroy(probe_engine);
    mqvpn_server_destroy(svr);
    close(svr_fd);
    close(probe_fd);
}

/* ── max_clients config boundary ── */

TEST(server_max_clients_config)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_listen(cfg, "0.0.0.0", 4433);
    mqvpn_config_set_max_clients(cfg, 1);
    ASSERT_EQ(cfg->max_clients, 1);

    /* Setter stores value as-is (no clamp) */
    mqvpn_config_set_max_clients(cfg, 0);
    ASSERT_EQ(cfg->max_clients, 0);

    mqvpn_config_free(cfg);
}

/* ── Main ── */

int
main(void)
{
    printf("test_server: libmqvpn server API tests\n");

    /* server_new validation */
    run_server_new_null_config();
    run_server_new_null_callbacks();
    run_server_new_bad_abi();
    run_server_new_missing_tun_output();
    run_server_new_missing_tunnel_config_ready();
    run_server_new_destroy();
    run_server_destroy_null();

    /* Lifecycle */
    run_server_lifecycle();
    run_server_lifecycle_with_v6();
    run_server_double_start();

    /* set_socket_fd */
    run_server_set_socket_fd();

    /* Null safety */
    run_server_get_stats_null();
    run_server_get_interest_null();
    run_server_tick_null();
    run_server_on_tun_packet_null();
    run_server_on_socket_recv_null();

    /* TUN packet with no sessions */
    run_server_on_tun_packet_no_sessions();

    /* Session lifecycle (test_server_session per impl_plan) */
    run_server_session_callbacks_registered();
    run_server_session_set_socket_with_addr();
    run_server_session_on_tun_v6_no_sessions();

    /* QUIC loopback integration test (test_server_session per impl_plan) */
    run_server_session_quic_loopback();
    run_server_runtime_added_path_not_stuck_pending();

    /* Control API: get_status with and without clients */
    run_server_get_status_no_clients();
    run_server_get_status_with_client();

    /* Fixed (pinned) IP tests */
    run_server_pinned_ip_preconfig_reserves_pool();
    run_server_set_user_fixed_ip_api();
    run_server_remove_user_releases_pinned_ip();

    /* max_clients config boundary */
    run_server_max_clients_config();

    /* Regression: issue #4282 — no crash on non-H3 probe */
    run_server_no_crash_on_non_h3_probe();

    printf("\n  %d/%d tests passed\n", g_tests_passed, g_tests_run);
    return (g_tests_passed == g_tests_run) ? 0 : 1;
}
