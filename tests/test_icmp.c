// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_icmp.c — mqvpn_icmp_send_v4 / _v6 packet-builder unit tests.
 *
 * icmp.c is pure logic: it formats an ICMP(v6) error into a stack buffer and
 * hands it to a callback — no sockets, no I/O — so it is fully unit-testable.
 * These tests pin (1) the IP/ICMP header field layout, (2) the RFC-mandated
 * quote-length + min-MTU truncation, (3) the guard conditions that must drop
 * the packet, and (4) checksum correctness (recomputed independently: a valid
 * checksum folds back to 0 over the covered bytes).
 *
 * Uses an always-active CHECK (not assert()) so a Release / -DNDEBUG build
 * cannot silently no-op the assertions.
 */

#include "icmp.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_failed = 0;

#define CHECK(cond)                                                         \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            g_failed++;                                                     \
        }                                                                   \
    } while (0)

/* ── Capture callback (matches mqvpn_tun_output_fn) ── */

static struct {
    int called;
    uint8_t buf[2048];
    size_t len;
} g_cap;

static void
capture(const uint8_t *pkt, size_t len, void *ctx)
{
    (void)ctx;
    g_cap.called++;
    g_cap.len = len;
    if (len <= sizeof(g_cap.buf)) memcpy(g_cap.buf, pkt, len);
}

static void
reset_cap(void)
{
    memset(&g_cap, 0, sizeof(g_cap));
}

/* ── Independent checksum recomputations ──
 * ref_inet_fold matches icmp.c's ICMP-body folding (the `if (i+1<len)` odd-byte
 * guard at icmp.c:96) and, for the even-length IPv4 header verified here,
 * yields the same value as ipv4_checksum(). ref_icmpv6_cksum is a line-for-line
 * mirror of icmpv6_checksum(). A valid checksum folds back to 0 over its bytes. */

static uint16_t
ref_inet_fold(const uint8_t *p, size_t len)
{
    uint32_t s = 0;
    for (size_t i = 0; i < len; i += 2) {
        s += ((uint32_t)p[i] << 8);
        if (i + 1 < len) s += p[i + 1];
    }
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return ~(uint16_t)s;
}

static uint16_t
ref_icmpv6_cksum(const uint8_t *src6, const uint8_t *dst6, const uint8_t *icmp,
                 size_t icmpv6_len)
{
    uint32_t s = 0;
    for (int i = 0; i < 16; i += 2)
        s += ((uint32_t)src6[i] << 8) | src6[i + 1];
    for (int i = 0; i < 16; i += 2)
        s += ((uint32_t)dst6[i] << 8) | dst6[i + 1];
    s += (uint32_t)(icmpv6_len >> 16);
    s += (uint32_t)(icmpv6_len & 0xFFFF);
    s += 58;
    for (size_t i = 0; i < icmpv6_len; i += 2) {
        s += ((uint32_t)icmp[i] << 8);
        if (i + 1 < icmpv6_len) s += icmp[i + 1];
    }
    while (s >> 16)
        s = (s & 0xFFFF) + (s >> 16);
    return ~(uint16_t)s;
}

/* ── IPv4 ── */

static void
test_v4_ptb_layout_and_checksums(void)
{
    reset_cap();
    /* 20-byte IPv4 header, IHL=5, original sender 10.0.0.9. */
    uint8_t orig[20];
    memset(orig, 0, sizeof(orig));
    orig[0] = 0x45;
    orig[12] = 10;
    orig[13] = 0;
    orig[14] = 0;
    orig[15] = 9;
    const uint8_t src_ip[4] = {10, 0, 0, 1};

    mqvpn_icmp_send_v4(capture, NULL, src_ip, 3, 4, 1400, orig, sizeof(orig));

    CHECK(g_cap.called == 1);
    /* icmp_data_len = min(ihl+8, orig_len) = min(28,20) = 20; total = 20+8+20 */
    CHECK(g_cap.len == 48);

    const uint8_t *pkt = g_cap.buf;
    CHECK(pkt[0] == 0x45);                      /* version/IHL */
    CHECK(((pkt[2] << 8) | pkt[3]) == 48);      /* total length field */
    CHECK(pkt[8] == 64);                        /* TTL = MQVPN_ICMP_TTL */
    CHECK(pkt[9] == 1);                         /* protocol = ICMP */
    CHECK(memcmp(pkt + 12, src_ip, 4) == 0);    /* source = our src_ip */
    CHECK(memcmp(pkt + 16, orig + 12, 4) == 0); /* dest = original sender */

    const uint8_t *icmp = pkt + 20;
    CHECK(icmp[0] == 3);                       /* type */
    CHECK(icmp[1] == 4);                       /* code */
    CHECK(((icmp[6] << 8) | icmp[7]) == 1400); /* next-hop MTU */

    /* Header checksum valid -> folds to 0 over the 20-byte header. */
    CHECK(ref_inet_fold(pkt, 20) == 0);
    /* ICMP checksum valid -> folds to 0 over the 8+20 ICMP bytes. */
    CHECK(ref_inet_fold(icmp, 8 + 20) == 0);
}

