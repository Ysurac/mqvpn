// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#include "icmp.h"

#include <stdint.h>
#include <string.h>

/* ─── Checksum helpers ─── */

static uint16_t
ipv4_checksum(const uint8_t *hdr, size_t len)
{
    uint32_t cksum = 0;
    for (size_t i = 0; i < len; i += 2)
        cksum += ((uint32_t)hdr[i] << 8) | hdr[i + 1];
    while (cksum >> 16)
        cksum = (cksum & 0xFFFF) + (cksum >> 16);
    return ~(uint16_t)cksum;
}

static uint16_t
icmpv6_checksum(const uint8_t *src6, const uint8_t *dst6, const uint8_t *icmp,
                size_t icmpv6_len)
{
    uint32_t cksum = 0;

    /* Pseudo-header: src (16), dst (16), length (4), next-hdr=58 (4) */
    for (int i = 0; i < 16; i += 2)
        cksum += ((uint32_t)src6[i] << 8) | src6[i + 1];
    for (int i = 0; i < 16; i += 2)
        cksum += ((uint32_t)dst6[i] << 8) | dst6[i + 1];
    cksum += (uint32_t)(icmpv6_len >> 16);
    cksum += (uint32_t)(icmpv6_len & 0xFFFF);
    cksum += 58; /* next header = ICMPv6 */

    /* ICMPv6 payload */
    for (size_t i = 0; i < icmpv6_len; i += 2) {
        cksum += ((uint32_t)icmp[i] << 8);
        if (i + 1 < icmpv6_len) cksum += icmp[i + 1];
    }

    while (cksum >> 16)
        cksum = (cksum & 0xFFFF) + (cksum >> 16);
    return ~(uint16_t)cksum;
}

/* ─── Error eligibility (RFC 1122 §3.2.2, RFC 4443 §2.4(e)) ─── */

int
mqvpn_icmp_error_allowed_v4(const uint8_t *orig, size_t orig_len)
{
    if (!orig || orig_len < IPV4_MIN_HDR) return 0;
    size_t ihl = (size_t)(orig[0] & 0x0F) * 4;
    if (ihl < IPV4_MIN_HDR || ihl > orig_len) return 0;

    /* Not about a non-first fragment: it carries no transport header. */
    if ((((uint16_t)orig[6] << 8 | orig[7]) & 0x1FFF) != 0) return 0;

    /* Not to a multicast or limited-broadcast destination. */
    const uint8_t *dst = orig + 16;
    if ((dst[0] & 0xF0) == 0xE0) return 0;
    if (dst[0] == 255 && dst[1] == 255 && dst[2] == 255 && dst[3] == 255) return 0;

    /* Not when the source does not name a single host: zero, loopback,
     * multicast or class E (which includes 255.255.255.255). */
    const uint8_t *src = orig + 12;
    if (src[0] == 0 && src[1] == 0 && src[2] == 0 && src[3] == 0) return 0;
    if (src[0] == 127 || (src[0] & 0xE0) == 0xE0) return 0;

    /* Not about an ICMP error message (an informational one is fine). A
     * datagram too short to show its ICMP type is treated as an error. */
    if (orig[9] == 1) {
        if (orig_len <= ihl) return 0;
        uint8_t t = orig[ihl];
        if (t == 3 || t == 4 || t == 5 || t == 11 || t == 12) return 0;
    }
    return 1;
}

int
mqvpn_icmp_error_allowed_v6(const uint8_t *orig, size_t orig_len, uint8_t type,
                            uint8_t code)
{
    if (!orig || orig_len < IPV6_MIN_HDR) return 0;

    /* Not when the source does not name a single node: unspecified (::) or
     * multicast. */
    const uint8_t *src = orig + 8;
    static const uint8_t zero16[16] = {0};
    if (memcmp(src, zero16, 16) == 0 || src[0] == 0xFF) return 0;

    /* Not to a multicast destination, except Packet Too Big and Parameter
     * Problem code 2 (RFC 4443 §2.4(e.3)). */
    if (orig[24] == 0xFF && !(type == 2 || (type == 4 && code == 2))) return 0;

    /* Walk the extension headers we can see: no error about a non-first
     * fragment, nor about an ICMPv6 error message (types below 128). A walk
     * that runs off the captured bytes cannot tell, and allows the error. */
    uint8_t nh = orig[6];
    size_t off = IPV6_MIN_HDR;
    for (int i = 0; i < 8; i++) {
        if (nh == 58) {
            if (off < orig_len && orig[off] < 128) return 0;
            return 1;
        }
        if (nh == 44) { /* Fragment */
            if (off + 8 > orig_len) return 1;
            if ((((uint16_t)orig[off + 2] << 8 | orig[off + 3]) & 0xFFF8) != 0) return 0;
            nh = orig[off];
            off += 8;
            continue;
        }
        if (nh == 0 || nh == 43 || nh == 60) { /* Hop-by-Hop, Routing, Dest Opts */
            if (off + 2 > orig_len) return 1;
            size_t hlen = ((size_t)orig[off + 1] + 1) * 8;
            nh = orig[off];
            off += hlen;
            continue;
        }
        return 1;
    }
    return 1;
}

