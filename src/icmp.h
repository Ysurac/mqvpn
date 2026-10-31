// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#ifndef MQVPN_ICMP_H
#define MQVPN_ICMP_H

#include "libmqvpn.h" /* for mqvpn_tun_output_fn typedef */

/* Constants */
#define IPV4_MIN_HDR   20
#define IPV6_MIN_HDR   40
#define IPV6_MIN_MTU   1280
#define MQVPN_ICMP_TTL 64

/* Uses existing mqvpn_tun_output_fn from libmqvpn.h */

/*
 * Whether an ICMP error may be sent about `orig` (RFC 1122 §3.2.2): 0 for a
 * non-first fragment, a multicast or limited-broadcast destination, a source
 * that does not name a single host (zero, loopback, multicast, class E), an
 * ICMP error message, or a malformed header; 1 otherwise. The send functions
 * below apply it themselves; callers that rate-limit check it first so a
 * suppressed error does not use up a token.
 */
int mqvpn_icmp_error_allowed_v4(const uint8_t *orig, size_t orig_len);

/*
 * IPv6 counterpart (RFC 4443 §2.4(e)) for an error of (type, code): 0 for an
 * unspecified or multicast source, a multicast destination (except Packet
 * Too Big and Parameter Problem code 2), a non-first fragment, an ICMPv6
 * error message (type < 128) or a short header; 1 otherwise.
 */
int mqvpn_icmp_error_allowed_v6(const uint8_t *orig, size_t orig_len, uint8_t type,
                                uint8_t code);

/*
 * Send ICMPv4 error message.  Generic (type, code, mtu) supports:
 *   type=3,code=4,mtu=N  -> Fragmentation Needed (PTB)
 *   type=3,code=1,mtu=0  -> Host Unreachable (Dest Unreach)
 *   type=11,code=0,mtu=0 -> Time Exceeded
 * Bytes 6-7 of ICMP body: MTU for PTB (type=3,code=4), zero for others.
 * src_ip: 4-byte source IP for the ICMP response.
 * orig/orig_len: the original packet that triggered the ICMP.
 * The ICMP destination IP is derived from orig[12..15] (original sender).
 */
void mqvpn_icmp_send_v4(mqvpn_tun_output_fn cb, void *ctx, const uint8_t *src_ip,
                        uint8_t type, uint8_t code, uint16_t mtu, const uint8_t *orig,
                        size_t orig_len);

/*
 * Send ICMPv6 error message.  Generic (type, code, mtu) supports:
 *   type=2,code=0,mtu=N  -> Packet Too Big
 *   type=1,code=3,mtu=0  -> Address Unreachable (Dest Unreach)
 *   type=3,code=0,mtu=0  -> Time Exceeded
 * Bytes 4-7 of ICMPv6 body: MTU for PTB (type=2), zero for others.
 * src_ip6: 16-byte source IPv6 address.
 * The ICMPv6 destination is derived from orig[8..23] (original sender).
 */
void mqvpn_icmp_send_v6(mqvpn_tun_output_fn cb, void *ctx, const uint8_t *src_ip6,
                        uint8_t type, uint8_t code, uint32_t mtu, const uint8_t *orig,
                        size_t orig_len);

#endif /* MQVPN_ICMP_H */