static void
test_v4_time_exceeded_zero_mtu(void)
{
    reset_cap();
    /* Source 10.0.0.9: an all-zero source does not name a host and would
     * (correctly) get no ICMP error at all. */
    uint8_t orig[20] = {0x45, [12] = 10, [15] = 9};
    const uint8_t src_ip[4] = {192, 168, 1, 1};

    mqvpn_icmp_send_v4(capture, NULL, src_ip, 11, 0, 0, orig, sizeof(orig));

    CHECK(g_cap.called == 1);
    const uint8_t *icmp = g_cap.buf + 20;
    CHECK(icmp[0] == 11);
    CHECK(icmp[1] == 0);
    CHECK(icmp[6] == 0 && icmp[7] == 0); /* MTU bytes zero for non-PTB */
    CHECK(ref_inet_fold(g_cap.buf, 20) == 0);
}

static void
test_v4_quote_len_caps_at_ihl_plus_8(void)
{
    reset_cap();
    /* 60-byte original, IHL=5 -> quote = ihl+8 = 28 (not the whole 60). */
    uint8_t orig[60];
    memset(orig, 0, sizeof(orig));
    orig[0] = 0x45;
    orig[12] = 10; /* source 10.0.0.9 */
    orig[15] = 9;

    mqvpn_icmp_send_v4(capture, NULL, (const uint8_t[]){1, 2, 3, 4}, 3, 4, 1400, orig,
                       sizeof(orig));

    CHECK(g_cap.called == 1);
    CHECK(g_cap.len == 20 + 8 + 28); /* = 56 */
}

static void
test_v4_guards(void)
{
    uint8_t orig[20] = {0x45};
    const uint8_t src_ip[4] = {1, 1, 1, 1};

    /* NULL callback -> no output. */
    reset_cap();
    mqvpn_icmp_send_v4(NULL, NULL, src_ip, 3, 4, 1400, orig, sizeof(orig));
    CHECK(g_cap.called == 0);

    /* orig_len below the minimum IPv4 header -> dropped. */
    reset_cap();
    mqvpn_icmp_send_v4(capture, NULL, src_ip, 3, 4, 1400, orig, 10);
    CHECK(g_cap.called == 0);

    /* IHL claims 24 bytes but only 20 present -> dropped. */
    reset_cap();
    uint8_t bad[20] = {0x46}; /* IHL=6 -> 24 bytes */
    mqvpn_icmp_send_v4(capture, NULL, src_ip, 3, 4, 1400, bad, sizeof(bad));
    CHECK(g_cap.called == 0);

    /* NULL orig / NULL src_ip -> dropped. */
    reset_cap();
    mqvpn_icmp_send_v4(capture, NULL, NULL, 3, 4, 1400, orig, sizeof(orig));
    CHECK(g_cap.called == 0);
    mqvpn_icmp_send_v4(capture, NULL, src_ip, 3, 4, 1400, NULL, sizeof(orig));
    CHECK(g_cap.called == 0);
}

/* ── IPv6 ── */

