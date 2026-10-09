// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_dgram_dedup.c — unit tests for the copy filter the redundant
 * scheduler's receive side uses (src/dgram_dedup.c).
 *
 * Build: see CMakeLists.txt (test_dgram_dedup target)
 */
#include "dgram_dedup.h"
#include <stdio.h>
#include <string.h>

static int g_pass = 0, g_fail = 0;

#define ASSERT_EQ_INT(a, b, msg)                                              \
    do {                                                                      \
        if ((long long)(a) == (long long)(b)) {                               \
            g_pass++;                                                         \
        } else {                                                              \
            g_fail++;                                                         \
            fprintf(stderr, "FAIL [%s]: %lld != %lld\n", msg, (long long)(a), \
                    (long long)(b));                                          \
        }                                                                     \
    } while (0)

#define ASSERT_TRUE(cond, msg)                   \
    do {                                         \
        if (cond) {                              \
            g_pass++;                            \
        } else {                                 \
            g_fail++;                            \
            fprintf(stderr, "FAIL [%s]\n", msg); \
        }                                        \
    } while (0)

#define T0  1000000ULL
#define WIN MQVPN_DGRAM_DEDUP_WINDOW_US

/* An IPv4/UDP-looking packet whose bytes depend on n. */
static void
make_pkt(uint8_t *p, size_t len, uint32_t n)
{
    for (size_t i = 0; i < len; i++)
        p[i] = (uint8_t)(i * 7 + 3);
    p[0] = 0x45;
    memcpy(p + 28, &n, sizeof(n));
}

static void
test_copies_dropped_within_window(void)
{
    mqvpn_dgram_dedup_t *d = mqvpn_dgram_dedup_new(42);
    ASSERT_TRUE(d != NULL, "new");
    uint8_t pkt[1300];
    make_pkt(pkt, sizeof(pkt), 1);

    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0), 0,
                  "first copy passes");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + 30000), 1,
                  "second copy dropped");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + WIN - 1), 1,
                  "third copy, last microsecond of the window, dropped");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_dropped(d), 2, "dropped count");
    mqvpn_dgram_dedup_free(d);
}

static void
test_same_packet_after_window_passes(void)
{
    mqvpn_dgram_dedup_t *d = mqvpn_dgram_dedup_new(42);
    uint8_t pkt[100];
    make_pkt(pkt, sizeof(pkt), 2);

    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0), 0, "first passes");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + WIN), 0,
                  "same packet once the window has passed");
    /* ... and is the new reference for its own copies */
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + WIN + 10), 1,
                  "copy of the resent packet dropped");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_dropped(d), 1, "dropped count");
    mqvpn_dgram_dedup_free(d);
}

static void
test_different_packets_pass(void)
{
    mqvpn_dgram_dedup_t *d = mqvpn_dgram_dedup_new(7);
    uint8_t a[1300], b[1300];
    make_pkt(a, sizeof(a), 3);
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, a, sizeof(a), T0), 0, "a passes");

    /* one byte different, at the start, in the middle, at the end */
    static const size_t at[] = {1, 640, 1299};
    for (size_t i = 0; i < sizeof(at) / sizeof(at[0]); i++) {
        memcpy(b, a, sizeof(a));
        b[at[i]] ^= 0x01;
        ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, b, sizeof(b), T0 + 1), 0,
                      "one-byte difference passes");
    }
    /* same bytes, shorter */
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, a, sizeof(a) - 1, T0 + 2), 0,
                  "prefix of a passes");
    /* lengths that are not a multiple of 8, and a 1-byte packet */
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, a, 21, T0 + 3), 0, "21 bytes pass");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, a, 21, T0 + 4), 1, "21 bytes again dropped");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, a, 1, T0 + 5), 0, "1 byte passes");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, a, 1, T0 + 6), 1, "1 byte again dropped");
    ASSERT_EQ_INT(mqvpn_dgram_dedup_dropped(d), 2, "dropped count");
    mqvpn_dgram_dedup_free(d);
}

/* Many distinct packets within one window: none may be dropped, and once
 * the table has overflowed the copies of the oldest ones may get through
 * (missed copies), but nothing else changes. */
static void
test_overflow_never_drops_distinct_packets(void)
{
    mqvpn_dgram_dedup_t *d = mqvpn_dgram_dedup_new(99);
    uint8_t pkt[200];
    const uint32_t n = 100000;
    int dropped = 0;
    for (uint32_t i = 0; i < n; i++) {
        make_pkt(pkt, sizeof(pkt), i);
        dropped += mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + i % WIN);
    }
    ASSERT_EQ_INT(dropped, 0, "no distinct packet dropped");

    /* the most recent packets are still remembered */
    make_pkt(pkt, sizeof(pkt), n - 1);
    ASSERT_EQ_INT(mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + (n - 1) % WIN + 1), 1,
                  "copy of the newest packet dropped");
    mqvpn_dgram_dedup_free(d);
}

/* A burst of copies in flight: each packet's copy arrives after the next
 * few originals, as with two paths whose delays differ. */
static void
test_interleaved_copies(void)
{
    mqvpn_dgram_dedup_t *d = mqvpn_dgram_dedup_new(5);
    uint8_t pkt[1300];
    const int n = 2000, lag = 50;
    int passed = 0, dropped = 0;
    for (int i = 0; i < n + lag; i++) {
        if (i < n) {
            make_pkt(pkt, sizeof(pkt), (uint32_t)i);
            passed +=
                !mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + (uint64_t)i * 100);
        }
        if (i >= lag) {
            make_pkt(pkt, sizeof(pkt), (uint32_t)(i - lag));
            dropped +=
                mqvpn_dgram_dedup_seen(d, pkt, sizeof(pkt), T0 + (uint64_t)i * 100);
        }
    }
    ASSERT_EQ_INT(passed, n, "every original passes");
    ASSERT_EQ_INT(dropped, n, "every copy dropped");
    mqvpn_dgram_dedup_free(d);
}

static void
test_null_safe(void)
{
    ASSERT_EQ_INT(mqvpn_dgram_dedup_dropped(NULL), 0, "dropped(NULL)");
    mqvpn_dgram_dedup_free(NULL);
    ASSERT_TRUE(1, "free(NULL)");
}

int
main(void)
{
    test_copies_dropped_within_window();
    test_same_packet_after_window_passes();
    test_different_packets_pass();
    test_overflow_never_drops_distinct_packets();
    test_interleaved_copies();
    test_null_safe();
    printf("test_dgram_dedup: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
