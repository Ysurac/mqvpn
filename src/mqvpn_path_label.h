// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * mqvpn_path_label.h — wire payload for the PATH_LABEL capsule.
 *
 * Problem 1 (persistence): xquic never reuses a QUIC multipath path_id
 * once abandoned (by design — see xqc_conn_is_path_abandoned()), so a
 * full path teardown + recreate (interface replaced, extended outage past
 * the retry budget) always gets a brand new path_id. A server-side weight/
 * dscp_mask assignment keyed by raw path_id does not survive that.
 *
 * Problem 2 (symmetry): QUIC multipath scheduling is per-endpoint-per-
 * direction — a weight/dscp_mask the client sets for itself only ever
 * affects packets *it* sends (uplink). It has no effect on which path the
 * server picks for the downlink packets it sends back. Without this
 * capsule, getting both directions to agree meant configuring both sides
 * separately by hand.
 *
 * Fix: the CLIENT is the only side that actually knows "this new path_id
 * is my wan2, replacing the one that just dropped" (it created the path,
 * and already has a stable local name for it — mqvpn_path_desc_t.iface) —
 * and it already knows its own weight/dscp_mask for that path. It
 * announces {iface, weight, dscp_mask} for path_id N to the server as a
 * PATH_LABEL capsule (RFC 9297 Capsule Protocol) on the existing
 * CONNECT-IP request stream, every time it (re)activates a secondary
 * path. No xquic changes: the Capsule Protocol already carries an
 * app-defined uint64_t type field, and mqvpn's server already tolerates
 * unrecognized capsule types on that stream by skipping them (see
 * mqvpn_server.c's capsule dispatch loop) — an older peer on either end
 * just never sees/sends this and falls back to the existing path_id-only,
 * client-uplink-only, non-persistent behavior.
 *
 * The server keys its per-user persistence table (svr_path_label_t) by
 * the iface string carried here, and re-applies the stored weight/
 * dscp_mask to whatever path_id is currently announced for that iface —
 * solving problem 1. By DEFAULT it also *adopts* the client's own
 * announced weight/dscp_mask for its own downlink scheduling — solving
 * problem 2, so setting a weight/dscp_mask once on the client is enough
 * for both directions to agree, with no separate server-side action
 * required. An operator can still call mqvpn_server_set_path_weight_by_iface()
 * / mqvpn_server_set_path_dscp_mask_by_iface() explicitly to pin the
 * server to a *different* value than the client's own (asymmetric
 * control) — an explicit call always takes precedence over the client's
 * announcement from then on; see mqvpn_server.c's dispatch handler.
 *
 * This header is deliberately xquic-free (mirrors flow_sched.h) so it can
 * be unit tested without linking xquic; mqvpn_client.c/mqvpn_server.c wrap
 * it with the actual xqc_h3_ext_capsule_encode/decode +
 * xqc_h3_request_send_body calls.
 */
#ifndef MQVPN_PATH_LABEL_H
#define MQVPN_PATH_LABEL_H

#include <stddef.h>
#include <stdint.h>

/* Capsule type (RFC 9297 §3.1's type field is an app-defined uint64_t —
 * no xquic/IETF registration needed). Chosen well outside the single-byte
 * range IETF capsule types (DATAGRAM/ADDRESS_ASSIGN/ADDRESS_REQUEST/
 * ROUTE_ADVERTISEMENT = 0x00-0x03) currently occupy, to make a future
 * collision vanishingly unlikely: ASCII "MQPL" read as a big-endian u32. */
#define MQVPN_CAPSULE_PATH_LABEL 0x4d51504cULL

/* Matches path_entry_t.name / mqvpn_path_desc_t.iface (both char[16]). */
#define MQVPN_PATH_LABEL_IFACE_MAX 15

/* Max encoded payload size: 8 (path_id) + 1 (iface_len) + 15 (iface)
 * + 4 (weight) + 8 (dscp_mask). */
#define MQVPN_PATH_LABEL_PAYLOAD_MAX 36

/* Encode {path_id, iface, weight, dscp_mask} into buf (wire format:
 * 8-byte big-endian path_id, 1-byte iface length, iface bytes (not
 * NUL-terminated on the wire), 4-byte big-endian weight, 8-byte
 * big-endian dscp_mask). weight=0 / dscp_mask=0 both mean "the client has
 * no dedicated value for this path" — the same sentinel these fields
 * already use everywhere else (mqvpn_path_desc_t, mqvpn_client_set_path_weight()
 * etc.) — so the server has nothing to adopt for that field, not "adopt
 * zero". `iface` must be a NUL-terminated string of at most
 * MQVPN_PATH_LABEL_IFACE_MAX bytes. Returns the number of bytes written
 * (always <= MQVPN_PATH_LABEL_PAYLOAD_MAX), or -1 if iface is NULL/empty/
 * too long or buf is too small. */
int mqvpn_path_label_encode(uint8_t *buf, size_t buf_len, uint64_t path_id,
                            const char *iface, uint32_t weight, uint64_t dscp_mask);

/* Decode a PATH_LABEL capsule payload. iface_out must be at least
 * MQVPN_PATH_LABEL_IFACE_MAX+1 bytes; always NUL-terminated on success.
 * weight_out/dscp_mask_out receive 0 when the client has no dedicated
 * value for that field (see mqvpn_path_label_encode()'s doc comment).
 * Returns 0 on success, -1 on a malformed/truncated payload (caller should
 * treat this the same as an unrecognized capsule: skip and continue —
 * never tear down the connection over it). */
int mqvpn_path_label_decode(const uint8_t *payload, size_t payload_len,
                            uint64_t *path_id_out, char *iface_out,
                            size_t iface_out_cap, uint32_t *weight_out,
                            uint64_t *dscp_mask_out);

#endif /* MQVPN_PATH_LABEL_H */
