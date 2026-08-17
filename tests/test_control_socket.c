/*
 * test_control_socket.c — Unit tests for control_socket.c dispatch()
 *
 * Drives dispatch() end to end with the server-facing API mocked: command
 * routing, argument validation, error codes and response JSON shape for the
 * server commands (user management, stats/status/build info, FEC and reorder
 * stats), the client-mode path commands (add_path/remove_path/list_paths/
 * set_path_weight/set_path_dscp_mask, keyed by iface), and the dual-mode
 * set_path_weight/set_path_dscp_mask server-mode form (keyed by user +
 * path_id instead) — including the get_all_fec_stats branch which had a
 * missing return value when truncated == 0.
 *
 * Includes control_socket.c directly (same technique as test_status.c) and
 * provides lightweight stubs for all external API dependencies.  Links only
 * libevent (needed by the non-dispatch socket machinery in control_socket.c)
 * and reorder_rx.c (real latency-percentile helpers used by
 * get_reorder_stats).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <inttypes.h>

/* ── Concrete bodies for the two opaque server/client types ─────────────── *
 * libmqvpn.h forward-declares these as "struct mqvpn_server_s" etc.        *
 * We only use them via pointer in dispatch(), so a dummy body suffices.     */
struct mqvpn_server_s { int _; };
struct mqvpn_client_s { int _; };

/* ── Headers (declares types and API functions) ──────────────────────────── */
#include "libmqvpn.h"
#include "mqvpn_internal.h"
#include "log.h"

/* ── Stub log functions (replaces log.c) ────────────────────────────────── */
void mqvpn_log(mqvpn_log_level_t level, const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}
void mqvpn_log_set_level(mqvpn_log_level_t level) { (void)level; }

/* ── Stub state ──────────────────────────────────────────────────────────── */
static mqvpn_internal_fec_entry_t g_fec_entries[MQVPN_MAX_USERS];
static int g_fec_n = 0; /* entry count; -1 = FEC not built */

/* Configurable return values for user management stubs */
static int g_add_user_rc      = MQVPN_OK;
static int g_set_fixed_ip_rc  = MQVPN_OK;
static int g_remove_user_rc   = MQVPN_OK;
static int g_list_users_n     = 0;
static char g_list_users_names[MQVPN_MAX_USERS][64];

/* Configurable state for stats/status/FEC/reorder stubs */
static int g_n_clients = 0;
static uint64_t g_uptime = 0;
static int g_client_info_n = 0;
static mqvpn_client_info_t g_client_info_tmpl;
static int g_client_fec_rc = -1; /* -1 = not built, 0 = not found, 1 = found */
static mqvpn_internal_fec_stats_t g_client_fec_tmpl;

static mqvpn_internal_client_reinject_t g_reinject_tmpl; /* configured per test */

static int g_all_fec_rc = 0;
static int g_all_fec_n = 0;

static int g_reorder_rc = 0;

/* ── Server API stubs ────────────────────────────────────────────────────── */
int mqvpn_server_add_user(mqvpn_server_t *s, const char *n, const char *k)
{ (void)s; (void)n; (void)k; return g_add_user_rc; }

int mqvpn_server_set_user_fixed_ip(mqvpn_server_t *s, const char *n, const char *ip)
{ (void)s; (void)n; (void)ip; return g_set_fixed_ip_rc; }

int mqvpn_server_remove_user(mqvpn_server_t *s, const char *n)
{ (void)s; (void)n; return g_remove_user_rc; }

int mqvpn_server_list_users(const mqvpn_server_t *s, char names[][64], int max)
{
    (void)s;
    int n = g_list_users_n < max ? g_list_users_n : max;
    for (int i = 0; i < n; i++)
        strncpy(names[i], g_list_users_names[i], 63);
    return n;
}

int mqvpn_server_get_stats(const mqvpn_server_t *s, mqvpn_stats_t *st)
{
    (void)s;
    memset(st, 0, sizeof(*st));
    st->struct_size = sizeof(*st);
    st->bytes_tx = 111;
    st->tcp_flows_total = 7;
    st->udp_tx_sends = 1234;
    st->udp_tx_datagrams = 5678;
    return MQVPN_OK;
}

int mqvpn_server_get_n_clients(const mqvpn_server_t *s) { (void)s; return g_n_clients; }

uint64_t mqvpn_server_uptime_seconds(const mqvpn_server_t *s) { (void)s; return g_uptime; }

int mqvpn_server_get_client_info(const mqvpn_server_t *s,
                                  mqvpn_client_info_t *out, int max, int *n)
{
    (void)s;
    int nn = g_client_info_n < max ? g_client_info_n : max;
    for (int i = 0; i < nn; i++)
        out[i] = g_client_info_tmpl;
    *n = nn;
    return MQVPN_OK;
}

const char *mqvpn_version_string(void) { return "test"; }

const char *mqvpn_server_scheduler_label(const mqvpn_server_t *s)
{ (void)s; return "none"; }

const char *mqvpn_path_state_label(int state)
{ return state == 2 ? "active" : "validating"; }

int
mqvpn_server_get_client_reinject(const mqvpn_server_t *s,
                                 mqvpn_internal_client_reinject_t *out, int max)
{
    (void)s;
    if (max > 0) out[0] = g_reinject_tmpl;
    return max > 0 ? 1 : 0;
}

int mqvpn_server_get_client_fec_stats(const mqvpn_server_t *s,
                                       const char *user,
                                       mqvpn_internal_fec_stats_t *out)
{
    (void)s;
    (void)user;
    if (g_client_fec_rc == 1) *out = g_client_fec_tmpl;
    return g_client_fec_rc;
}

int mqvpn_server_get_all_fec_stats(const mqvpn_server_t *s,
                                    mqvpn_internal_fec_entry_t *out, int max)
{
    (void)s;
    if (g_fec_n < 0) return -1;
    int n = g_fec_n < max ? g_fec_n : max;
    memcpy(out, g_fec_entries, (size_t)n * sizeof(*out));
    return n;
}

int mqvpn_server_get_reorder_stats(const mqvpn_server_t *s, mqvpn_reorder_stats_t *out)
{
    (void)s;
    if (out) {
        memset(out, 0, sizeof(*out));
        out->delivered_count = 55;
    }
    return g_reorder_rc;
}

/* Configurable return + captured-args for server-mode set_path_weight /
 * set_path_dscp_mask (keyed by user + path_id, not iface — see
 * control_socket.c's doc comment). */
static int g_srv_set_weight_rc = MQVPN_OK;
static char g_srv_set_weight_user[64];
static uint64_t g_srv_set_weight_path_id;
static uint32_t g_srv_set_weight_weight;

int mqvpn_server_set_path_weight(mqvpn_server_t *s, const char *user, uint64_t path_id,
                                 uint32_t weight)
{
    (void)s;
    snprintf(g_srv_set_weight_user, sizeof(g_srv_set_weight_user), "%s", user ? user : "");
    g_srv_set_weight_path_id = path_id;
    g_srv_set_weight_weight = weight;
    return g_srv_set_weight_rc;
}

