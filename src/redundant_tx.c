// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#include "redundant_tx.h"

#include "libmqvpn.h" /* MQVPN_MAX_PATHS */

#include <stdlib.h>
#include <string.h>

#define RTX_CAP       16384 /* power of two; both tables */
#define RTX_LIMIT_MIN 300
#define RTX_RTT_SLACK 250000u /* us added to srtt in the limit */

/* ---- port list / packet matching ---- */

int
mqvpn_redundant_parse_ports(const char *s, uint16_t *out, int max)
{
    if (!s) return 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
        p++;
    if (!*p) return 0;

    int n_tok = 0, n = 0;
    for (p = s;; p++) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        unsigned v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') {
            if (v <= 65535) v = v * 10 + (unsigned)(*p - '0');
            digits++;
            p++;
        }
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')
            p++;
        if (digits == 0 || v < 1 || v > 65535) return -1;
        if (++n_tok > max) return -1;
        int dup = 0;
        for (int i = 0; i < n; i++)
            if (out[i] == v) dup = 1;
        if (!dup) out[n++] = (uint16_t)v;
        if (*p == '\0') return n;
        if (*p != ',') return -1;
    }
}

int
mqvpn_redundant_match(const uint16_t *ports, int n_ports, const uint8_t *pkt, size_t len)
{
    if (!pkt || len < 1) return 0;
    size_t off;
    if ((pkt[0] >> 4) == 4) {
        if (len < 20 || pkt[9] != 17) return 0;
        if (n_ports <= 0) return 1;
        if ((((unsigned)pkt[6] << 8 | pkt[7]) & 0x1fff) != 0) return 0; /* non-first */
        off = (size_t)(pkt[0] & 0x0f) * 4;
        if (off < 20) return 0;
    } else if ((pkt[0] >> 4) == 6) {
        /* No extension-header walk, same as flow_hash_pkt. */
        if (len < 40 || pkt[6] != 17) return 0;
        if (n_ports <= 0) return 1;
        off = 40;
    } else {
        return 0;
    }
    if (off + 4 > len) return 0;
    uint16_t dport = (uint16_t)((pkt[off + 2] << 8) | pkt[off + 3]);
    for (int i = 0; i < n_ports; i++)
        if (ports[i] == dport) return 1;
    return 0;
}

/* ---- guard ---- */

/* Open-addressing table (linear probe, backward-shift delete, no eviction).
 * Copies: key = dgram_id, a = original key, b = path (-1 detached).
 * Originals: key = internal counter, a = unresolved copies, b = acked flag. */
typedef struct {
    uint64_t key;
    uint64_t a;
    int32_t b;
    uint8_t used;
} rtx_ent_t;

typedef struct {
    rtx_ent_t *e;
    int n;
} rtx_tab_t;

typedef struct {
    int bound;
    uint64_t xqc_id;
    int outstanding;
    uint64_t srtt_us;
} rtx_path_t;

struct mqvpn_redundant_tx_s {
    rtx_tab_t copies, origs;
    uint64_t next_orig;
    rtx_path_t path[MQVPN_MAX_PATHS];
    int have_win;
    uint64_t win_sec;
    uint32_t win_count, prev_count; /* originals in win_sec / the second before */
};

static uint32_t
rtx_slot(uint64_t k)
{
    k *= 0x9E3779B97F4A7C15ull;
    return (uint32_t)(k >> 40) & (RTX_CAP - 1);
}

static rtx_ent_t *
tab_find(rtx_tab_t *t, uint64_t key)
{
    for (uint32_t i = rtx_slot(key);; i = (i + 1) & (RTX_CAP - 1)) {
        if (!t->e[i].used) return NULL;
        if (t->e[i].key == key) return &t->e[i];
    }
}

static rtx_ent_t *
tab_insert(rtx_tab_t *t, uint64_t key)
{
    if (t->n >= RTX_CAP) return NULL;
    uint32_t i = rtx_slot(key);
    while (t->e[i].used)
        i = (i + 1) & (RTX_CAP - 1);
    memset(&t->e[i], 0, sizeof(t->e[i]));
    t->e[i].used = 1;
    t->e[i].key = key;
    t->n++;
    return &t->e[i];
}

static void
tab_remove(rtx_tab_t *t, rtx_ent_t *ent)
{
    uint32_t i = (uint32_t)(ent - t->e), j = i;
    for (;;) {
        j = (j + 1) & (RTX_CAP - 1);
        if (!t->e[j].used) break;
        uint32_t h = rtx_slot(t->e[j].key);
        /* entry j may move to hole i unless its home lies cyclically in (i, j] */
        if (i <= j ? (h > i && h <= j) : (h > i || h <= j)) continue;
        t->e[i] = t->e[j];
        i = j;
    }
    t->e[i].used = 0;
    t->n--;
}

mqvpn_redundant_tx_t *
mqvpn_redundant_tx_new(void)
{
    mqvpn_redundant_tx_t *t = calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->copies.e = calloc(RTX_CAP, sizeof(rtx_ent_t));
    t->origs.e = calloc(RTX_CAP, sizeof(rtx_ent_t));
    if (!t->copies.e || !t->origs.e) {
        mqvpn_redundant_tx_free(t);
        return NULL;
    }
    return t;
}

