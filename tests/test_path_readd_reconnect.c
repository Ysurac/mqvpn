/*
 * test_path_readd_reconnect.c — Regression tests for issue #4276
 *
 * Bug: when usb1 bounces repeatedly, each bounce consumes one xquic path-creation
 * budget slot (XQC_MAX_PATHS_COUNT=8 per connection).  After budget exhaustion a
 * forced reconnect is triggered, but because usb1 is already physically UP after
 * the reconnect, no new netlink event fires to re-trigger try_readd_removed_path().
 * The path is stranded.
 *
 * These unit tests verify the library-level slot lifecycle that the reconnect
 * recovery path depends on:
 *   - drop_path() + add_path_fd() (RTM_DELLINK → RTM_NEWADDR) gives a clean PENDING slot
 *   - remove_path() + add_path_fd() (try_readd_removed_path undo → retry) reuses the slot
 *   - Many repeated bounce cycles do not exhaust the path slot array
 *   - A CLOSED path (retries exhausted) is re-addable after reconnect
 *
 * Full reconnect-triggered re-add (the missing piece after budget exhaustion)
 * requires a live client+server and is covered by the e2e test suite.
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

/* ── Helpers ── */

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
 * Group 1: RTM_DELLINK → RTM_NEWADDR (drop_path + re-add)
 *
 * When the kernel deletes the usb1 interface (RTM_DELLINK), the platform calls
 * drop_path() — which closes the slot without sending a QUIC close frame.
 * When the interface comes back (RTM_NEWADDR), try_readd_removed_path() creates
 * a fresh socket and calls add_path_fd().  These tests verify that slot is
 * correctly reused and starts in a clean PENDING state.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * RTM_DELLINK → RTM_NEWADDR: drop then re-add starts PENDING.
 * This is the basic contract for the usb1 bounce recovery path.
 */
TEST(drop_then_readd_starts_pending)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(10, "usb1", 0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 10, &d);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_PENDING);

    /* Interface disappears (RTM_DELLINK → drop_path) */
    ASSERT_EQ(mqvpn_client_drop_path(c, h1), MQVPN_OK);
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_CLOSED);

    /* Interface reappears (RTM_NEWADDR → try_readd_removed_path → add_path_fd) */
    mqvpn_path_desc_t d2 = make_desc(11, "usb1", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 11, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h2), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/*
 * drop_path + re-add reuses the slot; path count must not grow.
 * A usb1 bounce must not consume an extra slot each time.
 */
TEST(drop_then_readd_reuses_slot)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(10, "usb1", 0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 10, &d);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(count_paths(c), 1);

    ASSERT_EQ(mqvpn_client_drop_path(c, h1), MQVPN_OK);

    mqvpn_path_desc_t d2 = make_desc(11, "usb1", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 11, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h2, h1);          /* new handle */
    ASSERT_EQ(count_paths(c), 1); /* slot reused, count unchanged */

    mqvpn_client_destroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 2: try_readd_removed_path undo → retry (remove_path + re-add)
 *
 * When activation fails (path stays PENDING), try_readd_removed_path() calls
 * remove_path() to undo the add and schedules a retry on the next netlink
 * event.  These tests verify that remove_path + add_path_fd correctly reuses
 * the slot and produces a clean PENDING entry.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * undo (remove_path) + retry (add_path_fd) gives a fresh PENDING slot.
 * This is the try_readd_removed_path retry contract.
 */