static void
test_v6_ptb_layout_and_checksum(void)
{
    reset_cap();
    /* 40-byte IPv6 header; original sender in orig[8..23]. */
    uint8_t orig[40];
    memset(orig, 0, sizeof(orig));
    orig[0] = 0x60;
    for (int i = 0; i < 16; i++)
        orig[8 + i] = (uint8_t)(0xA0 + i); /* original source addr */
    uint8_t src6[16];
    for (int i = 0; i < 16; i++)
        src6[i] = (uint8_t)(0x10 + i);

    mqvpn_icmp_send_v6(capture, NULL, src6, 2, 0, 1400, orig, sizeof(orig));

    CHECK(g_cap.called == 1);
    /* icmpv6_len = 8 + 40 = 48; total = 40 + 48 = 88 (well under 1280). */
    CHECK(g_cap.len == 88);

    const uint8_t *pkt = g_cap.buf;
    CHECK(pkt[0] == 0x60);                      /* version */
    CHECK(pkt[6] == 58);                        /* next header = ICMPv6 */
    CHECK(pkt[7] == 64);                        /* hop limit */
    CHECK(memcmp(pkt + 8, src6, 16) == 0);      /* source = our src6 */
    CHECK(memcmp(pkt + 24, orig + 8, 16) == 0); /* dest = original sender */

    const uint8_t *icmp = pkt + 40;
    CHECK(icmp[0] == 2); /* type = Packet Too Big */
    CHECK(icmp[1] == 0); /* code */
    /* 32-bit MTU field (bytes 4..7). */
    CHECK(((uint32_t)icmp[4] << 24 | (uint32_t)icmp[5] << 16 | (uint32_t)icmp[6] << 8 |
           icmp[7]) == 1400);

    /* ICMPv6 checksum valid -> folds to 0 over pseudo-header + payload. */
    CHECK(ref_icmpv6_cksum(pkt + 8, pkt + 24, icmp, 48) == 0);
}

static void
test_v6_truncates_to_min_mtu(void)
{
    reset_cap();
    /* Oversized original forces the quote down so the whole packet fits the
     * 1280-byte IPv6 minimum MTU: total = 40 + 8 + (1280-48) = 1280. */
    uint8_t orig[2000];
    memset(orig, 0, sizeof(orig));
    orig[0] = 0x60;
    orig[8] = 0xFD; /* source fd00::9 (an unspecified source gets no error) */
    orig[23] = 9;

    mqvpn_icmp_send_v6(capture, NULL, (const uint8_t[16]){0}, 2, 0, 1400, orig,
                       sizeof(orig));

    CHECK(g_cap.called == 1);
    CHECK(g_cap.len == 1280);
}

static void
test_v6_guards(void)
{
    uint8_t orig[40] = {0x60};
    uint8_t src6[16] = {0};

    reset_cap();
    mqvpn_icmp_send_v6(NULL, NULL, src6, 2, 0, 1400, orig, sizeof(orig));
    CHECK(g_cap.called == 0);

    reset_cap();
    mqvpn_icmp_send_v6(capture, NULL, src6, 2, 0, 1400, orig, 30); /* < 40 */
    CHECK(g_cap.called == 0);

    reset_cap();
    mqvpn_icmp_send_v6(capture, NULL, NULL, 2, 0, 1400, orig, sizeof(orig));
    CHECK(g_cap.called == 0);
    mqvpn_icmp_send_v6(capture, NULL, src6, 2, 0, 1400, NULL, sizeof(orig));
    CHECK(g_cap.called == 0);
}

/* ── Error eligibility (RFC 1122 §3.2.2 / RFC 4443 §2.4(e)) ── */

/* A plain UDP datagram 10.0.0.9 -> 192.0.2.1 that may draw an error. */
static void
make_v4(uint8_t *p, size_t len)
{
    memset(p, 0, len);
    p[0] = 0x45;
    p[9] = 17;
    p[12] = 10;
    p[15] = 9;
    p[16] = 192;
    p[18] = 2;
    p[19] = 1;
}

static int
v4_sent(const uint8_t *orig, size_t len)
{
    reset_cap();
    mqvpn_icmp_send_v4(capture, NULL, (const uint8_t[]){10, 0, 0, 1}, 11, 0, 0, orig,
                       len);
    return g_cap.called;
}

