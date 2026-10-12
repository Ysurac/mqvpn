// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_redundant_tx.c — unit tests for the redundant-mode TX module
 * (port-list parsing, packet matching, per-path outstanding-copy guard).
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
    mqvpn_redundant_tx_commit(t, &id, &path, 1, now_us);
    return id;
}

static int
sel(mqvpn_redundant_tx_t *t, const int *usable, int n, int *out)
{
    return mqvpn_redundant_tx_select(t, usable, n, out);
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
    mqvpn_redundant_tx_commit(t, &id, &none, 0, 600000); /* k == 0: not counted */
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);       /* pps still 0 */
    add_copy(t, 1, 1500000);                             /* second 1 */
    mqvpn_redundant_tx_set_srtt(t, 0, 150000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 400);
    assert(mqvpn_redundant_tx_limit(t, 1) == 300);
    mqvpn_redundant_tx_free(t);
}

static void
test_limit_first_second_rate(void)
{
    mqvpn_redundant_tx_t *t;
#define FRESH()                                    \
    do {                                           \
        t = mqvpn_redundant_tx_new();              \
        mqvpn_redundant_tx_set_srtt(t, 0, 300000); \
    } while (0)
    FRESH(); /* trickle lead-in, then the real flow */
    add_copy(t, 1, 5000000);
    add_copy(t, 1, 5120000);
    for (int i = 0; i < 200; i++)
        add_copy(t, 1, 5300000 + (uint64_t)i * 500);
    add_copy(t, 1, 5400000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 1100); /* 2000 pps */
    mqvpn_redundant_tx_free(t);

    FRESH(); /* only the adjacent bucket counts */
    for (int i = 0; i < 200; i++)
        add_copy(t, 1, 6000000 + (uint64_t)i * 500);
    add_copy(t, 1, 6200000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    FRESH(); /* no complete bucket yet */
    for (int i = 0; i < 150; i++)
        add_copy(t, 1, 7000000 + (uint64_t)i * 500);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    FRESH(); /* 20 pkt/s stays at the floor */
    for (int i = 0; i < 20; i++)
        add_copy(t, 1, (uint64_t)i * 50000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    FRESH(); /* clock stepped back: window and bucket restart */
    for (int i = 0; i < 1710; i++)
        add_copy(t, 1, 8000000 + (uint64_t)i * 900000 / 1709);
    add_copy(t, 1, 8100000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 300);
    mqvpn_redundant_tx_free(t);

    FRESH(); /* the previous second's count wins when larger */
    for (int i = 0; i < 1900; i++)
        add_copy(t, 1, 9000000 + (uint64_t)i * 500);
    add_copy(t, 1, 10000000);
    assert(mqvpn_redundant_tx_limit(t, 0) == 1900 * 550000 / 1000000); /* 1045 */
    mqvpn_redundant_tx_free(t);
#undef FRESH
}

static void
test_ack_lost_accounting(void)
{
    mqvpn_redundant_tx_t *t = mqvpn_redundant_tx_new();
    uint64_t a[2] = {1, 2}, b[2] = {3, 4}, c[2] = {5, 6};
    int p[2] = {0, 1};
    mqvpn_redundant_tx_commit(t, a, p, 2, 0);
    mqvpn_redundant_tx_commit(t, b, p, 2, 0);
    mqvpn_redundant_tx_commit(t, c, p, 2, 0);
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
    mqvpn_redundant_tx_set_srtt(t, 2, 1000000000ull);
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
    mqvpn_redundant_tx_commit(t, x, p, 2, 0);
    mqvpn_redundant_tx_commit(t, y, p, 2, 0);
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
    printf("\n=== test_redundant_tx: %d passed ===\n", g_run);
    return 0;
}