static int g_srv_set_dscp_mask_rc = MQVPN_OK;
static char g_srv_set_dscp_mask_user[64];
static uint64_t g_srv_set_dscp_mask_path_id;
static uint64_t g_srv_set_dscp_mask_mask;

int mqvpn_server_set_path_dscp_mask(mqvpn_server_t *s, const char *user, uint64_t path_id,
                                    uint64_t dscp_mask)
{
    (void)s;
    snprintf(g_srv_set_dscp_mask_user, sizeof(g_srv_set_dscp_mask_user), "%s",
             user ? user : "");
    g_srv_set_dscp_mask_path_id = path_id;
    g_srv_set_dscp_mask_mask = dscp_mask;
    return g_srv_set_dscp_mask_rc;
}

/* Configurable return + captured-args for the _by_iface (persistent)
 * variants — see mqvpn_path_label.h. */
static int g_srv_set_weight_iface_rc = MQVPN_OK;
static char g_srv_set_weight_iface_user[64];
static char g_srv_set_weight_iface_iface[32];
static uint32_t g_srv_set_weight_iface_weight;

int mqvpn_server_set_path_weight_by_iface(mqvpn_server_t *s, const char *user,
                                          const char *iface, uint32_t weight)
{
    (void)s;
    snprintf(g_srv_set_weight_iface_user, sizeof(g_srv_set_weight_iface_user), "%s",
             user ? user : "");
    snprintf(g_srv_set_weight_iface_iface, sizeof(g_srv_set_weight_iface_iface), "%s",
             iface ? iface : "");
    g_srv_set_weight_iface_weight = weight;
    return g_srv_set_weight_iface_rc;
}

static int g_srv_set_dscp_mask_iface_rc = MQVPN_OK;
static char g_srv_set_dscp_mask_iface_user[64];
static char g_srv_set_dscp_mask_iface_iface[32];
static uint64_t g_srv_set_dscp_mask_iface_mask;

int mqvpn_server_set_path_dscp_mask_by_iface(mqvpn_server_t *s, const char *user,
                                             const char *iface, uint64_t dscp_mask)
{
    (void)s;
    snprintf(g_srv_set_dscp_mask_iface_user, sizeof(g_srv_set_dscp_mask_iface_user), "%s",
             user ? user : "");
    snprintf(g_srv_set_dscp_mask_iface_iface, sizeof(g_srv_set_dscp_mask_iface_iface), "%s",
             iface ? iface : "");
    g_srv_set_dscp_mask_iface_mask = dscp_mask;
    return g_srv_set_dscp_mask_iface_rc;
}

/* ── Client API stubs (cli_ctx branch, see call_dispatch_client below) ──────
 * mqvpn_client_t is the same dummy 1-int body as mqvpn_server_t (top of
 * file) -- these functions never dereference their `c` argument, they read
 * only the g_cli_* globals below, configured per test exactly like the
 * g_client_info_tmpl / g_reinject_tmpl / g_client_fec_tmpl stubs above. */
static const char *g_cli_scheduler_label = "unknown";
static uint64_t g_cli_uptime = 0;
static mqvpn_client_state_t g_cli_state = MQVPN_STATE_IDLE;
static int g_cli_stats_rc = MQVPN_OK;
static mqvpn_stats_t g_cli_stats_tmpl;
static int g_cli_info_rc = 0;        /* 0 = not connected, 1 = filled */
static mqvpn_client_info_t g_cli_info_tmpl;
static int g_cli_reinject_rc = 0;    /* 0 = not connected, 1 = filled */
static mqvpn_internal_client_reinject_t g_cli_reinject_tmpl;
static int g_cli_fec_rc = -1;        /* -1 = not built, 0 = not connected, 1 = filled */
static mqvpn_internal_fec_stats_t g_cli_fec_tmpl;
static int g_cli_reorder_rc = 0;
static mqvpn_reorder_stats_t g_cli_reorder_tmpl;

const char *mqvpn_client_scheduler_label(const mqvpn_client_t *c)
{ (void)c; return g_cli_scheduler_label; }

uint64_t mqvpn_client_uptime_seconds(const mqvpn_client_t *c)
{ (void)c; return g_cli_uptime; }

mqvpn_client_state_t mqvpn_client_get_state(const mqvpn_client_t *c)
{ (void)c; return g_cli_state; }

int mqvpn_client_get_stats(const mqvpn_client_t *c, mqvpn_stats_t *out)
{ (void)c; *out = g_cli_stats_tmpl; return g_cli_stats_rc; }

int mqvpn_client_get_info(const mqvpn_client_t *c, mqvpn_client_info_t *out)
{
    (void)c;
    if (g_cli_info_rc == 1) *out = g_cli_info_tmpl;
    else memset(out, 0, sizeof(*out));
    return g_cli_info_rc;
}

int mqvpn_client_get_reinject(const mqvpn_client_t *c,
                              mqvpn_internal_client_reinject_t *out)
{
    (void)c;
    if (g_cli_reinject_rc == 1) *out = g_cli_reinject_tmpl;
    else out->n_paths = 0;
    return g_cli_reinject_rc;
}

int mqvpn_client_get_fec_stats(const mqvpn_client_t *c, mqvpn_internal_fec_stats_t *out)
{
    (void)c;
    memset(out, 0, sizeof(*out));
    if (g_cli_fec_rc == 1) *out = g_cli_fec_tmpl;
    return g_cli_fec_rc;
}

int mqvpn_client_get_reorder_stats(const mqvpn_client_t *c, mqvpn_reorder_stats_t *out)
{
    (void)c;
    if (out) *out = g_cli_reorder_tmpl;
    return g_cli_reorder_rc;
}

/* ── Platform stubs ─────────────────────────────────────────────────────── */
#include "platform_internal.h"

/* Configurable return values for path management stubs */
static int g_add_path_rc          = 0;
static int g_remove_path_rc       = 0;
static int g_set_path_weight_rc   = 0;
static int g_set_path_dscp_mask_rc = 0;
static int g_list_paths_n         = 0;
static char g_list_paths_names[MQVPN_MAX_PATHS][IFNAMSIZ];

int platform_add_path(platform_ctx_t *p, const char *iface, int backup)
{ (void)p; (void)iface; (void)backup; return g_add_path_rc; }

int platform_remove_path(platform_ctx_t *p, const char *iface)
{ (void)p; (void)iface; return g_remove_path_rc; }

int platform_list_paths(platform_ctx_t *p, char names[][IFNAMSIZ], int max)
{
    (void)p;
    int n = g_list_paths_n < max ? g_list_paths_n : max;
    for (int i = 0; i < n; i++)
        strncpy(names[i], g_list_paths_names[i], IFNAMSIZ - 1);
    return n;
}

int platform_set_path_weight(platform_ctx_t *p, const char *iface, uint32_t weight)
{ (void)p; (void)iface; (void)weight; return g_set_path_weight_rc; }

int platform_set_path_dscp_mask(platform_ctx_t *p, const char *iface, uint64_t dscp_mask)
{ (void)p; (void)iface; (void)dscp_mask; return g_set_path_dscp_mask_rc; }