TEST(remove_then_readd_starts_pending)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(20, "usb1", 0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 20, &d);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);

    /* Undo: activation failed, try_readd_removed_path calls remove_path */
    ASSERT_EQ(mqvpn_client_remove_path(c, h1), MQVPN_OK);
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_CLOSED);

    /* Retry: next netlink event triggers a new add_path_fd */
    mqvpn_path_desc_t d2 = make_desc(21, "usb1", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 21, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h2), MQVPN_PATH_PENDING);
    ASSERT_EQ(count_paths(c), 1); /* slot reused */

    mqvpn_client_destroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 3: Repeated bounce cycles (the core issue #4276 scenario)
 *
 * usb1 goes down and up repeatedly.  Each cycle must reuse the same slot.
 * The path count must stay constant and each new entry must start PENDING.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * 8 drop+readd cycles on usb1 with eth0 present.
 * Path count must stay at 2; each new usb1 handle must be PENDING.
 *
 * 8 cycles is chosen because XQC_MAX_PATHS_COUNT=8 — the xquic budget that
 * gets exhausted in issue #4276.  The slot array must not fill up regardless.
 */
TEST(eight_bounces_slot_count_stable)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d0 = make_desc(10, "eth0", 0);
    mqvpn_path_handle_t h_eth0 = mqvpn_client_add_path_fd(c, 10, &d0);
    ASSERT_NE(h_eth0, (mqvpn_path_handle_t)-1);

    mqvpn_path_desc_t d1 = make_desc(11, "usb1", 0);
    mqvpn_path_handle_t h_usb1 = mqvpn_client_add_path_fd(c, 11, &d1);
    ASSERT_NE(h_usb1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(count_paths(c), 2);

    for (int bounce = 0; bounce < 8; bounce++) {
        /* usb1 goes down (RTM_DELLINK) */
        ASSERT_EQ(mqvpn_client_drop_path(c, h_usb1), MQVPN_OK);
        ASSERT_EQ(path_status(c, h_usb1), MQVPN_PATH_CLOSED);

        /* usb1 comes back (RTM_NEWADDR → add_path_fd with fresh fd) */
        int new_fd = 100 + bounce;
        mqvpn_path_desc_t d = make_desc(new_fd, "usb1", 0);
        h_usb1 = mqvpn_client_add_path_fd(c, new_fd, &d);
        ASSERT_NE(h_usb1, (mqvpn_path_handle_t)-1);
        ASSERT_EQ(path_status(c, h_usb1), MQVPN_PATH_PENDING);
        /* Slot count must not grow — each bounce reuses the same slot */
        ASSERT_EQ(count_paths(c), 2);
    }

    /* eth0 must be unaffected by all the usb1 bounces */
    ASSERT_EQ(path_status(c, h_eth0), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/*
 * 8 remove+readd cycles (try_readd_removed_path undo pattern).
 * Same as above but using remove_path instead of drop_path, exercising
 * the undo path that fires when xqc_conn_create_path() fails synchronously.
 */
TEST(eight_undo_retry_cycles_slot_count_stable)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d0 = make_desc(10, "eth0", 0);
    mqvpn_path_handle_t h_eth0 = mqvpn_client_add_path_fd(c, 10, &d0);
    ASSERT_NE(h_eth0, (mqvpn_path_handle_t)-1);

    mqvpn_path_desc_t d1 = make_desc(11, "usb1", 0);
    mqvpn_path_handle_t h_usb1 = mqvpn_client_add_path_fd(c, 11, &d1);
    ASSERT_NE(h_usb1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(count_paths(c), 2);

    for (int i = 0; i < 8; i++) {
        /* Undo (activation failed → remove_path) */
        ASSERT_EQ(mqvpn_client_remove_path(c, h_usb1), MQVPN_OK);

        /* Retry (next netlink event → add_path_fd) */
        int new_fd = 100 + i;
        mqvpn_path_desc_t d = make_desc(new_fd, "usb1", 0);
        h_usb1 = mqvpn_client_add_path_fd(c, new_fd, &d);
        ASSERT_NE(h_usb1, (mqvpn_path_handle_t)-1);
        ASSERT_EQ(path_status(c, h_usb1), MQVPN_PATH_PENDING);
        ASSERT_EQ(count_paths(c), 2);
    }

    ASSERT_EQ(path_status(c, h_eth0), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 4: Re-add after CLOSED (reconnect scenario)
 *
 * When the xquic path budget is exhausted and a forced reconnect is triggered,
 * the path may transition to CLOSED (retries exhausted) before the reconnect
 * completes.  After reconnect, the platform must be able to re-add the path
 * via add_path_fd() using the CLOSED slot.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * CLOSED path (simulating retries exhausted) can be re-added after reconnect.
 * Uses remove_path() to reach CLOSED since we cannot drive retries without a
 * live connection; the slot semantics are identical (status=CLOSED, active=0).
 */
TEST(closed_path_readdable_after_reconnect)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(30, "usb1", 0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 30, &d);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);

    /* Path closes (retries exhausted or explicit removal) */
    ASSERT_EQ(mqvpn_client_remove_path(c, h1), MQVPN_OK);
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_CLOSED);

    /* Reconnect completes; platform re-adds usb1 with a fresh socket */
    mqvpn_path_desc_t d2 = make_desc(31, "usb1", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 31, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h2, h1);
    ASSERT_EQ(path_status(c, h2), MQVPN_PATH_PENDING);
    ASSERT_EQ(count_paths(c), 1); /* slot reused */

    mqvpn_client_destroy(c);
}

/*
 * CLOSED path (via drop_path) can also be re-added.
 * drop_path is used by the platform when the kernel destroys the interface
 * (RTM_DELLINK); same CLOSED semantics, different call path.
 */
TEST(closed_via_drop_readdable)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    mqvpn_path_desc_t d = make_desc(40, "usb1", 0);
    mqvpn_path_handle_t h1 = mqvpn_client_add_path_fd(c, 40, &d);
    ASSERT_NE(h1, (mqvpn_path_handle_t)-1);

    ASSERT_EQ(mqvpn_client_drop_path(c, h1), MQVPN_OK);
    ASSERT_EQ(path_status(c, h1), MQVPN_PATH_CLOSED);

    mqvpn_path_desc_t d2 = make_desc(41, "usb1", 0);
    mqvpn_path_handle_t h2 = mqvpn_client_add_path_fd(c, 41, &d2);
    ASSERT_NE(h2, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h2), MQVPN_PATH_PENDING);
    ASSERT_EQ(count_paths(c), 1);

    mqvpn_client_destroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 5: Slot array capacity is not exhausted by bouncing
 *
 * With MQVPN_MAX_PATHS=4, repeated bouncing on a single interface must never
 * cause add_path_fd() to fail with -1 (no available slots).
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Filling slots to capacity then bouncing one path must not overflow.
 * Simulates a 4-path setup where one interface (usb1) bounces while the
 * others remain stable.
 */
TEST(full_slot_array_bounce_does_not_overflow)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    /* Fill all slots */
    mqvpn_path_handle_t handles[MQVPN_MAX_PATHS];
    const char *ifaces[] = {"eth0", "usb1", "wlan0", "eth1"};
    for (int i = 0; i < MQVPN_MAX_PATHS; i++) {
        mqvpn_path_desc_t d = make_desc(10 + i, ifaces[i], 0);
        handles[i] = mqvpn_client_add_path_fd(c, 10 + i, &d);
        ASSERT_NE(handles[i], (mqvpn_path_handle_t)-1);
    }
    ASSERT_EQ(count_paths(c), MQVPN_MAX_PATHS);

    /* usb1 (slot 1) bounces 4 times; slots must stay at MQVPN_MAX_PATHS */
    for (int bounce = 0; bounce < 4; bounce++) {
        ASSERT_EQ(mqvpn_client_drop_path(c, handles[1]), MQVPN_OK);

        int new_fd = 100 + bounce;
        mqvpn_path_desc_t d = make_desc(new_fd, "usb1", 0);
        handles[1] = mqvpn_client_add_path_fd(c, new_fd, &d);
        ASSERT_NE(handles[1], (mqvpn_path_handle_t)-1);
        ASSERT_EQ(path_status(c, handles[1]), MQVPN_PATH_PENDING);
        ASSERT_EQ(count_paths(c), MQVPN_MAX_PATHS);
    }

    /* All other paths unaffected */
    ASSERT_EQ(path_status(c, handles[0]), MQVPN_PATH_PENDING); /* eth0 */
    ASSERT_EQ(path_status(c, handles[2]), MQVPN_PATH_PENDING); /* wlan0 */
    ASSERT_EQ(path_status(c, handles[3]), MQVPN_PATH_PENDING); /* eth1 */

    mqvpn_client_destroy(c);
}

