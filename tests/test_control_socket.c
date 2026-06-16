/*
 * test_control_socket.c — Unit tests for control_socket.c dispatch()
 *
 * Covers the get_all_fec_stats branch which had a missing return value when
 * truncated == 0.  Also tests the error paths: FEC not built, unknown cmd,
 * missing cmd.
 *
 * Includes control_socket.c directly (same technique as test_status.c) and
 * provides lightweight stubs for all external API dependencies.  Links only
 * libevent (needed by the non-dispatch socket machinery in control_socket.c).
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

/* ── Stub state used by mqvpn_server_get_all_fec_stats ──────────────────── */
static mqvpn_internal_fec_entry_t g_fec_entries[MQVPN_MAX_USERS];
static int g_fec_n = 0; /* entry count; -1 = FEC not built */

/* ── Server API stubs ────────────────────────────────────────────────────── */
int mqvpn_server_add_user(mqvpn_server_t *s, const char *n, const char *k)
{ (void)s; (void)n; (void)k; return MQVPN_OK; }

int mqvpn_server_set_user_fixed_ip(mqvpn_server_t *s, const char *n, const char *ip)
{ (void)s; (void)n; (void)ip; return MQVPN_OK; }

int mqvpn_server_remove_user(mqvpn_server_t *s, const char *n)
{ (void)s; (void)n; return MQVPN_OK; }

int mqvpn_server_list_users(const mqvpn_server_t *s, char names[][64], int max)
{ (void)s; (void)names; (void)max; return 0; }

int mqvpn_server_get_stats(const mqvpn_server_t *s, mqvpn_stats_t *st)
{ (void)s; (void)st; return MQVPN_OK; }

int mqvpn_server_get_n_clients(const mqvpn_server_t *s) { (void)s; return 0; }

uint64_t mqvpn_server_uptime_seconds(const mqvpn_server_t *s) { (void)s; return 0; }

int mqvpn_server_get_client_info(const mqvpn_server_t *s,
                                  mqvpn_client_info_t *out, int max, int *n)
{ (void)s; (void)out; (void)max; *n = 0; return MQVPN_OK; }

const char *mqvpn_version_string(void) { return "test"; }

const char *mqvpn_server_scheduler_label(const mqvpn_server_t *s)
{ (void)s; return "none"; }

const char *mqvpn_path_state_label(int state) { (void)state; return "unknown"; }

int mqvpn_server_get_client_fec_stats(const mqvpn_server_t *s,
                                       const char *user,
                                       mqvpn_internal_fec_stats_t *out)
{ (void)s; (void)user; (void)out; return -1; }

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
{ (void)s; if (out) memset(out, 0, sizeof(*out)); return 0; }

/* ── Platform stubs ─────────────────────────────────────────────────────── */
#include "platform_internal.h"

int platform_add_path(platform_ctx_t *p, const char *iface, int backup)
{ (void)p; (void)iface; (void)backup; return 0; }

int platform_remove_path(platform_ctx_t *p, const char *iface)
{ (void)p; (void)iface; return 0; }

int platform_list_paths(platform_ctx_t *p, char names[][IFNAMSIZ], int max)
{ (void)p; (void)names; (void)max; return 0; }

int platform_set_path_weight(platform_ctx_t *p, const char *iface, uint32_t weight)
{ (void)p; (void)iface; (void)weight; return 0; }

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

static int
call_dispatch(const char *req, char *resp, size_t resp_len)
{
    return dispatch(req, resp, resp_len, (mqvpn_server_t *)NULL, NULL);
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

int
main(void)
{
    printf("test_control_socket:\n");

    run_get_all_fec_stats_empty();
    run_get_all_fec_stats_one_entry();
    run_get_all_fec_stats_two_entries();
    run_get_all_fec_stats_fec_not_built();
    run_dispatch_missing_cmd();
    run_dispatch_unknown_cmd();

    printf("\n  %d/%d tests passed\n", g_passed, g_run);
    return g_passed == g_run ? 0 : 1;
}
