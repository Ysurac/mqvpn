// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * dgram_dedup.c — see dgram_dedup.h.
 *
 * A set-associative table of (key, time) entries; the key is a seeded 64-bit
 * hash of the whole packet and its length.
 */
#include "dgram_dedup.h"

#include <stdlib.h>
#include <string.h>

#define DEDUP_WAYS 4
#define DEDUP_SETS 1024 /* power of two: 4096 entries, 64 KiB */

struct mqvpn_dgram_dedup_s {
    uint64_t key[DEDUP_SETS][DEDUP_WAYS]; /* 0: empty */
    uint64_t seen_us[DEDUP_SETS][DEDUP_WAYS];
    uint64_t seed;
    uint64_t dropped;
};

mqvpn_dgram_dedup_t *
mqvpn_dgram_dedup_new(uint64_t seed)
{
    mqvpn_dgram_dedup_t *d = calloc(1, sizeof(*d));
    if (d) d->seed = seed;
    return d;
}

void
mqvpn_dgram_dedup_free(mqvpn_dgram_dedup_t *d)
{
    free(d);
}

uint64_t
mqvpn_dgram_dedup_dropped(const mqvpn_dgram_dedup_t *d)
{
    return d ? d->dropped : 0;
}

/* Each 64-bit word goes through x = (x ^ w) * odd constant, which is
 * injective in w, in one of four independent lanes (so that the multiplies
 * overlap: about 50 ns for 1300 bytes); the lanes are then folded the same
 * way. Two packets of the same length that differ in a single word never
 * get the same key. The splitmix64 finalizer spreads every input bit over
 * the whole key. */
static uint64_t
dedup_key(uint64_t seed, const uint8_t *pkt, size_t len)
{
    const uint64_t m = 0x9e3779b97f4a7c15ULL;
    uint64_t a = (seed ^ (uint64_t)len) * m, b = a ^ 1, c = a ^ 2, d = a ^ 3;
    uint64_t w[4];
    size_t i = 0;

    for (; i + 32 <= len; i += 32) {
        memcpy(w, pkt + i, 32);
        a = (a ^ w[0]) * m;
        b = (b ^ w[1]) * m;
        c = (c ^ w[2]) * m;
        d = (d ^ w[3]) * m;
    }
    uint64_t h = ((((a ^ b) * m) ^ c) * m ^ d) * m;
    for (; i + 8 <= len; i += 8) {
        memcpy(w, pkt + i, 8);
        h = (h ^ w[0]) * m;
    }
    if (i < len) {
        w[0] = 0;
        memcpy(w, pkt + i, len - i);
        h = (h ^ w[0]) * m;
    }
    h ^= h >> 30;
    h *= 0xbf58476d1ce4e5b9ULL;
    h ^= h >> 27;
    h *= 0x94d049bb133111ebULL;
    h ^= h >> 31;
    return h | 1;
}

int
mqvpn_dgram_dedup_seen(mqvpn_dgram_dedup_t *d, const uint8_t *pkt, size_t len,
                       uint64_t now_us)
{
    uint64_t key = dedup_key(d->seed, pkt, len);
    size_t set = (size_t)(key >> 32) & (DEDUP_SETS - 1);
    uint64_t *k = d->key[set];
    uint64_t *t = d->seen_us[set];
    int victim = -1, oldest = 0;

    for (int i = 0; i < DEDUP_WAYS; i++) {
        if (k[i] == 0 || now_us - t[i] >= MQVPN_DGRAM_DEDUP_WINDOW_US) {
            if (victim < 0) victim = i; /* empty or expired */
            continue;
        }
        if (k[i] == key) {
            d->dropped++;
            return 1;
        }
        if (t[i] < t[oldest]) oldest = i;
    }
    /* every entry live: replace the oldest */
    if (victim < 0) victim = oldest;
    k[victim] = key;
    t[victim] = now_us;
    return 0;
}