/* ── Pull in the implementation under test ──────────────────────────────── */
#include "../src/platform/linux/control_socket.c"

/* ── Test infrastructure ─────────────────────────────────────────────────── */
static int g_run    = 0;
static int g_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void)   \
    {                              \
        g_run++;                   \
        printf("  %-60s ", #name); \
        test_##name();             \
        g_passed++;                \
        printf("PASS\n");          \
    }                              \
    static void test_##name(void)

#define ASSERT_STR_EQ(a, b)                                           \
    do {                                                              \
        if (strcmp((a), (b)) != 0) {                                  \
            printf("FAIL\n    %s:%d:\n      got:  \"%s\"\n"           \
                   "      want: \"%s\"\n", __FILE__, __LINE__, (a), (b)); \
            exit(1);                                                   \
        }                                                             \
    } while (0)

#define ASSERT_CONTAINS(haystack, needle)                                  \
    do {                                                                   \
        if (!strstr((haystack), (needle))) {                               \
            printf("FAIL\n    %s:%d: \"%s\" not found in \"%s\"\n",       \
                   __FILE__, __LINE__, (needle), (haystack));              \
            exit(1);                                                       \
        }                                                                  \
    } while (0)

/* Platform-owned RX offload counters the control socket borrows. Non-zero and
 * unequal so a get_stats regression that hardcodes 0 or swaps the pair cannot
 * pass. */
static uint64_t g_gro_receives = 61;
static uint64_t g_gro_datagrams = 83;

static int
call_dispatch(const char *req, char *resp, size_t resp_len)
{
    /* Stack-built context: dispatch and the handlers only read ->server,
     * ->cli_ctx and the borrowed counter pointers, never the libevent
     * members. */
    ctrl_socket_t cs = {
        .server = (mqvpn_server_t *)NULL,
        .cli_ctx = NULL,
        .gro_receives = &g_gro_receives,
        .gro_datagrams = &g_gro_datagrams,
    };
    return dispatch(req, resp, resp_len, &cs);
}

/* call_dispatch_client: passes a non-NULL cli_ctx so path commands work */
static platform_ctx_t g_fake_cli_ctx;
static int
call_dispatch_client(const char *req, char *resp, size_t resp_len)
{
    ctrl_socket_t cs = {
        .server = (mqvpn_server_t *)NULL,
        .cli_ctx = &g_fake_cli_ctx,
        .gro_receives = &g_gro_receives,
        .gro_datagrams = &g_gro_datagrams,
    };
    return dispatch(req, resp, resp_len, &cs);
}

/* ── Tests ───────────────────────────────────────────────────────────────── */

/* Regression: dispatch() previously fell through without returning when
 * get_all_fec_stats returned 0 entries and truncated == 0. */
TEST(get_all_fec_stats_empty)
{
    g_fec_n = 0;
    char resp[4096];
    call_dispatch("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true,\"n_clients\":0,\"clients\":[]}");
}

TEST(get_all_fec_stats_one_entry)
{
    g_fec_n = 1;
    memset(g_fec_entries, 0, sizeof(g_fec_entries));
    strncpy(g_fec_entries[0].user, "alice", sizeof(g_fec_entries[0].user) - 1);
    g_fec_entries[0].stats.enable_fec        = 1;
    g_fec_entries[0].stats.mp_state          = 1;
    g_fec_entries[0].stats.mp_state_label    = "active_with_standby";
    g_fec_entries[0].stats.fec_send_cnt      = 42;
    g_fec_entries[0].stats.fec_recover_cnt   = 17;
    g_fec_entries[0].stats.lost_dgram_cnt    = 3;
    g_fec_entries[0].stats.total_app_bytes   = 9000;
    g_fec_entries[0].stats.standby_app_bytes = 1234;

    char resp[4096];
    call_dispatch("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"n_clients\":1");
    ASSERT_CONTAINS(resp, "\"user\":\"alice\"");
    ASSERT_CONTAINS(resp, "\"enable_fec\":1");
    ASSERT_CONTAINS(resp, "\"fec_send_cnt\":42");
    ASSERT_CONTAINS(resp, "\"mp_state_label\":\"active_with_standby\"");
}

TEST(get_all_fec_stats_two_entries)
{
    g_fec_n = 2;
    memset(g_fec_entries, 0, sizeof(g_fec_entries));
    strncpy(g_fec_entries[0].user, "alice", sizeof(g_fec_entries[0].user) - 1);
    strncpy(g_fec_entries[1].user, "bob",   sizeof(g_fec_entries[1].user) - 1);
    g_fec_entries[0].stats.mp_state_label = "single_path";
    g_fec_entries[1].stats.mp_state_label = "single_path";

    char resp[4096];
    call_dispatch("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"n_clients\":2");
    ASSERT_CONTAINS(resp, "\"user\":\"alice\"");
    ASSERT_CONTAINS(resp, "\"user\":\"bob\"");
}

TEST(get_all_fec_stats_fec_not_built)
{
    g_fec_n = -1;
    char resp[4096];
    call_dispatch("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":false,\"error\":\"fec not built\"}");
}

TEST(dispatch_missing_cmd)
{
    char resp[4096];
    call_dispatch("{\"no_cmd\":1}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "missing cmd");
}

TEST(dispatch_unknown_cmd)
{
    char resp[4096];
    call_dispatch("{\"cmd\":\"bogus_command\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "unknown cmd");
}

/* ── add_user ────────────────────────────────────────────────────────────── */

TEST(add_user_success)
{
    g_add_user_rc = MQVPN_OK;
    char resp[256];
    call_dispatch("{\"cmd\":\"add_user\",\"name\":\"alice\",\"key\":\"s3cr3t\"}",
                  resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(add_user_missing_name)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"add_user\",\"key\":\"s3cr3t\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "name and key required");
}

TEST(add_user_missing_key)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"add_user\",\"name\":\"alice\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "name and key required");
}

TEST(add_user_server_failure)
{
    g_add_user_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch("{\"cmd\":\"add_user\",\"name\":\"alice\",\"key\":\"s3cr3t\"}",
                  resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "add_user failed");
    g_add_user_rc = MQVPN_OK;
}

TEST(add_user_with_fixed_ip_success)
{
    g_add_user_rc     = MQVPN_OK;
    g_set_fixed_ip_rc = MQVPN_OK;
    char resp[256];
    call_dispatch("{\"cmd\":\"add_user\",\"name\":\"carol\","
                  "\"key\":\"pass\",\"fixed_ip\":\"10.0.0.50\"}",
                  resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(add_user_with_fixed_ip_failure)
{
    g_add_user_rc     = MQVPN_OK;
    g_set_fixed_ip_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch("{\"cmd\":\"add_user\",\"name\":\"carol\","
                  "\"key\":\"pass\",\"fixed_ip\":\"10.0.0.99\"}",
                  resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "fixed_ip invalid");
    g_set_fixed_ip_rc = MQVPN_OK;
}

/* ── set_user_fixed_ip ───────────────────────────────────────────────────── */

TEST(set_user_fixed_ip_success)
{
    g_set_fixed_ip_rc = MQVPN_OK;
    char resp[256];
    call_dispatch("{\"cmd\":\"set_user_fixed_ip\",\"name\":\"alice\","
                  "\"fixed_ip\":\"10.0.0.50\"}",
                  resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(set_user_fixed_ip_missing_name)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"set_user_fixed_ip\",\"fixed_ip\":\"10.0.0.50\"}",
                  resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "name required");
}

TEST(set_user_fixed_ip_failure)
{
    g_set_fixed_ip_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch("{\"cmd\":\"set_user_fixed_ip\",\"name\":\"alice\","
                  "\"fixed_ip\":\"10.0.0.99\"}",
                  resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "set_user_fixed_ip failed");
    g_set_fixed_ip_rc = MQVPN_OK;
}

/* ── remove_user ─────────────────────────────────────────────────────────── */

TEST(remove_user_success)
{
    g_remove_user_rc = MQVPN_OK;
    char resp[256];
    call_dispatch("{\"cmd\":\"remove_user\",\"name\":\"alice\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(remove_user_missing_name)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"remove_user\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "name required");
}

TEST(remove_user_not_found)
{
    g_remove_user_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch("{\"cmd\":\"remove_user\",\"name\":\"nobody\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");
    g_remove_user_rc = MQVPN_OK;
}

/* ── list_users ──────────────────────────────────────────────────────────── */

TEST(list_users_empty)
{
    g_list_users_n = 0;
    char resp[512];
    call_dispatch("{\"cmd\":\"list_users\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"users\":[]");
}

TEST(list_users_two_entries)
{
    g_list_users_n = 2;
    strncpy(g_list_users_names[0], "alice", 63);
    strncpy(g_list_users_names[1], "bob",   63);
    char resp[512];
    call_dispatch("{\"cmd\":\"list_users\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"alice\"");
    ASSERT_CONTAINS(resp, "\"bob\"");
    g_list_users_n = 0;
}

/* ── get_stats / get_status / get_build_info ─────────────────────────────── */

TEST(get_stats)
{
    g_n_clients = 3;
    g_uptime = 4242;
    char resp[2048];
    call_dispatch("{\"cmd\":\"get_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"n_clients\":3");
    ASSERT_CONTAINS(resp, "\"bytes_tx\":111");
    ASSERT_CONTAINS(resp, "\"tcp_flows_total\":7");
    ASSERT_CONTAINS(resp, "\"uptime_sec\":4242");
    /* Offload counters reach the JSON from BOTH sources: udp_tx_* through
     * mqvpn_stats_t (the library issues those sends), udp_rx_* straight from
     * the platform's borrowed counters (GRO never crosses the library ABI).
     * The get_stats body is a hand-written field-by-field snprintf, so a new
     * mqvpn_stats_t field silently reads 0 here unless it is added in both
     * places — that is exactly the failure this pins. */
    ASSERT_CONTAINS(resp, "\"udp_tx_sends\":1234");
    ASSERT_CONTAINS(resp, "\"udp_tx_datagrams\":5678");
    ASSERT_CONTAINS(resp, "\"udp_rx_receives\":61");
    ASSERT_CONTAINS(resp, "\"udp_rx_datagrams\":83");
    g_n_clients = 0;
    g_uptime = 0;
}

/* Client mode: cs->server is NULL, so this must NOT fall through to the
 * server-mode stub above (which would read the g_n_clients/g_uptime
 * globals this test never sets) -- it has to route through
 * mqvpn_client_get_stats/get_state/uptime_seconds instead. This is the
 * live-observed regression: bytes_tx, dgram_*, and uptime_sec all read 0
 * despite a real, traffic-carrying tunnel until this branch existed. */
TEST(get_stats_client_mode_connected)
{
    memset(&g_cli_stats_tmpl, 0, sizeof(g_cli_stats_tmpl));
    g_cli_stats_tmpl.bytes_tx = 222;
    g_cli_stats_tmpl.tcp_flows_total = 9;
    g_cli_stats_tmpl.udp_tx_sends = 4321;
    g_cli_stats_tmpl.udp_tx_datagrams = 8765;
    g_cli_state = MQVPN_STATE_ESTABLISHED;
    g_cli_uptime = 1010;

    char resp[2048];
    call_dispatch_client("{\"cmd\":\"get_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"n_clients\":1");
    ASSERT_CONTAINS(resp, "\"bytes_tx\":222");
    ASSERT_CONTAINS(resp, "\"tcp_flows_total\":9");
    ASSERT_CONTAINS(resp, "\"uptime_sec\":1010");
    ASSERT_CONTAINS(resp, "\"udp_tx_sends\":4321");
    ASSERT_CONTAINS(resp, "\"udp_tx_datagrams\":8765");
    /* udp_rx_* still come from the borrowed GRO counters regardless of mode */
    ASSERT_CONTAINS(resp, "\"udp_rx_receives\":61");
    ASSERT_CONTAINS(resp, "\"udp_rx_datagrams\":83");

    g_cli_state = MQVPN_STATE_IDLE;
    g_cli_uptime = 0;
}

/* Not connected right now (IDLE/CONNECTING/etc.) -> n_clients:0, not an
 * error -- same "nobody connected" semantics a server with zero sessions
 * has, from this side's point of view. */
TEST(get_stats_client_mode_not_connected)
{
    g_cli_state = MQVPN_STATE_CONNECTING;
    char resp[2048];
    call_dispatch_client("{\"cmd\":\"get_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"n_clients\":0");
    g_cli_state = MQVPN_STATE_IDLE;
}

TEST(get_status_empty)
{
    g_client_info_n = 0;
    char resp[4096];
    call_dispatch("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true,\"n_clients\":0,\"clients\":[]}");
}

TEST(get_status_one_client_with_path)
{
    memset(&g_client_info_tmpl, 0, sizeof(g_client_info_tmpl));
    strncpy(g_client_info_tmpl.username, "alice",
            sizeof(g_client_info_tmpl.username) - 1);
    strncpy(g_client_info_tmpl.endpoint, "1.2.3.4:443",
            sizeof(g_client_info_tmpl.endpoint) - 1);
    g_client_info_tmpl.n_paths = 1;
    g_client_info_tmpl.paths[0].path_id = 7;
    g_client_info_tmpl.paths[0].state = 2; /* -> "active" */
    g_client_info_n = 1;
    memset(&g_reinject_tmpl, 0, sizeof(g_reinject_tmpl));

    char resp[8192];
    call_dispatch("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"n_clients\":1");
    ASSERT_CONTAINS(resp, "\"user\":\"alice\"");
    ASSERT_CONTAINS(resp, "\"endpoint\":\"1.2.3.4:443\"");
    ASSERT_CONTAINS(resp, "\"path_id\":7");
    ASSERT_CONTAINS(resp, "\"state_label\":\"active\"");
    g_client_info_n = 0;
}

/* Alignment semantics (a): a reinject snapshot entry whose path_id matches
 * the client-info path emits its nonzero value. */
TEST(get_status_reinject_matched_path_id)
{
    memset(&g_client_info_tmpl, 0, sizeof(g_client_info_tmpl));
    strcpy(g_client_info_tmpl.username, "alice");
    strcpy(g_client_info_tmpl.endpoint, "1.2.3.4:443");
    g_client_info_tmpl.n_paths = 1;
    g_client_info_tmpl.paths[0].path_id = 7;
    g_client_info_tmpl.paths[0].state = 2;
    g_client_info_n = 1;

    memset(&g_reinject_tmpl, 0, sizeof(g_reinject_tmpl));
    g_reinject_tmpl.n_paths = 1;
    g_reinject_tmpl.paths[0].path_id = 7;
    g_reinject_tmpl.paths[0].reinject_tx_bytes = 99999;

    char resp[8192];
    call_dispatch("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"path_id\":7");
    ASSERT_CONTAINS(resp, "\"reinject_tx_bytes\":99999");
    g_client_info_n = 0;
}

/* Alignment semantics (b): a mismatched or wholly absent path_id in the
 * reinject snapshot both emit "reinject_tx_bytes":0 — the field is always
 * present so the JSON shape stays constant regardless of match. Two phases
 * pin the same constant-shape outcome from two different snapshot states. */
TEST(get_status_reinject_mismatched_path_id)
{
    memset(&g_client_info_tmpl, 0, sizeof(g_client_info_tmpl));
    strcpy(g_client_info_tmpl.username, "alice");
    strcpy(g_client_info_tmpl.endpoint, "1.2.3.4:443");
    g_client_info_tmpl.n_paths = 1;
    g_client_info_tmpl.paths[0].path_id = 7;
    g_client_info_tmpl.paths[0].state = 2;
    g_client_info_n = 1;

    /* Phase 1: reinject snapshot has an entry, but its path_id mismatches. */
    memset(&g_reinject_tmpl, 0, sizeof(g_reinject_tmpl));
    g_reinject_tmpl.n_paths = 1;
    g_reinject_tmpl.paths[0].path_id = 42; /* mismatch vs client path_id 7 */
    g_reinject_tmpl.paths[0].reinject_tx_bytes = 99999;

    char resp[8192];
    call_dispatch("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"path_id\":7");
    ASSERT_CONTAINS(resp, "\"reinject_tx_bytes\":0");

    /* Phase 2: reinject snapshot has no entry at all (n_paths == 0). */
    memset(&g_reinject_tmpl, 0, sizeof(g_reinject_tmpl)); /* n_paths = 0 */

    call_dispatch("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"path_id\":7");
    ASSERT_CONTAINS(resp, "\"reinject_tx_bytes\":0");
    g_client_info_n = 0;
}

/* Client mode: mqvpn_server_get_client_info(NULL, ...) always yields
 * n_clients=0/clients:[] regardless of real connection state -- this is
 * the live-observed regression (get_status reported empty on a bench
 * router mid-tunnel with real traffic flowing). Must route through
 * mqvpn_client_get_info()/get_reinject() instead. */
TEST(get_status_client_mode_not_connected)
{
    g_cli_info_rc = 0;
    char resp[4096];
    call_dispatch_client("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true,\"n_clients\":0,\"clients\":[]}");
}

TEST(get_status_client_mode_connected_with_path)
{
    memset(&g_cli_info_tmpl, 0, sizeof(g_cli_info_tmpl));
    strncpy(g_cli_info_tmpl.username, "bob", sizeof(g_cli_info_tmpl.username) - 1);
    strncpy(g_cli_info_tmpl.endpoint, "5.6.7.8:443",
            sizeof(g_cli_info_tmpl.endpoint) - 1);
    g_cli_info_tmpl.n_paths = 1;
    g_cli_info_tmpl.paths[0].path_id = 3;
    g_cli_info_tmpl.paths[0].state = 2; /* -> "active" */
    g_cli_info_rc = 1;

    memset(&g_cli_reinject_tmpl, 0, sizeof(g_cli_reinject_tmpl));
    g_cli_reinject_tmpl.n_paths = 1;
    g_cli_reinject_tmpl.paths[0].path_id = 3;
    g_cli_reinject_tmpl.paths[0].reinject_tx_bytes = 55555;
    g_cli_reinject_rc = 1;

    char resp[8192];
    call_dispatch_client("{\"cmd\":\"get_status\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"n_clients\":1");
    ASSERT_CONTAINS(resp, "\"user\":\"bob\"");
    ASSERT_CONTAINS(resp, "\"endpoint\":\"5.6.7.8:443\"");
    ASSERT_CONTAINS(resp, "\"path_id\":3");
    ASSERT_CONTAINS(resp, "\"state_label\":\"active\"");
    ASSERT_CONTAINS(resp, "\"reinject_tx_bytes\":55555");

    g_cli_info_rc = 0;
    g_cli_reinject_rc = 0;
}

/* ── get_build_info ───────────────────────────────────────────────────────── */

TEST(get_build_info)
{
    char resp[512];
    call_dispatch("{\"cmd\":\"get_build_info\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"version\":\"test\"");
    ASSERT_CONTAINS(resp, "\"scheduler\":\"none\"");
    ASSERT_CONTAINS(resp, "\"fec_enabled\":");
}

/* Client mode: mqvpn_server_scheduler_label(NULL) degrades to "unknown"
 * rather than erroring, which is exactly how this was found live -- the
 * scheduler field always read "unknown" on a client-mode router regardless
 * of its real configured scheduler (minrtt, in that case). */
TEST(get_build_info_client_mode)
{
    g_cli_scheduler_label = "minrtt";
    char resp[512];
    call_dispatch_client("{\"cmd\":\"get_build_info\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"version\":\"test\"");
    ASSERT_CONTAINS(resp, "\"scheduler\":\"minrtt\"");
    ASSERT_CONTAINS(resp, "\"fec_enabled\":");
    g_cli_scheduler_label = "unknown";
}

/* ── get_fec_stats (per user) ────────────────────────────────────────────── */

TEST(get_fec_stats_missing_user)
{
    char resp[512];
    call_dispatch("{\"cmd\":\"get_fec_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user required");
}

TEST(get_fec_stats_user_not_found)
{
    g_client_fec_rc = 0;
    char resp[512];
    call_dispatch("{\"cmd\":\"get_fec_stats\",\"user\":\"ghost\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");
}

TEST(get_fec_stats_not_built)
{
    g_client_fec_rc = -1;
    char resp[512];
    call_dispatch("{\"cmd\":\"get_fec_stats\",\"user\":\"alice\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "fec not built");
}

TEST(get_fec_stats_success)
{
    memset(&g_client_fec_tmpl, 0, sizeof(g_client_fec_tmpl));
    g_client_fec_tmpl.enable_fec      = 1;
    g_client_fec_tmpl.mp_state        = 1;
    g_client_fec_tmpl.mp_state_label  = "active_with_standby";
    g_client_fec_tmpl.fec_send_cnt    = 142;
    g_client_fec_tmpl.fec_recover_cnt = 17;
    g_client_fec_rc = 1;
    char resp[1024];
    call_dispatch("{\"cmd\":\"get_fec_stats\",\"user\":\"alice\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"user\":\"alice\"");
    ASSERT_CONTAINS(resp, "\"mp_state_label\":\"active_with_standby\"");
    ASSERT_CONTAINS(resp, "\"fec_send_cnt\":142");
    ASSERT_CONTAINS(resp, "\"fec_recover_cnt\":17");
    g_client_fec_rc = -1;
}

/* Client mode: identity is checked against mqvpn_client_get_info()'s own
 * username (there is only ever one possible "user" -- this client's own
 * upstream connection), not looked up in a server-side session table. */
TEST(get_fec_stats_client_mode_success)
{
    memset(&g_cli_info_tmpl, 0, sizeof(g_cli_info_tmpl));
    strncpy(g_cli_info_tmpl.username, "carol", sizeof(g_cli_info_tmpl.username) - 1);
    g_cli_info_rc = 1;

    memset(&g_cli_fec_tmpl, 0, sizeof(g_cli_fec_tmpl));
    g_cli_fec_tmpl.enable_fec = 1;
    g_cli_fec_tmpl.mp_state_label = "single_path";
    g_cli_fec_tmpl.fec_send_cnt = 3;
    g_cli_fec_rc = 1;

    char resp[1024];
    call_dispatch_client("{\"cmd\":\"get_fec_stats\",\"user\":\"carol\"}", resp,
                         sizeof(resp));
    ASSERT_CONTAINS(resp, "\"user\":\"carol\"");
    ASSERT_CONTAINS(resp, "\"mp_state_label\":\"single_path\"");
    ASSERT_CONTAINS(resp, "\"fec_send_cnt\":3");

    g_cli_info_rc = 0;
    g_cli_fec_rc = -1;
}

TEST(get_fec_stats_client_mode_wrong_username)
{
    memset(&g_cli_info_tmpl, 0, sizeof(g_cli_info_tmpl));
    strncpy(g_cli_info_tmpl.username, "carol", sizeof(g_cli_info_tmpl.username) - 1);
    g_cli_info_rc = 1;
    g_cli_fec_rc = 1; /* would succeed if reached -- must not be */

    char resp[512];
    call_dispatch_client("{\"cmd\":\"get_fec_stats\",\"user\":\"mallory\"}", resp,
                         sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");

    g_cli_info_rc = 0;
    g_cli_fec_rc = -1;
}

TEST(get_all_fec_stats_client_mode_one_entry)
{
    memset(&g_cli_info_tmpl, 0, sizeof(g_cli_info_tmpl));
    strncpy(g_cli_info_tmpl.username, "dave", sizeof(g_cli_info_tmpl.username) - 1);
    g_cli_info_rc = 1;

    memset(&g_cli_fec_tmpl, 0, sizeof(g_cli_fec_tmpl));
    g_cli_fec_tmpl.enable_fec = 1;
    g_cli_fec_tmpl.mp_state_label = "active_only";
    g_cli_fec_rc = 1;

    char resp[1024];
    call_dispatch_client("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"n_clients\":1");
    ASSERT_CONTAINS(resp, "\"user\":\"dave\"");
    ASSERT_CONTAINS(resp, "\"mp_state_label\":\"active_only\"");

    g_cli_info_rc = 0;
    g_cli_fec_rc = -1;
}

TEST(get_all_fec_stats_client_mode_not_connected)
{
    g_cli_fec_rc = 0; /* FEC built, but not connected right now */
    char resp[512];
    call_dispatch_client("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true,\"n_clients\":0,\"clients\":[]}");
    g_cli_fec_rc = -1;
}

TEST(get_all_fec_stats_client_mode_not_built)
{
    g_cli_fec_rc = -1;
    char resp[512];
    call_dispatch_client("{\"cmd\":\"get_all_fec_stats\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":false,\"error\":\"fec not built\"}");
}

/* ── get_reorder_stats ───────────────────────────────────────────────────── */

TEST(get_reorder_stats)
{
    g_reorder_rc = 0;
    char resp[2048];
    call_dispatch("{\"cmd\":\"get_reorder_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"reorder\":{");
    ASSERT_CONTAINS(resp, "\"delivered_count\":55");
    ASSERT_CONTAINS(resp, "\"added_latency_p99_ms\":");
}

TEST(get_reorder_stats_internal_error)
{
    /* Failure-branch parity with get_fec_stats / get_all_fec_stats: a negative
     * getter return must surface {"error":"internal error"}, not a malformed
     * or half-built reorder object. */
    g_reorder_rc = -1;
    char resp[512];
    call_dispatch("{\"cmd\":\"get_reorder_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "internal error");
    g_reorder_rc = 0;
}

/* Regression test for the original bug report: on a client-mode router,
 * get_reorder_stats unconditionally called mqvpn_server_get_reorder_stats
 * (cs->server), which is NULL by design in client mode
 * (platform_linux.c's client-mode ctrl_socket_create call passes NULL for
 * server, non-NULL for cli_ctx) -- so every client-mode get_reorder_stats
 * request failed with {"ok":false,"error":"internal error"}, live-confirmed
 * on an OpenMPTCProuter bench (mqvpn 0.16.0) despite the tunnel being up
 * and passing real traffic. mqvpn_client_get_reorder_stats() already
 * existed (wired into the Android/iOS bridges) but was never connected to
 * the Linux control socket -- this pins the fix. */
TEST(get_reorder_stats_client_mode)
{
    memset(&g_cli_reorder_tmpl, 0, sizeof(g_cli_reorder_tmpl));
    g_cli_reorder_tmpl.delivered_count = 77;
    g_cli_reorder_rc = 0;

    char resp[2048];
    call_dispatch_client("{\"cmd\":\"get_reorder_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"reorder\":{");
    ASSERT_CONTAINS(resp, "\"delivered_count\":77");
}

TEST(get_reorder_stats_client_mode_internal_error)
{
    g_cli_reorder_rc = -1;
    char resp[512];
    call_dispatch_client("{\"cmd\":\"get_reorder_stats\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "internal error");
    g_cli_reorder_rc = 0;
}

/* ── path commands (client mode only) ───────────────────────────────────── */

TEST(add_path_success)
{
    g_add_path_rc = 0;
    char resp[256];
    call_dispatch_client("{\"cmd\":\"add_path\",\"iface\":\"eth0\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(add_path_server_mode_rejected)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"add_path\",\"iface\":\"eth0\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "not supported in server mode");
}

TEST(add_path_missing_iface)
{
    char resp[256];
    call_dispatch_client("{\"cmd\":\"add_path\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "iface required");
}

TEST(add_path_platform_failure)
{
    g_add_path_rc = -1;
    char resp[256];
    call_dispatch_client("{\"cmd\":\"add_path\",\"iface\":\"eth0\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "add_path failed");
    g_add_path_rc = 0;
}

TEST(remove_path_success)
{
    g_remove_path_rc = 0;
    char resp[256];
    call_dispatch_client("{\"cmd\":\"remove_path\",\"iface\":\"eth0\"}", resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(remove_path_server_mode_rejected)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"remove_path\",\"iface\":\"eth0\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "not supported in server mode");
}

TEST(remove_path_missing_iface)
{
    char resp[256];
    call_dispatch_client("{\"cmd\":\"remove_path\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "iface required");
}

TEST(set_path_weight_success)
{
    g_set_path_weight_rc = 0;
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_weight\",\"iface\":\"eth0\",\"weight\":3}",
                         resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(set_path_weight_out_of_range)
{
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_weight\",\"iface\":\"eth0\",\"weight\":99999}",
                         resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "weight must be 0-65535");
}

TEST(set_path_weight_missing_weight)
{
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_weight\",\"iface\":\"eth0\"}",
                         resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "weight required");
}

TEST(set_path_dscp_mask_success)
{
    g_set_path_dscp_mask_rc = 0;
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_dscp_mask\",\"iface\":\"eth0\",\"dscp_mask\":3}",
                         resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(set_path_dscp_mask_hex)
{
    g_set_path_dscp_mask_rc = 0;
    char resp[256];
    /* 0x400000000000 == 1ULL << 46 (EF), built via MQVPN_DSCP_BIT(46).
     * Unquoted, like every other numeric field this mini-parser reads
     * (json_find_key() returns the raw text after ':', not a validated
     * JSON number token — see strtoull(..., 0) in the handler). */
    call_dispatch_client(
        "{\"cmd\":\"set_path_dscp_mask\",\"iface\":\"eth0\",\"dscp_mask\":0x400000000000}",
        resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
}

TEST(set_path_dscp_mask_platform_failure)
{
    g_set_path_dscp_mask_rc = -1;
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_dscp_mask\",\"iface\":\"eth0\",\"dscp_mask\":3}",
                         resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "path not found");
    g_set_path_dscp_mask_rc = 0;
}

TEST(set_path_dscp_mask_missing_mask)
{
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_dscp_mask\",\"iface\":\"eth0\"}",
                         resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "dscp_mask required");
}

TEST(set_path_dscp_mask_missing_iface)
{
    char resp[256];
    call_dispatch_client("{\"cmd\":\"set_path_dscp_mask\",\"dscp_mask\":3}",
                         resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "iface required");
}

/* ── set_path_weight / set_path_dscp_mask, SERVER mode ──
 * Keyed by user + path_id instead of iface (control_socket.c doc comment
 * explains why a server has no local-interface concept for a path). */

TEST(set_path_weight_server_success)
{
    g_srv_set_weight_rc = MQVPN_OK;
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_weight\",\"user\":\"alice\",\"path_id\":2,\"weight\":10}",
                  resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
    ASSERT_STR_EQ(g_srv_set_weight_user, "alice");
    if (g_srv_set_weight_path_id != 2 || g_srv_set_weight_weight != 10) {
        printf("FAIL\n    captured path_id/weight mismatch: %" PRIu64 "/%u\n",
               g_srv_set_weight_path_id, g_srv_set_weight_weight);
        exit(1);
    }
}

TEST(set_path_weight_server_user_not_found)
{
    g_srv_set_weight_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_weight\",\"user\":\"nobody\",\"path_id\":0,\"weight\":1}",
                  resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");
    g_srv_set_weight_rc = MQVPN_OK;
}

TEST(set_path_weight_server_path_not_found)
{
    g_srv_set_weight_rc = MQVPN_ERR_ENGINE;
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_weight\",\"user\":\"alice\",\"path_id\":99,\"weight\":1}",
                  resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "path not found");
    g_srv_set_weight_rc = MQVPN_OK;
}

TEST(set_path_weight_server_missing_user)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_weight\",\"path_id\":0,\"weight\":1}", resp,
                  sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user required");
}

TEST(set_path_weight_server_missing_path_id)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_weight\",\"user\":\"alice\",\"weight\":1}", resp,
                  sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "iface or path_id required");
}

TEST(set_path_weight_server_by_iface_success)
{
    g_srv_set_weight_iface_rc = MQVPN_OK;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_weight\",\"user\":\"alice\",\"iface\":\"wlan0\",\"weight\":10}",
        resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
    ASSERT_STR_EQ(g_srv_set_weight_iface_user, "alice");
    ASSERT_STR_EQ(g_srv_set_weight_iface_iface, "wlan0");
    if (g_srv_set_weight_iface_weight != 10) {
        printf("FAIL\n    captured weight mismatch: %u\n", g_srv_set_weight_iface_weight);
        exit(1);
    }
}

TEST(set_path_weight_server_iface_takes_priority_over_path_id)
{
    /* Both given: iface wins, per the doc comment. */
    g_srv_set_weight_iface_rc = MQVPN_OK;
    g_srv_set_weight_rc = MQVPN_OK;
    g_srv_set_weight_path_id = 999; /* pre-dirty: must NOT be touched */
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_weight\",\"user\":\"alice\",\"iface\":\"wlan0\","
        "\"path_id\":2,\"weight\":10}",
        resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
    ASSERT_STR_EQ(g_srv_set_weight_iface_iface, "wlan0");
    if (g_srv_set_weight_path_id != 999) {
        printf("FAIL\n    path_id-keyed setter was called; iface should have taken priority\n");
        exit(1);
    }
}

TEST(set_path_weight_server_by_iface_user_not_found)
{
    g_srv_set_weight_iface_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_weight\",\"user\":\"nobody\",\"iface\":\"wlan0\",\"weight\":1}",
        resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");
    g_srv_set_weight_iface_rc = MQVPN_OK;
}

TEST(set_path_dscp_mask_server_success)
{
    g_srv_set_dscp_mask_rc = MQVPN_OK;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_dscp_mask\",\"user\":\"alice\",\"path_id\":2,"
        "\"dscp_mask\":0x400000000000}",
        resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
    ASSERT_STR_EQ(g_srv_set_dscp_mask_user, "alice");
    if (g_srv_set_dscp_mask_path_id != 2 ||
        g_srv_set_dscp_mask_mask != (1ULL << 46)) {
        printf("FAIL\n    captured path_id/mask mismatch: %" PRIu64 "/%" PRIu64 "\n",
               g_srv_set_dscp_mask_path_id, g_srv_set_dscp_mask_mask);
        exit(1);
    }
}

TEST(set_path_dscp_mask_server_user_not_found)
{
    g_srv_set_dscp_mask_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_dscp_mask\",\"user\":\"nobody\",\"path_id\":0,\"dscp_mask\":0}",
        resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");
    g_srv_set_dscp_mask_rc = MQVPN_OK;
}

TEST(set_path_dscp_mask_server_path_not_found)
{
    g_srv_set_dscp_mask_rc = MQVPN_ERR_ENGINE;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_dscp_mask\",\"user\":\"alice\",\"path_id\":99,\"dscp_mask\":0}",
        resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "path not found");
    g_srv_set_dscp_mask_rc = MQVPN_OK;
}

TEST(set_path_dscp_mask_server_missing_user)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_dscp_mask\",\"path_id\":0,\"dscp_mask\":0}", resp,
                  sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user required");
}

TEST(set_path_dscp_mask_server_missing_path_id)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"set_path_dscp_mask\",\"user\":\"alice\",\"dscp_mask\":0}", resp,
                  sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "iface or path_id required");
}

TEST(set_path_dscp_mask_server_by_iface_success)
{
    g_srv_set_dscp_mask_iface_rc = MQVPN_OK;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_dscp_mask\",\"user\":\"alice\",\"iface\":\"wlan0\","
        "\"dscp_mask\":0x400000000000}",
        resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
    ASSERT_STR_EQ(g_srv_set_dscp_mask_iface_user, "alice");
    ASSERT_STR_EQ(g_srv_set_dscp_mask_iface_iface, "wlan0");
    if (g_srv_set_dscp_mask_iface_mask != (1ULL << 46)) {
        printf("FAIL\n    captured mask mismatch: %" PRIu64 "\n",
               g_srv_set_dscp_mask_iface_mask);
        exit(1);
    }
}

TEST(set_path_dscp_mask_server_iface_takes_priority_over_path_id)
{
    g_srv_set_dscp_mask_iface_rc = MQVPN_OK;
    g_srv_set_dscp_mask_rc = MQVPN_OK;
    g_srv_set_dscp_mask_path_id = 999; /* pre-dirty: must NOT be touched */
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_dscp_mask\",\"user\":\"alice\",\"iface\":\"wlan0\","
        "\"path_id\":2,\"dscp_mask\":3}",
        resp, sizeof(resp));
    ASSERT_STR_EQ(resp, "{\"ok\":true}");
    ASSERT_STR_EQ(g_srv_set_dscp_mask_iface_iface, "wlan0");
    if (g_srv_set_dscp_mask_path_id != 999) {
        printf("FAIL\n    path_id-keyed setter was called; iface should have taken priority\n");
        exit(1);
    }
}

TEST(set_path_dscp_mask_server_by_iface_user_not_found)
{
    g_srv_set_dscp_mask_iface_rc = MQVPN_ERR_INVALID_ARG;
    char resp[256];
    call_dispatch(
        "{\"cmd\":\"set_path_dscp_mask\",\"user\":\"nobody\",\"iface\":\"wlan0\","
        "\"dscp_mask\":0}",
        resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "user not found");
    g_srv_set_dscp_mask_iface_rc = MQVPN_OK;
}

TEST(list_paths_empty)
{
    g_list_paths_n = 0;
    char resp[512];
    call_dispatch_client("{\"cmd\":\"list_paths\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"paths\":[]");
}

TEST(list_paths_two_entries)
{
    g_list_paths_n = 2;
    strncpy(g_list_paths_names[0], "eth0",  IFNAMSIZ - 1);
    strncpy(g_list_paths_names[1], "wlan0", IFNAMSIZ - 1);
    char resp[512];
    call_dispatch_client("{\"cmd\":\"list_paths\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":true");
    ASSERT_CONTAINS(resp, "\"eth0\"");
    ASSERT_CONTAINS(resp, "\"wlan0\"");
    g_list_paths_n = 0;
}

TEST(list_paths_server_mode_rejected)
{
    char resp[256];
    call_dispatch("{\"cmd\":\"list_paths\"}", resp, sizeof(resp));
    ASSERT_CONTAINS(resp, "\"ok\":false");
    ASSERT_CONTAINS(resp, "not supported in server mode");
}

int
main(void)
{
    printf("test_control_socket:\n");

    run_get_all_fec_stats_empty();
    run_get_all_fec_stats_one_entry();
    run_get_all_fec_stats_two_entries();
    run_get_all_fec_stats_fec_not_built();
    run_get_all_fec_stats_client_mode_one_entry();
    run_get_all_fec_stats_client_mode_not_connected();
    run_get_all_fec_stats_client_mode_not_built();
    run_dispatch_missing_cmd();
    run_dispatch_unknown_cmd();

    /* add_user */
    run_add_user_success();
    run_add_user_missing_name();
    run_add_user_missing_key();
    run_add_user_server_failure();
    run_add_user_with_fixed_ip_success();
    run_add_user_with_fixed_ip_failure();

    /* set_user_fixed_ip */
    run_set_user_fixed_ip_success();
    run_set_user_fixed_ip_missing_name();
    run_set_user_fixed_ip_failure();

    /* remove_user */
    run_remove_user_success();
    run_remove_user_missing_name();
    run_remove_user_not_found();

    /* list_users */
    run_list_users_empty();
    run_list_users_two_entries();

    /* stats / status / build info */
    run_get_stats();
    run_get_stats_client_mode_connected();
    run_get_stats_client_mode_not_connected();
    run_get_status_empty();
    run_get_status_one_client_with_path();
    run_get_status_reinject_matched_path_id();
    run_get_status_reinject_mismatched_path_id();
    run_get_status_client_mode_not_connected();
    run_get_status_client_mode_connected_with_path();
    run_get_build_info();
    run_get_build_info_client_mode();

    /* get_fec_stats (per user) */
    run_get_fec_stats_missing_user();
    run_get_fec_stats_user_not_found();
    run_get_fec_stats_not_built();
    run_get_fec_stats_success();
    run_get_fec_stats_client_mode_success();
    run_get_fec_stats_client_mode_wrong_username();

    /* get_reorder_stats */
    run_get_reorder_stats();
    run_get_reorder_stats_internal_error();
    run_get_reorder_stats_client_mode();
    run_get_reorder_stats_client_mode_internal_error();

    /* path commands */
    run_add_path_success();
    run_add_path_server_mode_rejected();
    run_add_path_missing_iface();
    run_add_path_platform_failure();
    run_remove_path_success();
    run_remove_path_server_mode_rejected();
    run_remove_path_missing_iface();
    run_set_path_weight_success();
    run_set_path_weight_out_of_range();
    run_set_path_weight_missing_weight();
    run_set_path_dscp_mask_success();
    run_set_path_dscp_mask_hex();
    run_set_path_dscp_mask_platform_failure();
    run_set_path_dscp_mask_missing_mask();
    run_set_path_dscp_mask_missing_iface();

    /* set_path_weight / set_path_dscp_mask, server mode */
    run_set_path_weight_server_success();
    run_set_path_weight_server_user_not_found();
    run_set_path_weight_server_path_not_found();
    run_set_path_weight_server_missing_user();
    run_set_path_weight_server_missing_path_id();
    run_set_path_weight_server_by_iface_success();
    run_set_path_weight_server_iface_takes_priority_over_path_id();
    run_set_path_weight_server_by_iface_user_not_found();
    run_set_path_dscp_mask_server_success();
    run_set_path_dscp_mask_server_user_not_found();
    run_set_path_dscp_mask_server_path_not_found();
    run_set_path_dscp_mask_server_missing_user();
    run_set_path_dscp_mask_server_missing_path_id();
    run_set_path_dscp_mask_server_by_iface_success();
    run_set_path_dscp_mask_server_iface_takes_priority_over_path_id();
    run_set_path_dscp_mask_server_by_iface_user_not_found();

    run_list_paths_empty();
    run_list_paths_two_entries();
    run_list_paths_server_mode_rejected();

    printf("\n  %d/%d tests passed\n", g_passed, g_run);
    return g_passed == g_run ? 0 : 1;
}
