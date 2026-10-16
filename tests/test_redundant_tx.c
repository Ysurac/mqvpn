// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_redundant_tx.c — unit tests for the redundant-mode TX module
 * (port-list parsing, packet matching, per-path outstanding-copy guard, overload mode).
 */

/* Keep assert() live even in Release builds (CI-checked). */
#undef NDEBUG
#include <assert.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "redundant_tx.h"

static int g_run = 0;
#define RUN(fn)                  \
    do {                         \
        printf("  %-40s ", #fn); \
        fn();                    \
        g_run++;                 \
        printf("PASS\n");        \
    } while (0)

static uint64_t g_next_id = 1;

/* ---- packet helpers ---- */

static size_t
mk_udp4(uint8_t *p, uint16_t dport, uint16_t frag_off)
{
    memset(p, 0, 28);
    p[0] = 0x45;
    p[6] = (uint8_t)(frag_off >> 8);
    p[7] = (uint8_t)frag_off;
    p[9] = 17;
    p[22] = (uint8_t)(dport >> 8);
    p[23] = (uint8_t)dport;
    return 28;
}

static size_t
mk_tcp4(uint8_t *p, uint16_t dport)
{
    size_t n = mk_udp4(p, dport, 0);
    p[9] = 6;
    return n;
}

static size_t
mk_udp6(uint8_t *p, uint8_t next_hdr, uint16_t dport)
{
    memset(p, 0, 48);
    p[0] = 0x60;
    p[6] = next_hdr;
    p[42] = (uint8_t)(dport >> 8);
    p[43] = (uint8_t)dport;
    return 48;
}

/* ---- state helpers ---- */

/* One unresolved single-copy original on `path`; returns its dgram id. */
static uint64_t
add_copy(mqvpn_redundant_tx_t *t, int path, uint64_t now_us)
{
    uint64_t id = g_next_id++;
    mqvpn_redundant_tx_commit(t, &id, &path, 1, now_us, 1400);
    return id;
}

static int
sel(mqvpn_redundant_tx_t *t, const int *usable, int n, int *out)
{
    return mqvpn_redundant_tx_select(t, usable, n, out);
}

/* Mirror the client at 1000 pps from from_ms to to_ms (inclusive, 1 ms apart)
 * on paths {0, 1}. Each ms, first the packet: if overload_bypass() returns 0,
 * select + commit (ids from g_next_id), acking path 1's copy at once when
 * (path-1 copies so far in this call % 100) < ack_pct; then, when ms % 100 == 0,
 * update_mode(). Stops at the first non-zero update result, returns it and
 * its time in *at_ms (0 if none). */
static int
drive(mqvpn_redundant_tx_t *t, uint64_t from_ms, uint64_t to_ms, int ack_pct,
      uint64_t *at_ms)
{
    const int usable[2] = {0, 1};
    uint64_t n1 = 0;
    *at_ms = 0;
    for (uint64_t ms = from_ms; ms <= to_ms; ms++) {
        uint64_t now = ms * 1000;
        if (!mqvpn_redundant_tx_overload_bypass(t, now)) {
            int paths[2], k = mqvpn_redundant_tx_select(t, usable, 2, paths);
            uint64_t ids[2];
            for (int i = 0; i < k; i++)
                ids[i] = g_next_id++;
            mqvpn_redundant_tx_commit(t, ids, paths, k, now, 1400);
            for (int i = 0; i < k; i++)
                if (paths[i] == 1 && n1++ % 100 < (uint64_t)ack_pct)
                    mqvpn_redundant_tx_on_acked(t, ids[i]);
        }
        if (ms % 100 == 0) {
            int r = mqvpn_redundant_tx_update_mode(t, usable, 2, now);
            if (r) {
                *at_ms = ms;
                return r;
            }
        }
    }
    return 0;
}

/* An all-short stream (nothing acked) enters overload mode at 2900 ms: five
 * judgements from 2100 ms, the first outside the grace. */
static void
enter_overload(mqvpn_redundant_tx_t *t)
{
    uint64_t at;
    assert(drive(t, 1, 6000, 0, &at) == 1);
    assert(at == 2900);
}

/* A stream of one original every step_ms from step_ms to to_ms, copies on paths
 * {0, 1} (no select, nothing acked), update_mode() at every multiple of 100 ms.
 * Stops at the first non-zero update result, returns it and its time in *at_ms
 * (0 if none). */
static int
drive_every(mqvpn_redundant_tx_t *t, uint64_t step_ms, uint64_t to_ms, uint64_t *at_ms)
{
    const int paths[2] = {0, 1};
    *at_ms = 0;
    for (uint64_t ms = step_ms; ms <= to_ms; ms += step_ms) {
        uint64_t ids[2];
        ids[0] = g_next_id++;
        ids[1] = g_next_id++;
        mqvpn_redundant_tx_commit(t, ids, paths, 2, ms * 1000, 1400);
        if (ms % 100 == 0) {
            int r = mqvpn_redundant_tx_update_mode(t, paths, 2, ms * 1000);
            if (r) {
                *at_ms = ms;
                return r;
            }
        }
    }
    return 0;
}

/* 500 pps of 200 B copies on paths {0, 1} from from_ms to to_ms (inclusive, one original
 * every 2 ms): 90% of each path's copies are acked at once, so every path is short by
 * rate, and the rest stay unresolved. At every multiple of 100 ms, before update_mode(),
 * each path's bytes in flight are set to inflight_pct % of its unresolved copies' bytes.
 * Stops at the first non-zero update result, returns it and its time in *at_ms (0 if
 * none). */
static int
drive_small(mqvpn_redundant_tx_t *t, uint64_t from_ms, uint64_t to_ms, int inflight_pct,
            uint64_t *at_ms)
{
    const int paths[2] = {0, 1};
    uint64_t n[2] = {0, 0};
    *at_ms = 0;
    for (uint64_t ms = from_ms; ms <= to_ms; ms += 2) {
        uint64_t ids[2];
        ids[0] = g_next_id++;
        ids[1] = g_next_id++;
        mqvpn_redundant_tx_commit(t, ids, paths, 2, ms * 1000, 200);
        for (int p = 0; p < 2; p++)
            if (n[p]++ % 10 < 9) mqvpn_redundant_tx_on_acked(t, ids[p]);
        if (ms % 100 == 0) {
            for (int p = 0; p < 2; p++) {
                uint64_t unresolved =
                    (uint64_t)mqvpn_redundant_tx_outstanding(t, p) * 200;
                mqvpn_redundant_tx_set_path_metrics(t, p, 0, 0,
                                                    unresolved * inflight_pct / 100);
            }
            int r = mqvpn_redundant_tx_update_mode(t, paths, 2, ms * 1000);
            if (r) {
                *at_ms = ms;
                return r;
            }
        }
    }
    return 0;
}

/* ---- parse ---- */

static void
test_parse_port_list_ok(void)
{
    uint16_t o[16];
    assert(mqvpn_redundant_parse_ports(" 4200 , 9000 ", o, 16) == 2);
    assert(o[0] == 4200 && o[1] == 9000);
    assert(mqvpn_redundant_parse_ports("4200,4200", o, 16) == 1);
    assert(mqvpn_redundant_parse_ports("", o, 16) == 0);
    assert(mqvpn_redundant_parse_ports(NULL, o, 16) == 0);
    assert(mqvpn_redundant_parse_ports("  ", o, 16) == 0);
    assert(mqvpn_redundant_parse_ports("4200,4200,4200,4200,4200,4200,4200,4200,4200,"
                                       "4200,4200,4200,4200,4200,4200,4200",
                                       o, 16) == 1);
    assert(o[0] == 4200);
    assert(mqvpn_redundant_parse_ports("65535,1", o, 16) == 2);
}

static void
test_parse_port_list_rejects(void)
{
    uint16_t o[16];
    const char *bad[] = {"4200,abc", "0",    "70000",     "-1", "4200,",
                         ",4200",    "42x0", "4200 9000", "+5"};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        assert(mqvpn_redundant_parse_ports(bad[i], o, 16) == -1);
    assert(mqvpn_redundant_parse_ports("4200,4200,4200,4200,4200,4200,4200,4200,4200,"
                                       "4200,4200,4200,4200,4200,4200,4200,4200",
                                       o, 16) == -1);
}

/* ---- match ---- */

static void
test_match_all_udp(void)
{
    uint8_t p[64];
    size_t n = mk_udp4(p, 5000, 0);
    assert(mqvpn_redundant_match(NULL, 0, p, n) == 1);
    n = mk_udp6(p, 17, 5000);
    assert(mqvpn_redundant_match(NULL, 0, p, n) == 1);
    n = mk_tcp4(p, 5000);
    assert(mqvpn_redundant_match(NULL, 0, p, n) == 0);
    n = mk_udp4(p, 5000, 100);
    assert(mqvpn_redundant_match(NULL, 0, p, n) == 1);
    mk_udp4(p, 5000, 0);
    assert(mqvpn_redundant_match(NULL, 0, p, 19) == 0);
}

static void
test_match_ports(void)
{
    uint16_t ports[1] = {4200};
    uint8_t p[64];
    size_t n = mk_udp4(p, 4200, 0);
    assert(mqvpn_redundant_match(ports, 1, p, n) == 1);
    n = mk_udp4(p, 4201, 0);
    assert(mqvpn_redundant_match(ports, 1, p, n) == 0);
    n = mk_udp4(p, 4200, 100);
    assert(mqvpn_redundant_match(ports, 1, p, n) == 0);
    n = mk_udp6(p, 17, 4200);
    assert(mqvpn_redundant_match(ports, 1, p, n) == 1);
    n = mk_udp6(p, 0, 4200);
    assert(mqvpn_redundant_match(ports, 1, p, n) == 0);
}

/* ---- select / guard ---- */

static void
test_select_all_under_limit(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    int u[3] = {0, 1, 2}, out[8];
    assert(sel(t, u, 3, out) == 3);
    assert(out[0] == 0 && out[1] == 1 && out[2] == 2);
    mqvpn_redundant_tx_free(t);
}

static void
test_select_skips_full_path(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    for (int i = 0; i < 300; i++)
        add_copy(t, 0, 0);
    int u[2] = {0, 1}, out[8];
    assert(sel(t, u, 2, out) == 1 && out[0] == 1);
    mqvpn_redundant_tx_free(t);
}

static void
test_select_all_full_least_loaded(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    for (int i = 0; i < 310; i++)
        add_copy(t, 0, 0);
    for (int i = 0; i < 305; i++)
        add_copy(t, 1, 0);
    int u[2] = {0, 1}, out[8];
    assert(sel(t, u, 2, out) == 1 && out[0] == 1);
    mqvpn_redundant_tx_free(t);
}

static void
test_select_single_path(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    int u[1] = {1}, out[8];
    assert(sel(t, u, 1, out) == 1 && out[0] == 1);
    mqvpn_redundant_tx_free(t);
}

static void
test_limit_scales(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    for (int i = 0; i < 1000; i++)
        add_copy(t, 1, 500000);
    int none = 0;
    uint64_t id = g_next_id++;
    mqvpn_redundant_tx_commit(t, &id, &none, 0, 600000, 1400); /* k == 0: not counted */
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);             /* pps still 0 */
    add_copy(t, 1, 1500000);                                   /* second 1 */
    mqvpn_redundant_tx_set_path_metrics(t, 0, 150000, 0, 0);
    assert(mqvpn_redundant_tx_limit(t, 0) == 400);
    assert(mqvpn_redundant_tx_limit(t, 1) == 300);
    mqvpn_redundant_tx_free(t);
}

