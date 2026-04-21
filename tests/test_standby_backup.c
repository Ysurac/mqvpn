/*
 * test_standby_backup.c — Unit tests for xquic-standby backup path behaviour.
 *
 * Tests the path_event callback sequence and path state machine for backup
 * paths.  Before a connection is established the paths sit in PENDING; the
 * deeper failover transitions (mark_standby / mark_available) are exercised
 * end-to-end by ci_bench_standby_backup.sh.
 *
 * No xquic engine is started — all tests use make_test_client() which creates
 * a client without calling connect().
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
        printf("  %-60s ", #name); \
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

#define ASSERT_NOT_NULL(a)                                                             \
    do {                                                                               \
        if ((a) == NULL) {                                                             \
            printf("FAIL\n    %s:%d  %s is NULL\n", __FILE__, __LINE__, #a);           \
            exit(1);                                                                   \
        }                                                                              \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                                  \
    do {                                                                                     \
        if (strcmp((a), (b)) != 0) {                                                         \
            printf("FAIL\n    %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__, (a), (b));     \
            exit(1);                                                                         \
        }                                                                                    \
    } while (0)

/* ── Mock callbacks ── */

static int                   g_path_event_count;
static mqvpn_path_handle_t   g_last_path_handle;
static mqvpn_path_status_t   g_last_path_status;

/* Record the last N path events so ordering can be checked */
#define MAX_RECORDED_EVENTS 16
static mqvpn_path_handle_t   g_event_handle[MAX_RECORDED_EVENTS];
static mqvpn_path_status_t   g_event_status[MAX_RECORDED_EVENTS];

static void
mock_path_event(mqvpn_path_handle_t handle, mqvpn_path_status_t status, void *ctx)
{
    (void)ctx;
    g_last_path_handle = handle;
    g_last_path_status = status;
    if (g_path_event_count < MAX_RECORDED_EVENTS) {
        g_event_handle[g_path_event_count] = handle;
        g_event_status[g_path_event_count] = status;
    }
    g_path_event_count++;
}

static void reset_events(void)
{
    g_path_event_count = 0;
    g_last_path_handle = -1;
    g_last_path_status = MQVPN_PATH_PENDING;
    memset(g_event_handle, -1, sizeof(g_event_handle));
    memset(g_event_status, 0, sizeof(g_event_status));
}

static void
dummy_tun_output(const uint8_t *p, size_t l, void *u)
{
    (void)p; (void)l; (void)u;
}

static void
dummy_config_ready(const mqvpn_tunnel_info_t *i, void *u)
{
    (void)i; (void)u;
}

static mqvpn_client_t *
make_client_with_events(void)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    mqvpn_config_set_server(cfg, "1.2.3.4", 443);

    mqvpn_client_callbacks_t cbs = MQVPN_CLIENT_CALLBACKS_INIT;
    cbs.tun_output         = dummy_tun_output;
    cbs.tunnel_config_ready = dummy_config_ready;
    cbs.path_event         = mock_path_event;

    mqvpn_client_t *c = mqvpn_client_new(cfg, &cbs, NULL);
    mqvpn_config_free(cfg);
    return c;
}

/* ── Helpers ── */

/* Find path info by handle; returns 1 on success. */
static int
find_path_info(mqvpn_client_t *c, mqvpn_path_handle_t h, mqvpn_path_info_t *out)
{
    mqvpn_path_info_t infos[8];
    int n = 0;
    if (mqvpn_client_get_paths(c, infos, 8, &n) != MQVPN_OK) return 0;
    for (int i = 0; i < n; i++) {
        if (infos[i].handle == h) { *out = infos[i]; return 1; }
    }
    return 0;
}

/* ── Tests: backup flag persistence ── */

TEST(backup_flag_stored_in_path_info)
{
    mqvpn_client_t *c = make_client_with_events();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t desc = {0};
    desc.fd    = 50;
    desc.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(desc.iface, sizeof(desc.iface), "lte0");

    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 50, &desc);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.flags & MQVPN_PATH_FLAG_BACKUP, (long long)MQVPN_PATH_FLAG_BACKUP);
    ASSERT_STR_EQ(info.name, "lte0");

    mqvpn_client_destroy(c);
}

TEST(primary_path_has_no_backup_flag)
{
    mqvpn_client_t *c = make_client_with_events();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t desc = {0};
    desc.fd = 10;
    snprintf(desc.iface, sizeof(desc.iface), "eth0");

    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 10, &desc);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.flags & MQVPN_PATH_FLAG_BACKUP, (long long)0);

    mqvpn_client_destroy(c);
}

TEST(backup_flag_constant_is_bit0)
{
    ASSERT_EQ(MQVPN_PATH_FLAG_BACKUP, (long long)1);
}

/* ── Tests: initial status before connection ── */

TEST(backup_path_status_pending_before_connect)
{
    /* Before connect() / multipath negotiation, every path (primary or backup)
     * must sit in PENDING — no xquic path exists yet. */
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd    = 52;
    desc.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(desc.iface, sizeof(desc.iface), "lte0");

    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 52, &desc);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.status, MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