/* ─── Public API ─── */

void
mqvpn_icmp_send_v4(mqvpn_tun_output_fn cb, void *ctx, const uint8_t *src_ip, uint8_t type,
                   uint8_t code, uint16_t mtu, const uint8_t *orig, size_t orig_len)
{
    if (!cb || !src_ip || !orig) return;
    if (orig_len < IPV4_MIN_HDR) return;

    size_t ihl = (size_t)(orig[0] & 0x0F) * 4;
    if (ihl < IPV4_MIN_HDR || ihl > orig_len) return;
    if (!mqvpn_icmp_error_allowed_v4(orig, orig_len)) return;

    size_t icmp_data_len = ihl + 8;
    if (icmp_data_len > orig_len) icmp_data_len = orig_len;

    size_t total = IPV4_MIN_HDR + 8 + icmp_data_len;
    uint8_t pkt[128];
    if (total > sizeof(pkt)) return;

    memset(pkt, 0, total);

    /* IPv4 header */
    pkt[0] = 0x45;
    pkt[1] = 0xC0;
    pkt[2] = (total >> 8) & 0xFF;
    pkt[3] = total & 0xFF;
    pkt[8] = MQVPN_ICMP_TTL;
    pkt[9] = 1; /* protocol = ICMP */
    memcpy(pkt + 12, src_ip, 4);
    memcpy(pkt + 16, orig + 12, 4);

    uint16_t ip_ck = ipv4_checksum(pkt, IPV4_MIN_HDR);
    pkt[10] = ip_ck >> 8;
    pkt[11] = ip_ck & 0xFF;

    /* ICMP header + data */
    uint8_t *icmp = pkt + IPV4_MIN_HDR;
    icmp[0] = type;
    icmp[1] = code;
    /* bytes 4-5 unused (zero); bytes 6-7 = MTU for PTB, zero otherwise */
    icmp[6] = mtu >> 8;
    icmp[7] = mtu & 0xFF;
    memcpy(icmp + 8, orig, icmp_data_len);

    size_t icmp_total = 8 + icmp_data_len;
    uint32_t cksum = 0;
    for (size_t i = 0; i < icmp_total; i += 2) {
        cksum += ((uint32_t)icmp[i] << 8);
        if (i + 1 < icmp_total) cksum += icmp[i + 1];
    }
    while (cksum >> 16)
        cksum = (cksum & 0xFFFF) + (cksum >> 16);
    uint16_t ic = ~(uint16_t)cksum;
    icmp[2] = ic >> 8;
    icmp[3] = ic & 0xFF;

    cb(pkt, total, ctx);
}

void
mqvpn_icmp_send_v6(mqvpn_tun_output_fn cb, void *ctx, const uint8_t *src_ip6,
                   uint8_t type, uint8_t code, uint32_t mtu, const uint8_t *orig,
                   size_t orig_len)
{
    if (!cb || !src_ip6 || !orig) return;
    if (orig_len < IPV6_MIN_HDR) return;
    if (!mqvpn_icmp_error_allowed_v6(orig, orig_len, type, code)) return;

    size_t icmpv6_data_len = orig_len;
    if (IPV6_MIN_HDR + 8 + icmpv6_data_len > IPV6_MIN_MTU)
        icmpv6_data_len = IPV6_MIN_MTU - IPV6_MIN_HDR - 8;

    size_t icmpv6_len = 8 + icmpv6_data_len;
    size_t total = IPV6_MIN_HDR + icmpv6_len;

    uint8_t pkt[IPV6_MIN_MTU];
    memset(pkt, 0, total);

    /* IPv6 header */
    pkt[0] = 0x60;
    pkt[4] = (icmpv6_len >> 8) & 0xFF;
    pkt[5] = icmpv6_len & 0xFF;
    pkt[6] = 58; /* next header = ICMPv6 */
    pkt[7] = MQVPN_ICMP_TTL;
    memcpy(pkt + 8, src_ip6, 16);
    memcpy(pkt + 24, orig + 8, 16);

    /* ICMPv6 header + data */
    uint8_t *icmp = pkt + IPV6_MIN_HDR;
    icmp[0] = type;
    icmp[1] = code;
    /* bytes 4-7 = MTU for PTB (type=2), zero for others */
    icmp[4] = (mtu >> 24) & 0xFF;
    icmp[5] = (mtu >> 16) & 0xFF;
    icmp[6] = (mtu >> 8) & 0xFF;
    icmp[7] = mtu & 0xFF;
    memcpy(icmp + 8, orig, icmpv6_data_len);

    uint16_t ic = icmpv6_checksum(pkt + 8, pkt + 24, icmp, icmpv6_len);
    icmp[2] = ic >> 8;
    icmp[3] = ic & 0xFF;

    cb(pkt, total, ctx);
}