static mqvpn_redundant_tx_t *
fresh_tracker(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    mqvpn_redundant_tx_set_path_metrics(t, 0, 300000, 0, 0);
    return t;
}

static void
test_limit_first_second_rate(void)
{
    mqvpn_redundant_tx_t *t;
    t = fresh_tracker(); /* trickle lead-in, then the real flow */
    add_copy(t, 1, 5000000);
    add_copy(t, 1, 5120000);
    for (int i = 0; i < 200; i++)
        add_copy(t, 1, 5300000 + (uint64_t)i * 500);
    add_copy(t, 1, 5400000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 1100); /* 2000 pps */
    mqvpn_redundant_tx_free(t);

    t = fresh_tracker(); /* only the adjacent bucket counts */
    for (int i = 0; i < 200; i++)
        add_copy(t, 1, 6000000 + (uint64_t)i * 500);
    add_copy(t, 1, 6200000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    t = fresh_tracker(); /* no complete bucket yet */
    for (int i = 0; i < 150; i++)
        add_copy(t, 1, 7000000 + (uint64_t)i * 500);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    t = fresh_tracker(); /* 20 pkt/s stays at the floor */
    for (int i = 0; i < 20; i++)
        add_copy(t, 1, (uint64_t)i * 50000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    t = fresh_tracker(); /* clock stepped back: window and bucket restart */
    for (int i = 0; i < 1710; i++)
        add_copy(t, 1, 8000000 + (uint64_t)i * 900000 / 1709);
    add_copy(t, 1, 8100000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    t = fresh_tracker(); /* the previous second's count wins when larger */
    for (int i = 0; i < 1900; i++)
        add_copy(t, 1, 9000000 + (uint64_t)i * 500);
    add_copy(t, 1, 10000000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 1900 * 550000 / 1000000); /* 1045 */
    mqvpn_redundant_tx_free(t);

    t = fresh_tracker(); /* step back inside the open bucket resets the bucket too */
    for (int i = 0; i < 1900; i++)
        add_copy(t, 1, 9000000 + (uint64_t)i * 500);
    add_copy(t, 1, 10000000);
    add_copy(t, 1, 10050000);
    add_copy(t, 1, 10049999);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);
}

static void
test_ack_lost_accounting(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    uint64_t a[2] = {1, 2}, b[2] = {3, 4}, c[2] = {5, 6};
    int p[2] = {0, 1};
    mqvpn_redundant_tx_commit(t, a, p, 2, 0, 1400);
    mqvpn_redundant_tx_commit(t, b, p, 2, 0, 1400);
    mqvpn_redundant_tx_commit(t, c, p, 2, 0, 1400);
    assert(mqvpn_redundant_tx_outstanding(t, 0) == 3);
    assert(mqvpn_redundant_tx_on_acked(t, 1) == 1);
    assert(mqvpn_redundant_tx_on_acked(t, 2) == 0);
    assert(mqvpn_redundant_tx_on_lost(t, 3) == 0);
    assert(mqvpn_redundant_tx_on_lost(t, 4) == 1);
    assert(mqvpn_redundant_tx_on_lost(t, 5) == 0);
    assert(mqvpn_redundant_tx_on_acked(t, 6) == 1);
    assert(mqvpn_redundant_tx_on_acked(t, 99) == -1);
    assert(mqvpn_redundant_tx_on_lost(t, 99) == -1);
    assert(mqvpn_redundant_tx_outstanding(t, 0) == 0);
    assert(mqvpn_redundant_tx_outstanding(t, 1) == 0);
    mqvpn_redundant_tx_free(t);
}

static void
test_no_eviction_of_live_copies(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    uint64_t stalled[300];
    for (int i = 0; i < 300; i++)
        stalled[i] = add_copy(t, 0, 0);
    for (int i = 0; i < 20000; i++) {
        uint64_t id = add_copy(t, 1, 0);
        assert(mqvpn_redundant_tx_on_acked(t, id) == 1);
    }
    assert(mqvpn_redundant_tx_outstanding(t, 0) == 300);
    int u[2] = {0, 1}, out[8];
    assert(sel(t, u, 2, out) == 1 && out[0] == 1);
    for (int i = 0; i < 300; i++) {
        int r = (i & 1) ? mqvpn_redundant_tx_on_acked(t, stalled[i])
                        : mqvpn_redundant_tx_on_lost(t, stalled[i]);
        assert(r == 1);
    }
    assert(mqvpn_redundant_tx_outstanding(t, 0) == 0);
    mqvpn_redundant_tx_free(t);
}

static void
test_capacity_truncates(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    for (int i = 0; i < 1000; i++)
        add_copy(t, 2, 0); /* gives pps 1000 */
    mqvpn_redundant_tx_set_path_metrics(t, 2, 1000000000ull, 0, 0);
    for (int i = 0; i < 16383 - 1000; i++)
        add_copy(t, 2, 1000000);
    int u[3] = {0, 1, 2}, out[8];
    assert(sel(t, u, 3, out) == 1);
    add_copy(t, 2, 1000000);
    assert(sel(t, u, 3, out) == 0);
    mqvpn_redundant_tx_free(t);
}

static void
test_full_table_unknown_lookup(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    for (int i = 0; i < 16384; i++)
        add_copy(t, 2, 0);
    assert(mqvpn_redundant_tx_on_acked(t, 999999999ull) == -1);
    assert(mqvpn_redundant_tx_on_lost(t, 999999999ull) == -1);
    mqvpn_redundant_tx_free(t);
}

static void
test_detach_and_rebind(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    mqvpn_redundant_tx_bind_path(t, 2, 7);
    uint64_t x[2] = {1, 2}, y[2] = {3, 4};
    int p[2] = {0, 2};
    mqvpn_redundant_tx_commit(t, x, p, 2, 0, 1400);
    mqvpn_redundant_tx_commit(t, y, p, 2, 0, 1400);
    assert(mqvpn_redundant_tx_outstanding(t, 2) == 2);
    mqvpn_redundant_tx_bind_path(t, 2, 7); /* same id: no detach */
    assert(mqvpn_redundant_tx_outstanding(t, 2) == 2);
    mqvpn_redundant_tx_bind_path(t, 2, 8);
    assert(mqvpn_redundant_tx_outstanding(t, 2) == 0);
    assert(mqvpn_redundant_tx_on_acked(t, 1) == 1);
    assert(mqvpn_redundant_tx_on_acked(t, 2) == 0);
    assert(mqvpn_redundant_tx_outstanding(t, 2) == 0);
    assert(mqvpn_redundant_tx_on_lost(t, 3) == 0);
    assert(mqvpn_redundant_tx_on_lost(t, 4) == 1);
    /* explicit detach */
    uint64_t z = add_copy(t, 2, 0);
    mqvpn_redundant_tx_detach_path(t, 2);
    assert(mqvpn_redundant_tx_outstanding(t, 2) == 0);
    assert(mqvpn_redundant_tx_on_lost(t, z) == 1);
    mqvpn_redundant_tx_free(t);
}

/* ---- overload mode ---- */

static void
test_overload_enters_when_every_path_short(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    enter_overload(t);
    int o0 = mqvpn_redundant_tx_outstanding(t, 0);
    int o1 = mqvpn_redundant_tx_outstanding(t, 1);
    assert(mqvpn_redundant_tx_overload_bypass(t, 2901000) == 1);
    assert(mqvpn_redundant_tx_outstanding(t, 0) == o0);
    assert(mqvpn_redundant_tx_outstanding(t, 1) == o1);
    mqvpn_redundant_tx_free(t);
}

static void
test_overload_not_while_one_path_keeps_up(void)
{
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    assert(drive(t, 1, 6000, 100, &at) == 0);
    mqvpn_redundant_tx_free(t);

    t = mqvpn_redundant_tx_new();
    assert(drive(t, 1, 6000, 97, &at) == 0); /* 970 pps: not under 95% of the stream */
    mqvpn_redundant_tx_free(t);

    t = mqvpn_redundant_tx_new();
    assert(drive(t, 1, 6000, 90, &at) == 1); /* 900 pps: short */
    assert(at == 2900);
    mqvpn_redundant_tx_free(t);
}

static void
test_overload_bypass_counts_rate(void)
{
    mqvpn_redundant_tx_t *t = fresh_tracker();
    enter_overload(t);
    assert(mqvpn_redundant_tx_limit(t, 0) == 550); /* 1000 pps */
    for (uint64_t i = 1; i <= 4000; i++)           /* 2000 pps for 2 s, none committed */
        assert(mqvpn_redundant_tx_overload_bypass(t, 2900000 + i * 500) == 1);
    assert(mqvpn_redundant_tx_limit(t, 0) == 1100); /* an uncounted bypass leaves 550 */
    mqvpn_redundant_tx_free(t);
}

static void
test_probe_needs_hold_and_fit(void)
{
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    /* 1000 pps worth of bytes: under 1.05 x the stream's lowest second */
    mqvpn_redundant_tx_set_path_metrics(t, 1, 0, 1400000, 0);
    enter_overload(t);
    assert(drive(t, 2901, 32900, 0, &at) == 0); /* the hold ended at 12900 */
    mqvpn_redundant_tx_set_path_metrics(t, 1, 0, 1500000, 0);
    assert(drive(t, 32901, 40000, 0, &at) == -1);
    assert(at == 33100);
    mqvpn_redundant_tx_free(t);
}

static void
test_probe_base_is_lowest_second(void)
{
    const int u[2] = {0, 1};
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    /* 1000 pps worth of bytes: no fit at base 999 or 1000, a fit at base 900 */
    mqvpn_redundant_tx_set_path_metrics(t, 1, 0, 1400000, 0);
    enter_overload(t);
    assert(drive(t, 2901, 4999, 0, &at) == 0);
    for (uint64_t ms = 5000; ms < 6000; ms++) { /* second 5 carries 900 packets */
        if (ms % 10) assert(mqvpn_redundant_tx_overload_bypass(t, ms * 1000) == 1);
        if (ms % 100 == 0)
            assert(mqvpn_redundant_tx_update_mode(t, u, 2, ms * 1000) == 0);
    }
    /* the hold ends at 12900, when the history holds seconds 2-11: base 900, not the
     * 1000 of the last second */
    assert(drive(t, 6000, 13000, 0, &at) == -1);
    assert(at == 12900);
    mqvpn_redundant_tx_free(t);
}

static void
test_failed_probe_backs_off(void)
{
    static const struct {
        uint64_t to_ms;
        int r;
        uint64_t at;
    } step[] = {
        {6000, 1, 2900},    /* entry */
        {13000, -1, 12900}, /* probe after the 10 s hold */
        {14000, 1, 13300},  /* two short judgements: hold 20 s */
        {34000, -1, 33300}, /* probe */
        {35000, 1, 33700},  /* hold 40 s, capped at 32 s */
        {66000, -1, 65700}, /* probe */
        {67000, 1, 66100},  /* hold stays 32 s */
        {99000, -1, 98100}, /* probe */
    };
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    mqvpn_redundant_tx_set_path_metrics(t, 1, 0, 1500000, 0); /* fits from the start */
    uint64_t from = 1, at;
    for (size_t i = 0; i < sizeof(step) / sizeof(step[0]); i++) {
        assert(drive(t, from, step[i].to_ms, 0, &at) == step[i].r);
        assert(at == step[i].at);
        from = at + 1;
    }
    mqvpn_redundant_tx_free(t);
}

static void
test_probe_held_resets_hold(void)
{
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    mqvpn_redundant_tx_set_path_metrics(t, 1, 0, 1500000, 0);
    enter_overload(t);
    assert(drive(t, 2901, 13000, 0, &at) == -1 && at == 12900);
    assert(drive(t, 12901, 14000, 0, &at) == 1 && at == 13300); /* hold 20 s */
    assert(drive(t, 13301, 34000, 0, &at) == -1 && at == 33300);
    /* path 1 carries the stream; at 43300 the probe has lasted 10 s: hold 10 s */
    assert(drive(t, 33301, 43300, 100, &at) == 0);
    assert(drive(t, 43301, 45000, 0, &at) == 1 && at == 44300);  /* five judgements */
    assert(drive(t, 44301, 55000, 0, &at) == -1 && at == 54300); /* 64300 at hold 20 s */
    mqvpn_redundant_tx_free(t);
}

static void
test_mode_resets_when_stream_stops(void)
{
    const int u[2] = {0, 1};
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    enter_overload(t); /* the last packet is at 2900 ms */
    for (uint64_t ms = 3000; ms < 3900; ms += 100)
        assert(mqvpn_redundant_tx_update_mode(t, u, 2, ms * 1000) == 0);
    assert(mqvpn_redundant_tx_update_mode(t, u, 2, 3900000) == -1);
    assert(drive(t, 3901, 10000, 0, &at) == 1);
    assert(at == 6800); /* the grace starts again */
    mqvpn_redundant_tx_free(t);

    t = mqvpn_redundant_tx_new();
    enter_overload(t);
    /* the clock is below the last counted packet */
    assert(mqvpn_redundant_tx_update_mode(t, u, 2, 2800000) == -1);
    mqvpn_redundant_tx_free(t);

    t = mqvpn_redundant_tx_new();
    enter_overload(t);
    /* a judgement restarts the window at 3100; 3000 is below it, not below the packet */
    assert(mqvpn_redundant_tx_update_mode(t, u, 2, 3100000) == 0);
    assert(mqvpn_redundant_tx_update_mode(t, u, 2, 3000000) == -1);
    mqvpn_redundant_tx_free(t);
}

static void
test_mode_resets_after_total_outage(void)
{
    const int u[2] = {0, 1};
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    enter_overload(t); /* the last packet is at 2900 ms */
    /* no path until 6000: nothing is counted and no update runs, and the first packet
     * of the returning path is counted before the next update */
    assert(mqvpn_redundant_tx_overload_bypass(t, 6001000) == 1); /* still overload */
    assert(mqvpn_redundant_tx_update_mode(t, u, 2, 6100000) == -1);
    assert(drive(t, 6101, 12000, 0, &at) == 1);
    assert(at == 9000); /* the window restarts at 6200, the grace ends at 8200 */
    mqvpn_redundant_tx_free(t);

    t = mqvpn_redundant_tx_new();
    assert(drive(t, 1, 3000, 100, &at) == 0); /* path 1 keeps up: normal mode */
    /* the same outage; the returning path has nothing acked yet */
    assert(drive(t, 6001, 12000, 0, &at) == 1);
    assert(at == 9000); /* a fresh grace */
    mqvpn_redundant_tx_free(t);
}

static void
test_short_needs_fed(void)
{
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    for (int p = 0; p < 2; p++) /* in flight covers the outstanding copies */
        mqvpn_redundant_tx_set_path_metrics(t, p, 0, 0, 100000000);
    assert(drive_every(t, 1, 3000, &at) == 0); /* no select: no path hits its limit */
    assert(drive(t, 3001, 6000, 0, &at) == 1); /* select skips both paths: fed */
    assert(at == 3900);
    mqvpn_redundant_tx_free(t);
}

static void
test_overload_needs_enough_copies(void)
{
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    assert(drive_every(t, 20, 6000, &at) == 0); /* 50 pps: 10 copies per window */
    mqvpn_redundant_tx_free(t);

    t = mqvpn_redundant_tx_new();
    assert(drive_every(t, 4, 6000, &at) == 1); /* 250 pps: 50 copies per window */
    assert(at == 2900);
    mqvpn_redundant_tx_free(t);
}

static void
test_fed_counts_bytes(void)
{
    uint64_t at;
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    /* everything sent is in flight: nothing is queued, so no path is fed */
    assert(drive_small(t, 2, 6000, 100, &at) == 0);
    /* half of it is: the copies queue, and every path is short by rate */
    assert(drive_small(t, 6002, 8000, 50, &at) == 1);
    assert(at == 6900); /* five fed judgements from 6100 ms, past the grace */
    mqvpn_redundant_tx_free(t);
}

int
main(void)
{
    RUN(test_parse_port_list_ok);
    RUN(test_parse_port_list_rejects);
    RUN(test_match_all_udp);
    RUN(test_match_ports);
    RUN(test_select_all_under_limit);
    RUN(test_select_skips_full_path);
    RUN(test_select_all_full_least_loaded);
    RUN(test_select_single_path);
    RUN(test_limit_scales);
    RUN(test_limit_first_second_rate);
    RUN(test_ack_lost_accounting);
    RUN(test_no_eviction_of_live_copies);
    RUN(test_capacity_truncates);
    RUN(test_full_table_unknown_lookup);
    RUN(test_detach_and_rebind);
    RUN(test_overload_enters_when_every_path_short);
    RUN(test_overload_not_while_one_path_keeps_up);
    RUN(test_overload_bypass_counts_rate);
    RUN(test_probe_needs_hold_and_fit);
    RUN(test_probe_base_is_lowest_second);
    RUN(test_failed_probe_backs_off);
    RUN(test_probe_held_resets_hold);
    RUN(test_mode_resets_when_stream_stops);
    RUN(test_mode_resets_after_total_outage);
    RUN(test_short_needs_fed);
    RUN(test_fed_counts_bytes);
    RUN(test_overload_needs_enough_copies);
    printf("\n=== test_redundant_tx: %d passed ===\n", g_run);
    return 0;
}
