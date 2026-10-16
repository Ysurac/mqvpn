// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * redundant_tx.h — redundant-mode uplink: port-list parsing, packet matching,
 * the per-path outstanding-copy guard and overload mode. Pure logic (no xquic,
 * no sockets).
 */
#ifndef MQVPN_REDUNDANT_TX_H
#define MQVPN_REDUNDANT_TX_H

#include <stddef.h>
#include <stdint.h>

/* Parse "p1, p2, ..." into out[] (distinct ports, first-seen order).
 * Returns the number written (0 for NULL / empty / whitespace-only), or -1 on
 * an empty or non-numeric token, a port outside 1-65535, or more than `max`
 * tokens (counted before duplicates are removed). */
int mqvpn_redundant_parse_ports(const char *s, uint16_t *out, int max);

/* 1 when the IP packet is a redundant-mode candidate: IPv4/IPv6 UDP, and, with
 * a port list, a readable destination port in the list. */
int mqvpn_redundant_match(const uint16_t *ports, int n_ports, const uint8_t *pkt,
                          size_t len);

typedef struct mqvpn_redundant_tx_s mqvpn_redundant_tx_t;

mqvpn_redundant_tx_t *mqvpn_redundant_tx_new(void);
void mqvpn_redundant_tx_free(mqvpn_redundant_tx_t *t);

/* Record which xquic path a slot carries; a different id than the recorded
 * one (slot reused) detaches the slot's existing copies first. */
void mqvpn_redundant_tx_bind_path(mqvpn_redundant_tx_t *t, int path,
                                  uint64_t xqc_path_id);
/* The slot is no longer usable: its copies stop counting (outstanding -> 0)
 * but stay tracked and resolve their original normally. */
void mqvpn_redundant_tx_detach_path(mqvpn_redundant_tx_t *t, int path);

/* Choose the paths (indices from `usable`, ascending) to send one packet on.
 * Skips paths at their limit; if all are at the limit, returns only the least
 * loaded one. Records the paths it skips at their limit (an input of
 * update_mode). Truncated to the free copy-table capacity; 0 when the original
 * table is full. Reserves nothing: call commit before the next select. */
int mqvpn_redundant_tx_select(mqvpn_redundant_tx_t *t, const int *usable, int n_usable,
                              int *out);

/* Record one original and its k >= 1 accepted copies; k == 0 is a no-op. */
void mqvpn_redundant_tx_commit(mqvpn_redundant_tx_t *t, const uint64_t *dgram_ids,
                               const int *paths, int k, uint64_t now_us);

/* -1 not tracked; 1 first ack of its original; 0 original already acked. */
int mqvpn_redundant_tx_on_acked(mqvpn_redundant_tx_t *t, uint64_t dgram_id);
/* -1 not tracked; 1 last unresolved copy and none was acked; 0 otherwise. */
int mqvpn_redundant_tx_on_lost(mqvpn_redundant_tx_t *t, uint64_t dgram_id);

int mqvpn_redundant_tx_outstanding(const mqvpn_redundant_tx_t *t, int path);
uint32_t mqvpn_redundant_tx_limit(const mqvpn_redundant_tx_t *t, int path);

/* Feed a usable path's smoothed RTT (sizes the guard limit), congestion
 * estimate (bytes/s) and bytes in flight (overload judgement); from the 100 ms
 * refresh. */
void mqvpn_redundant_tx_set_path_metrics(mqvpn_redundant_tx_t *t, int path,
                                         uint64_t srtt_us, uint64_t est_bw,
                                         uint64_t inflight);

/* Overload mode: while no usable path can carry the stream alone, redundant
 * packets go out once through the ordinary send. Call from the 100 ms refresh
 * while some path is usable, after set_path_metrics. Returns 1 on entering
 * overload mode, -1 on leaving it (a probe, or a reset: the stream stopped or
 * the clock stepped back), 0 otherwise. */
int mqvpn_redundant_tx_update_mode(mqvpn_redundant_tx_t *t, const int *usable,
                                   int n_usable, uint64_t now_us);

/* 1 in overload mode: the packet counts toward the stream rate and the caller
 * sends it once, untracked. 0 otherwise (nothing counted). */
int mqvpn_redundant_tx_overload_bypass(mqvpn_redundant_tx_t *t, uint64_t now_us);

#endif /* MQVPN_REDUNDANT_TX_H */
