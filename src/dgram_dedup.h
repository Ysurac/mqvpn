// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * dgram_dedup.h — let one copy of an inner packet through when several
 * arrive.
 *
 * The redundant scheduler sends every packet on every usable path. A
 * DATAGRAM frame carries no identifier (RFC 9221 §4), so the receiver gets
 * one frame per path and, without this filter, writes the same inner IP
 * packet to the TUN once per path. The filter remembers a seeded 64-bit hash
 * of each packet it lets through and drops a packet with the same hash that
 * arrives less than MQVPN_DGRAM_DEDUP_WINDOW_US later.
 *
 * The window is short so that a packet the application itself sends again
 * later (a retransmission, a repeated probe) still goes through: the copies
 * the scheduler made arrive within the difference between the paths' delays.
 * The table has a fixed size; when it is full the oldest entries go first, so
 * at a very high packet rate some copies get through instead of being
 * dropped, and a packet is never dropped for a reason other than a match.
 */
#ifndef MQVPN_DGRAM_DEDUP_H
#define MQVPN_DGRAM_DEDUP_H

#include <stddef.h>
#include <stdint.h>

#define MQVPN_DGRAM_DEDUP_WINDOW_US (200 * 1000)

typedef struct mqvpn_dgram_dedup_s mqvpn_dgram_dedup_t;

/* NULL on allocation failure (callers then deliver every packet). */
mqvpn_dgram_dedup_t *mqvpn_dgram_dedup_new(uint64_t seed);
void mqvpn_dgram_dedup_free(mqvpn_dgram_dedup_t *d);

/* 1: the same packet (same length and bytes, up to a 64-bit hash) was let
 * through less than MQVPN_DGRAM_DEDUP_WINDOW_US before now_us; drop this one.
 * 0: let it through (it is recorded). */
int mqvpn_dgram_dedup_seen(mqvpn_dgram_dedup_t *d, const uint8_t *pkt, size_t len,
                           uint64_t now_us);

/* Packets dropped as copies so far. */
uint64_t mqvpn_dgram_dedup_dropped(const mqvpn_dgram_dedup_t *d);

#endif /* MQVPN_DGRAM_DEDUP_H */