/*
 * After all bouncing, adding a genuinely new interface is impossible when
 * slots are full (no CLOSED slot available).
 * Documents the capacity boundary so regressions are visible.
 */
TEST(new_interface_rejected_when_slots_full)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    for (int i = 0; i < MQVPN_MAX_PATHS; i++) {
        mqvpn_path_desc_t d = make_desc(10 + i, "ethX", 0);
        snprintf(d.iface, sizeof(d.iface), "eth%d", i);
        ASSERT_NE(mqvpn_client_add_path_fd(c, 10 + i, &d),
                  (mqvpn_path_handle_t)-1);
    }

    /* All slots active (PENDING) — a fifth interface must be rejected */
    mqvpn_path_desc_t overflow = make_desc(99, "usb2", 0);
    ASSERT_EQ(mqvpn_client_add_path_fd(c, 99, &overflow), (mqvpn_path_handle_t)-1);

    mqvpn_client_destroy(c);
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 6: Exact issue #4276 reconnect sequence
 *
 * After budget-exhaustion: try_readd_removed_path() undoes the add (calls
 * remove_path, fd=-1).  A forced reconnect completes.  cb_state_changed()
 * fires ESTABLISHED and calls try_readd_removed_path() again.  This group
 * verifies the library-level contract that second re-add relies on.
 * ═══════════════════════════════════════════════════════════════════════════ */

