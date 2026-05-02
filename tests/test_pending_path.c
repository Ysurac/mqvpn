/*
 * test_pending_path.c — Regression tests for issue #4271
 *
 * Bug 1 (library fix in client_activate_path):
 *   A path added after multipath negotiation must not remain PENDING when
 *   xqc_conn_create_path() fails synchronously.  The library now transitions
 *   to DEGRADED + schedules a retry timer.  Full regression coverage requires
 *   a live client+server (e2e); the tests here verify the pre-condition states
 *   and the path-status API used by the e2e assertions.
 *
 * Bug 2 (library fix in tick_reconnect):
 *   mqvpn_rotate_primary_path() was never called; reconnect always hammered
 *   the same dead path.  The library now rotates before each reconnect attempt,
 *   skipping paths with active=0 (fd gone).  These unit tests verify the
 *   slot-lifecycle semantics that make the rotation safe.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "libmqvpn.h"
#include "mqvpn_internal.h"

/* ── Test infrastructure ── */

static int g_run    = 0;
static int g_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void)   \
    {                              \
        g_run++;                   \
        printf("  %-68s ", #name); \
        test_##name();             \
        g_passed++;                \
        printf("PASS\n");          \
    }                              \
    static void test_##name(void)

#define ASSERT_EQ(a, b)                                                                      \
    do {                                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                                  \
        if (_a != _b) {                                                                      \
            printf("FAIL\n    %s:%d  %s == %lld, expected %lld\n", __FILE__, __LINE__, #a,   \
                   _a, _b);                                                                  \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

#define ASSERT_NE(a, b)                                                                      \
    do {                                                                                     \
        long long _a = (long long)(a), _b = (long long)(b);                                  \
        if (_a == _b) {                                                                      \
            printf("FAIL\n    %s:%d  %s == %lld (unexpected equal)\n", __FILE__, __LINE__,   \
                   #a, _a);                                                                  \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

#define ASSERT_NOT_NULL(a)                                                               \
    do {                                                                                 \
        if ((a) == NULL) {                                                               \
            printf("FAIL\n    %s:%d  %s is NULL\n", __FILE__, __LINE__, #a);             \
            exit(1);                                                                     \
        }                                                                                \
    } while (0)

/* ── Mock callbacks ── */

static void dummy_tun_output(const uint8_t *p, size_t l, void *u) { (void)p;(void)l;(void)u; }
static void dummy_config_ready(const mqvpn_tunnel_info_t *i, void *u) { (void)i;(void)u; }

static mqvpn_client_t *
make_test_client(void)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_server(cfg, "127.0.0.1", 443);

    mqvpn_client_callbacks_t cbs = MQVPN_CLIENT_CALLBACKS_INIT;
    cbs.tun_output          = dummy_tun_output;
    cbs.tunnel_config_ready = dummy_config_ready;

    mqvpn_client_t *c = mqvpn_client_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    return c;
}

/* Return the status of path h, or -1 if not found. */
static int
path_status(mqvpn_client_t *c, mqvpn_path_handle_t h)
{
    mqvpn_path_info_t info[MQVPN_MAX_PATHS];
    int n = 0;
    if (mqvpn_client_get_paths(c, info, MQVPN_MAX_PATHS, &n) != MQVPN_OK) return -1;
    for (int i = 0; i < n; i++)
        if (info[i].handle == h) return (int)info[i].status;
    return -1;
}

/* Return total paths visible via get_paths(). */
static int
count_paths(mqvpn_client_t *c)
{
    mqvpn_path_info_t info[MQVPN_MAX_PATHS];
    int n = 0;
    mqvpn_client_get_paths(c, info, MQVPN_MAX_PATHS, &n);
    return n;
}

static mqvpn_path_desc_t
make_desc(int fd, const char *iface, uint32_t flags)
{
    mqvpn_path_desc_t d = {0};
    d.struct_size = sizeof(d);
    d.fd = fd;
    d.flags = flags;
    snprintf(d.iface, sizeof(d.iface), "%s", iface);
    return d;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 1: Bug 1 pre-conditions — path status before connection
 *
 * Full regression for Bug 1 (client_activate_path() → DEGRADED on failure)
 * requires a live connection and is covered by the e2e test suite.  These
 * unit tests verify the API surface used by those assertions.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Newly added path starts PENDING; no connection exists yet. */
TEST(path_starts_pending_before_connect)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(10, "eth0", 0);
    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 10, &d);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/* Multiple paths all start PENDING. */
TEST(two_paths_both_pending_before_connect)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d0 = make_desc(10, "eth0", 0);
    mqvpn_path_desc_t d1 = make_desc(11, "usb0", 0);
    mqvpn_path_handle_t h0 = mqvpn_client_add_path_fd(c, 10, &d0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 11, &d1);
    ASSERT_NE(h0, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h0), MQVPN_PATH_PENDING);
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/* get_paths() correctly reports PENDING status string. */
TEST(pending_status_string)
{
    ASSERT_EQ(strcmp(mqvpn_path_status_string(MQVPN_PATH_PENDING), "pending"), 0);
    ASSERT_EQ(strcmp(mqvpn_path_status_string(MQVPN_PATH_DEGRADED), "degraded"), 0);
    ASSERT_EQ(strcmp(mqvpn_path_status_string(MQVPN_PATH_CLOSED), "closed"), 0);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 2: Bug 2 — path removal prevents recovery
 *
 * remove_path() and drop_path() both set active=0, which prevents
 * reactivate_path() from recovering the slot.  These tests verify the
 * slot-lifecycle contract that tick_reconnect()'s rotation relies on.
 * ═══════════════════════════════════════════════════════════════════════════ */

/* remove_path() transitions status to CLOSED. */
TEST(remove_path_marks_closed)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(20, "usb1", 0);
    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 20, &d);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    ASSERT_EQ(mqvpn_client_remove_path(c, h), MQVPN_OK);
    ASSERT_EQ(path_status(c, h), MQVPN_PATH_CLOSED);

    mqvpn_client_destroy(c);
}

/*
 * After remove_path(), reactivate_path() must fail (active=0).
 * This documents the root cause of Bug 2: once removed, the path cannot
 * be recovered through the reactivation API — only by re-adding it via
 * add_path_fd() with a new socket.
 */
TEST(reactivate_fails_after_remove)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(20, "usb1", 0);
    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 20, &d);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    ASSERT_EQ(mqvpn_client_remove_path(c, h), MQVPN_OK);
    ASSERT_NE(mqvpn_client_reactivate_path(c, h), MQVPN_OK);

    mqvpn_client_destroy(c);
}

/*
 * The CLOSED slot left by remove_path() is reusable by add_path_fd().
 * This is the correct recovery path for Bug 2: the platform re-adds
 * the interface (new socket fd) once connectivity is restored.
 */
TEST(closed_slot_reused_by_add_path_fd)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(20, "usb1", 0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 20, &d);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(count_paths(c), 1);

    ASSERT_EQ(mqvpn_client_remove_path(c, h1), MQVPN_OK);

    mqvpn_path_desc_t d2 = make_desc(21, "usb1", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 21, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h2, h1);               /* new handle */
    ASSERT_EQ(count_paths(c), 1);    /* slot reused, count unchanged */
    ASSERT_EQ(path_status(c, h2), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/*
 * Scenario from issue #4271 Bug 2:
 *   path0 connected, path1 added (PENDING or DEGRADED with Bug 1 fix).
 *   Platform removes path1 due to no-internet detection.
 *   path0 then loses internet.
 *   Result: only path0 remains (PENDING), no fallback path.
 *
 * The fix in tick_reconnect() rotates primary_path_idx away from path0 on
 * the next reconnect attempt, but only finds path1 usable if its fd is still
 * alive (active=1).  Removing path1 via remove_path() (active=0) means
 * the rotation skips it, and reconnect must re-add via add_path_fd().
 */
TEST(remove_pending_path_leaves_only_dead_primary)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d0 = make_desc(10, "eth0", 0);
    mqvpn_path_desc_t d1 = make_desc(11, "usb1", 0);
    mqvpn_path_handle_t h0 = mqvpn_client_add_path_fd(c, 10, &d0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 11, &d1);
    ASSERT_NE(h0, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);

    /* Platform removes path1 (PENDING, no internet). */
    ASSERT_EQ(mqvpn_client_remove_path(c, h1), MQVPN_OK);

    /* path1 slot is CLOSED+inactive; path0 is still PENDING.
     * get_paths() still returns both slots (n_paths never decrements),
     * so we check status rather than count.  If path0 also loses internet
     * the client is stuck until the platform re-adds path1 via add_path_fd(). */
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_CLOSED);
    ASSERT_EQ(path_status(c, h0), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/* drop_path() has the same active=0 semantics. */
TEST(reactivate_fails_after_drop)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(30, "wlan0", 0);
    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 30, &d);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    ASSERT_EQ(mqvpn_client_drop_path(c, h), MQVPN_OK);
    ASSERT_EQ(path_status(c, h), MQVPN_PATH_CLOSED);
    ASSERT_NE(mqvpn_client_reactivate_path(c, h), MQVPN_OK);

    mqvpn_client_destroy(c);
}

/* Removing the last path is allowed by the library (guard is in platform code). */
TEST(remove_last_path_allowed)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(10, "eth0", 0);
    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 10, &d);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    ASSERT_EQ(mqvpn_client_remove_path(c, h), MQVPN_OK);
    /* remove_path() marks the slot CLOSED but does not decrement n_paths;
     * the slot is reusable by the next add_path_fd() call. */
    ASSERT_EQ(path_status(c, h), MQVPN_PATH_CLOSED);

    mqvpn_client_destroy(c);
}