void
mqvpn_redundant_tx_free(mqvpn_redundant_tx_t *t)
{
    if (!t) return;
    free(t->copies.e);
    free(t->origs.e);
    free(t);
}

static int
path_ok(int path)
{
    return path >= 0 && path < MQVPN_MAX_PATHS;
}

void
mqvpn_redundant_tx_detach_path(mqvpn_redundant_tx_t *t, int path)
{
    if (!t || !path_ok(path)) return;
    /* shortcut: full-table scan, slot loss/reuse is rare; index per path if it shows up
     * in profiles */
    for (int i = 0; i < RTX_CAP; i++)
        if (t->copies.e[i].used && t->copies.e[i].b == path) t->copies.e[i].b = -1;
    t->path[path].outstanding = 0;
}

void
mqvpn_redundant_tx_bind_path(mqvpn_redundant_tx_t *t, int path, uint64_t xqc_path_id)
{
    if (!t || !path_ok(path)) return;
    rtx_path_t *p = &t->path[path];
    if (p->bound && p->xqc_id != xqc_path_id) mqvpn_redundant_tx_detach_path(t, path);
    p->bound = 1;
    p->xqc_id = xqc_path_id;
}

void
mqvpn_redundant_tx_set_srtt(mqvpn_redundant_tx_t *t, int path, uint64_t srtt_us)
{
    if (t && path_ok(path)) t->path[path].srtt_us = srtt_us;
}

int
mqvpn_redundant_tx_outstanding(const mqvpn_redundant_tx_t *t, int path)
{
    return (t && path_ok(path)) ? t->path[path].outstanding : 0;
}

uint32_t
mqvpn_redundant_tx_limit(const mqvpn_redundant_tx_t *t, int path)
{
    if (!t || !path_ok(path)) return RTX_LIMIT_MIN;
    /* shortcut: pps is the second before the latest commit; an idle gap keeps the
     * last value until the next commit rolls the window. */
    uint64_t v =
        (uint64_t)t->prev_count * (t->path[path].srtt_us + RTX_RTT_SLACK) / 1000000u;
    if (v < RTX_LIMIT_MIN) v = RTX_LIMIT_MIN;
    return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v;
}

int
mqvpn_redundant_tx_select(mqvpn_redundant_tx_t *t, const int *usable, int n_usable,
                          int *out)
{
    if (!t || !usable || !out || n_usable <= 0) return 0;
    if (t->origs.n >= RTX_CAP) return 0;
    int n = 0, best = -1;
    for (int i = 0; i < n_usable; i++) {
        int p = usable[i];
        if (!path_ok(p)) continue;
        int o = t->path[p].outstanding;
        if (best < 0 || o < t->path[best].outstanding ||
            (o == t->path[best].outstanding && p < best))
            best = p;
        if ((uint32_t)o < mqvpn_redundant_tx_limit(t, p)) out[n++] = p;
    }
    if (n == 0 && best >= 0) out[n++] = best;
    int room = RTX_CAP - t->copies.n;
    return n < room ? n : room;
}

void
mqvpn_redundant_tx_commit(mqvpn_redundant_tx_t *t, const uint64_t *dgram_ids,
                          const int *paths, int k, uint64_t now_us)
{
    if (!t || k <= 0 || t->origs.n >= RTX_CAP || t->copies.n + k > RTX_CAP) return;
    uint64_t sec = now_us / 1000000u;
    if (!t->have_win || sec != t->win_sec) {
        t->prev_count = (t->have_win && sec == t->win_sec + 1) ? t->win_count : 0;
        t->have_win = 1;
        t->win_sec = sec;
        t->win_count = 0;
    }
    t->win_count++;

    rtx_ent_t *o = tab_insert(&t->origs, ++t->next_orig);
    o->a = (uint64_t)k;
    for (int i = 0; i < k; i++) {
        rtx_ent_t *c = tab_insert(&t->copies, dgram_ids[i]);
        c->a = o->key;
        c->b = path_ok(paths[i]) ? paths[i] : -1;
        if (c->b >= 0) t->path[c->b].outstanding++;
    }
}

/* Drop one resolved copy; returns its original (NULL if untracked). */
static rtx_ent_t *
resolve(mqvpn_redundant_tx_t *t, uint64_t dgram_id, int *last)
{
    rtx_ent_t *c = tab_find(&t->copies, dgram_id);
    if (!c) return NULL;
    rtx_ent_t *o = tab_find(&t->origs, c->a);
    if (c->b >= 0) t->path[c->b].outstanding--;
    tab_remove(&t->copies, c);
    *last = (--o->a == 0);
    return o;
}

int
mqvpn_redundant_tx_on_acked(mqvpn_redundant_tx_t *t, uint64_t dgram_id)
{
    int last;
    rtx_ent_t *o = t ? resolve(t, dgram_id, &last) : NULL;
    if (!o) return -1;
    int first = !o->b;
    o->b = 1;
    if (last) tab_remove(&t->origs, o);
    return first;
}

int
mqvpn_redundant_tx_on_lost(mqvpn_redundant_tx_t *t, uint64_t dgram_id)
{
    int last;
    rtx_ent_t *o = t ? resolve(t, dgram_id, &last) : NULL;
    if (!o) return -1;
    int r = last && !o->b;
    if (last) tab_remove(&t->origs, o);
    return r;
}