/*
 * Full #4276 slot sequence:
 *   1. usb1 added (PENDING)
 *   2. RTM_DELLINK → drop_path (CLOSED, fd freed)
 *   3. RTM_NEWLINK → try_readd_removed_path → add_path_fd; xquic fails →
 *      undo (remove_path, fd freed again)                [CLOSED, active=0]
 *   4. Budget exhausted → reconnect completes (ESTABLISHED)
 *   5. cb_state_changed(ESTABLISHED) calls try_readd_removed_path again →
 *      add_path_fd with fresh fd → PENDING on new connection
 *
 * Steps 2-3 are simulated by drop_path then remove_path.
 * Step 5 is simulated by a second add_path_fd call.
 */
TEST(budget_exhausted_reconnect_reseeds_path)
{
    mqvpn_client_t *c = make_test_client();
    ASSERT_NOT_NULL(c);

    /* Step 1: initial paths */
    mqvpn_path_desc_t d0 = make_desc(10, "eth0", 0);
    mqvpn_path_desc_t d1 = make_desc(11, "usb1", 0);
    mqvpn_path_handle_t h_eth0 = mqvpn_client_add_path_fd(c, 10, &d0);
    mqvpn_path_handle_t h_usb1 = mqvpn_client_add_path_fd(c, 11, &d1);
    ASSERT_NE(h_eth0, (mqvpn_path_handle_t)-1);
    ASSERT_NE(h_usb1, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(count_paths(c), 2);

    /* Step 2: RTM_DELLINK → drop_path */
    ASSERT_EQ(mqvpn_client_drop_path(c, h_usb1), MQVPN_OK);

    /* Step 3: try_readd_removed_path undo (xquic failed, remove_path called) */
    mqvpn_path_desc_t d1b = make_desc(12, "usb1", 0);
    mqvpn_path_handle_t h_tmp = mqvpn_client_add_path_fd(c, 12, &d1b);
    ASSERT_NE(h_tmp, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(mqvpn_client_remove_path(c, h_tmp), MQVPN_OK); /* undo */
    ASSERT_EQ(count_paths(c), 2); /* slot still present, CLOSED */

    /* Step 4-5: reconnect completes (ESTABLISHED) → re-add with fresh fd */
    mqvpn_path_desc_t d1c = make_desc(13, "usb1", 0);
    mqvpn_path_handle_t h_new = mqvpn_client_add_path_fd(c, 13, &d1c);
    ASSERT_NE(h_new, (mqvpn_path_handle_t)-1);
    ASSERT_EQ(path_status(c, h_new), MQVPN_PATH_PENDING);
    ASSERT_EQ(count_paths(c), 2); /* slot reused, eth0 unaffected */
    ASSERT_EQ(path_status(c, h_eth0), MQVPN_PATH_PENDING);

    mqvpn_client_destroy(c);
}

/* ── Runner ── */

int
main(void)
{
    printf("test_path_readd_reconnect  (issue #4276)\n");

    printf("\n  Group 1: RTM_DELLINK + RTM_NEWADDR (drop → re-add)\n");
    run_drop_then_readd_starts_pending();
    run_drop_then_readd_reuses_slot();

    printf("\n  Group 2: try_readd_removed_path undo + retry (remove → re-add)\n");
    run_remove_then_readd_starts_pending();

    printf("\n  Group 3: Repeated bounce cycles (core issue #4276)\n");
    run_eight_bounces_slot_count_stable();
    run_eight_undo_retry_cycles_slot_count_stable();

    printf("\n  Group 4: Re-add after CLOSED (reconnect scenario)\n");
    run_closed_path_readdable_after_reconnect();
    run_closed_via_drop_readdable();

    printf("\n  Group 5: Slot array capacity\n");
    run_full_slot_array_bounce_does_not_overflow();
    run_new_interface_rejected_when_slots_full();

    printf("\n  Group 6: Exact issue #4276 reconnect sequence\n");
    run_budget_exhausted_reconnect_reseeds_path();

    printf("\n%d/%d tests passed\n", g_passed, g_run);
    return (g_passed == g_run) ? 0 : 1;
}