static void
test_v4_error_eligibility(void)
{
    uint8_t p[48];

    make_v4(p, sizeof(p));
    CHECK(mqvpn_icmp_error_allowed_v4(p, sizeof(p)) == 1);
    CHECK(v4_sent(p, sizeof(p)) == 1);

    /* Non-first fragment (offset 185 * 8); a first fragment (MF only) is fine. */
    make_v4(p, sizeof(p));
    p[6] = 0x00;
    p[7] = 0xB9;
    CHECK(v4_sent(p, sizeof(p)) == 0);
    p[6] = 0x20; /* MF, offset 0 */
    p[7] = 0x00;
    CHECK(v4_sent(p, sizeof(p)) == 1);

    /* Multicast and limited-broadcast destinations. */
    make_v4(p, sizeof(p));
    p[16] = 224;
    CHECK(v4_sent(p, sizeof(p)) == 0);
    memset(p + 16, 255, 4);
    CHECK(v4_sent(p, sizeof(p)) == 0);

    /* Sources that do not name a single host. */
    make_v4(p, sizeof(p));
    memset(p + 12, 0, 4);
    CHECK(v4_sent(p, sizeof(p)) == 0);
    p[12] = 127;
    CHECK(v4_sent(p, sizeof(p)) == 0);
    p[12] = 239;
    CHECK(v4_sent(p, sizeof(p)) == 0);
    p[12] = 240;
    CHECK(v4_sent(p, sizeof(p)) == 0);

    /* ICMP errors get none, ICMP informational messages do. */
    make_v4(p, sizeof(p));
    p[9] = 1;
    const uint8_t errs[] = {3, 4, 5, 11, 12};
    for (size_t i = 0; i < sizeof(errs); i++) {
        p[20] = errs[i];
        CHECK(v4_sent(p, sizeof(p)) == 0);
    }
    p[20] = 8; /* echo request */
    CHECK(v4_sent(p, sizeof(p)) == 1);
    /* ICMP header not even present: cannot tell, so no error. */
    CHECK(v4_sent(p, 20) == 0);
}

/* A plain UDP datagram fd00::9 -> 2001:db8::1. */
static void
make_v6(uint8_t *p, size_t len)
{
    memset(p, 0, len);
    p[0] = 0x60;
    p[6] = 17;
    p[8] = 0xFD;
    p[23] = 9;
    p[24] = 0x20;
    p[25] = 0x01;
    p[26] = 0x0d;
    p[27] = 0xb8;
    p[39] = 1;
}

static int
v6_sent(const uint8_t *orig, size_t len, uint8_t type, uint8_t code)
{
    reset_cap();
    mqvpn_icmp_send_v6(capture, NULL, (const uint8_t[16]){0xFD, [15] = 1}, type, code,
                       1280, orig, len);
    return g_cap.called;
}

static void
test_v6_error_eligibility(void)
{
    uint8_t p[96];

    make_v6(p, sizeof(p));
    CHECK(mqvpn_icmp_error_allowed_v6(p, sizeof(p), 3, 0) == 1);
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 1);

    /* Unspecified and multicast sources. */
    make_v6(p, sizeof(p));
    memset(p + 8, 0, 16);
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 0);
    p[8] = 0xFF;
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 0);

    /* Multicast destination: Time Exceeded no, Packet Too Big yes. */
    make_v6(p, sizeof(p));
    p[24] = 0xFF;
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 0);
    CHECK(v6_sent(p, sizeof(p), 2, 0) == 1);

    /* ICMPv6 error (Destination Unreachable) vs informational (Echo). */
    make_v6(p, sizeof(p));
    p[6] = 58;
    p[40] = 1;
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 0);
    p[40] = 128;
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 1);

    /* Behind a Hop-by-Hop header (8 bytes), an ICMPv6 error is still seen. */
    make_v6(p, sizeof(p));
    p[6] = 0;
    p[40] = 58; /* next header */
    p[41] = 0;  /* (0 + 1) * 8 bytes */
    p[48] = 2;  /* ICMPv6 Packet Too Big */
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 0);

    /* Fragment header: a non-first fragment gets none, the first does. */
    make_v6(p, sizeof(p));
    p[6] = 44;
    p[40] = 17;   /* next header UDP */
    p[42] = 0x05; /* offset 0x5A8 >> 3 = 181 */
    p[43] = 0xA8;
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 0);
    p[42] = 0x00;
    p[43] = 0x01; /* offset 0, M flag */
    CHECK(v6_sent(p, sizeof(p), 3, 0) == 1);
}

int
main(void)
{
    test_v4_ptb_layout_and_checksums();
    test_v4_time_exceeded_zero_mtu();
    test_v4_quote_len_caps_at_ihl_plus_8();
    test_v4_guards();
    test_v6_ptb_layout_and_checksum();
    test_v6_truncates_to_min_mtu();
    test_v6_guards();
    test_v4_error_eligibility();
    test_v6_error_eligibility();

    if (g_failed) {
        fprintf(stderr, "test_icmp: %d CHECK(s) FAILED\n", g_failed);
        return 1;
    }
    printf("test_icmp: all OK\n");
    return 0;
}