TEST(primary_path_status_pending_before_connect)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd = 11;
    snprintf(desc.iface, sizeof(desc.iface), "eth0");

    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 11, &desc);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.status, MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/* ── Tests: mixed primary + backup ── */

TEST(mixed_primary_backup_flags_independent)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t dp = {0}, db = {0};
    dp.fd = 60; snprintf(dp.iface, sizeof(dp.iface), "eth0");
    db.fd = 61; db.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(db.iface, sizeof(db.iface), "lte0");

    mqvpn_path_handle_t hp = mqvpn_client_add_path_fd(c, 60, &dp);
    mqvpn_path_handle_t hb = mqvpn_client_add_path_fd(c, 61, &db);
    ASSERT_NE(hp, (mqvpn_path_handle_t)-1);
    ASSERT_NE(hb, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t infop, infob;
    ASSERT_EQ(find_path_info(c, hp, &infop), 1);
    ASSERT_EQ(find_path_info(c, hb, &infob), 1);

    ASSERT_EQ(infop.flags & MQVPN_PATH_FLAG_BACKUP, (long long)0);
    ASSERT_EQ(infob.flags & MQVPN_PATH_FLAG_BACKUP, (long long)MQVPN_PATH_FLAG_BACKUP);

    mqvpn_client_destroy(c);
}

TEST(two_backup_paths_both_reported)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t db1 = {0}, db2 = {0};
    db1.fd = 70; db1.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(db1.iface, sizeof(db1.iface), "lte0");
    db2.fd = 71; db2.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(db2.iface, sizeof(db2.iface), "lte1");

    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 70, &db1);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 71, &db2);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t infos[8];
    int n = 0;
    ASSERT_EQ(mqvpn_client_get_paths(c, infos, 8, &n), MQVPN_OK);
    ASSERT_EQ(n, 2);
    /* Both must carry the backup flag */
    ASSERT_EQ(infos[0].flags & MQVPN_PATH_FLAG_BACKUP, (long long)MQVPN_PATH_FLAG_BACKUP);
    ASSERT_EQ(infos[1].flags & MQVPN_PATH_FLAG_BACKUP, (long long)MQVPN_PATH_FLAG_BACKUP);

    mqvpn_client_destroy(c);
}

/* ── Tests: path_event callback ── */

TEST(no_path_event_before_connect)
{
    /* Adding a backup path before connect must NOT fire path_event
     * (the library has no connection yet, so no status to report). */
    mqvpn_client_t *c = make_client_with_events();
    reset_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd    = 80;
    desc.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(desc.iface, sizeof(desc.iface), "lte0");

    mqvpn_client_add_path_fd(c, 80, &desc);

    ASSERT_EQ(g_path_event_count, 0);

    mqvpn_client_destroy(c);
}

TEST(path_event_count_zero_for_primary_before_connect)
{
    mqvpn_client_t *c = make_client_with_events();
    reset_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd = 12;
    snprintf(desc.iface, sizeof(desc.iface), "eth0");
    mqvpn_client_add_path_fd(c, 12, &desc);

    ASSERT_EQ(g_path_event_count, 0);

    mqvpn_client_destroy(c);
}

/* ── Tests: remove / reuse ── */

TEST(backup_path_removable)
{
    /* remove_path returns OK and marks the path CLOSED.
     * The slot stays in get_paths output (n_paths is not decremented). */
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd    = 90;
    desc.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(desc.iface, sizeof(desc.iface), "lte0");

    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 90, &desc);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    ASSERT_EQ(mqvpn_client_remove_path(c, h), MQVPN_OK);

    /* Slot stays in the array but is marked CLOSED */
    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.status, MQVPN_PATH_CLOSED);

    mqvpn_client_destroy(c);
}

TEST(backup_path_slot_reusable_after_remove)
{
    mqvpn_client_t *c = make_client_with_events();

    /* Fill all 4 slots — mix primary + backup */
    mqvpn_path_desc_t dp = {0}, db = {0};
    dp.fd = 100;
    db.fd = 101; db.flags = MQVPN_PATH_FLAG_BACKUP;

    mqvpn_path_handle_t hp = mqvpn_client_add_path_fd(c, 100, &dp);
    mqvpn_path_handle_t hb = mqvpn_client_add_path_fd(c, 101, &db);
    mqvpn_client_add_path_fd(c, 102, NULL);
    mqvpn_client_add_path_fd(c, 103, NULL);

    /* 5th should fail */
    ASSERT_EQ(mqvpn_client_add_path_fd(c, 104, NULL), (mqvpn_path_handle_t)-1);

    /* Remove backup, then adding one more should succeed */
    ASSERT_EQ(mqvpn_client_remove_path(c, hb), MQVPN_OK);
    mqvpn_path_handle_t h_new = mqvpn_client_add_path_fd(c, 105, NULL);
    ASSERT_NE(h_new, (mqvpn_path_handle_t)-1);

    (void)hp;
    mqvpn_client_destroy(c);
}