/* Slot freed by remove can accommodate a path up to MQVPN_MAX_PATHS total. */
TEST(max_paths_with_slot_reuse)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_handle_t handles[MQVPN_MAX_PATHS];
    for (int i = 0; i < MQVPN_MAX_PATHS; i++) {
        mqvpn_path_desc_t d = make_desc(10 + i, "ethX", 0);
        snprintf(d.iface, sizeof(d.iface), "eth%d", i);
        handles[i] = mqvpn_client_add_path_fd(c, 10 + i, &d);
        ASSERT_NE(handles[i], (mqvpn_path_handle_t)-1);
    }
    ASSERT_EQ(count_paths(c), MQVPN_MAX_PATHS);

    /* Overflow without removal must fail. */
    mqvpn_path_desc_t overflow = make_desc(99, "overflow", 0);
    ASSERT_EQ(mqvpn_client_add_path_fd(c, 99, &overflow), (mqvpn_path_handle_t)-1);

    /* Free one slot, then a new path fits. */
    ASSERT_EQ(mqvpn_client_remove_path(c, handles[0]), MQVPN_OK);
    mqvpn_path_desc_t d2 = make_desc(99, "eth_new", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 99, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(count_paths(c), MQVPN_MAX_PATHS);

    mqvpn_client_destroy(c);
}

/* ── Runner ── */

int
main(void)
{
    printf("test_pending_path  (issue #4271)\n");

    printf("\n  Bug 1 pre-conditions (path status API)\n");
    run_path_starts_pending_before_connect();
    run_two_paths_both_pending_before_connect();
    run_pending_status_string();

    printf("\n  Bug 2: path removal / slot lifecycle\n");
    run_remove_path_marks_closed();
    run_reactivate_fails_after_remove();
    run_closed_slot_reused_by_add_path_fd();
    run_remove_pending_path_leaves_only_dead_primary();
    run_reactivate_fails_after_drop();
    run_remove_last_path_allowed();
    run_max_paths_with_slot_reuse();

    printf("\n%d/%d tests passed\n", g_passed, g_run);
    return (g_passed == g_run) ? 0 : 1;
}