/* ── Tests: get_paths includes backup paths in count ── */

TEST(get_paths_counts_backup_paths)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t dp = {0}, db = {0};
    dp.fd = 200;
    db.fd = 201; db.flags = MQVPN_PATH_FLAG_BACKUP;

    mqvpn_client_add_path_fd(c, 200, &dp);
    mqvpn_client_add_path_fd(c, 201, &db);

    mqvpn_path_info_t infos[4];
    int n = 0;
    ASSERT_EQ(mqvpn_client_get_paths(c, infos, 4, &n), MQVPN_OK);
    ASSERT_EQ(n, 2);

    mqvpn_client_destroy(c);
}

TEST(get_paths_truncates_to_max_out)
{
    mqvpn_client_t *c = make_client_with_events();

    /* Add 3 paths (1 primary + 2 backup) */
    mqvpn_path_desc_t d = {0};
    d.fd = 300;
    mqvpn_client_add_path_fd(c, 300, &d);
    d.fd = 301; d.flags = MQVPN_PATH_FLAG_BACKUP;
    mqvpn_client_add_path_fd(c, 301, &d);
    d.fd = 302;
    mqvpn_client_add_path_fd(c, 302, &d);

    /* Request only 2 slots */
    mqvpn_path_info_t infos[2];
    int n = 0;
    ASSERT_EQ(mqvpn_client_get_paths(c, infos, 2, &n), MQVPN_OK);
    ASSERT_EQ(n, 2); /* truncated */

    mqvpn_client_destroy(c);
}

/* ── Tests: tick is safe with backup paths registered ── */

TEST(tick_safe_with_backup_paths_no_connection)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd    = 400;
    desc.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(desc.iface, sizeof(desc.iface), "lte0");
    mqvpn_client_add_path_fd(c, 400, &desc);

    /* tick() must not crash without a live connection */
    ASSERT_EQ(mqvpn_client_tick(c), MQVPN_OK);
    ASSERT_EQ(mqvpn_client_tick(c), MQVPN_OK);

    mqvpn_client_destroy(c);
}

TEST(tick_safe_mixed_paths_no_connection)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t dp = {0}, db = {0};
    dp.fd = 500;
    db.fd = 501; db.flags = MQVPN_PATH_FLAG_BACKUP;
    mqvpn_client_add_path_fd(c, 500, &dp);
    mqvpn_client_add_path_fd(c, 501, &db);

    ASSERT_EQ(mqvpn_client_tick(c), MQVPN_OK);

    mqvpn_client_destroy(c);
}

/* ── Tests: drop_path on backup path ── */

TEST(drop_path_on_backup)
{
    mqvpn_client_t *c = make_client_with_events();

    mqvpn_path_desc_t desc = {0};
    desc.fd    = 600;
    desc.flags = MQVPN_PATH_FLAG_BACKUP;
    snprintf(desc.iface, sizeof(desc.iface), "lte0");

    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 600, &desc);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    /* drop_path does an immediate remove (fd is dead) — must not crash */
    ASSERT_EQ(mqvpn_client_drop_path(c, h), MQVPN_OK);

    /* Slot stays in the array but is marked CLOSED */
    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.status, MQVPN_PATH_CLOSED);

    mqvpn_client_destroy(c);
}

/* ── Tests: null safety for backup paths ── */

TEST(add_path_null_desc_still_works)
{
    /* NULL desc is allowed — path gets default flags (no BACKUP) */
    mqvpn_client_t *c = make_client_with_events();
    mqvpn_path_handle_t h = mqvpn_client_add_path_fd(c, 700, NULL);
    ASSERT_NE(h, (mqvpn_path_handle_t)-1);

    mqvpn_path_info_t info;
    ASSERT_EQ(find_path_info(c, h, &info), 1);
    ASSERT_EQ(info.flags & MQVPN_PATH_FLAG_BACKUP, (long long)0);

    mqvpn_client_destroy(c);
}

/* ── Runner ── */

int
main(void)
{
    printf("test_standby_backup\n");

    run_backup_flag_stored_in_path_info();
    run_primary_path_has_no_backup_flag();
    run_backup_flag_constant_is_bit0();
    run_backup_path_status_pending_before_connect();
    run_primary_path_status_pending_before_connect();
    run_mixed_primary_backup_flags_independent();
    run_two_backup_paths_both_reported();
    run_no_path_event_before_connect();
    run_path_event_count_zero_for_primary_before_connect();
    run_backup_path_removable();
    run_backup_path_slot_reusable_after_remove();
    run_get_paths_counts_backup_paths();
    run_get_paths_truncates_to_max_out();
    run_tick_safe_with_backup_paths_no_connection();
    run_tick_safe_mixed_paths_no_connection();
    run_drop_path_on_backup();
    run_add_path_null_desc_still_works();

    printf("\n%d/%d tests passed\n", g_passed, g_run);
    return (g_passed == g_run) ? 0 : 1;
}
