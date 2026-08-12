// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * auth.h — PSK authentication utilities for mqvpn
 *
 * - Key generation: 32 bytes from /dev/urandom → base64
 * - Constant-time comparison for PSK verification
 * - Base64 encoding (RFC 4648)
 */
#ifndef MQVPN_AUTH_H
#define MQVPN_AUTH_H

#include <stddef.h>

/* Generate a random PSK and print to stdout. Returns 0 on success. */
int mqvpn_auth_genkey(void);

/* Constant-time comparison. Returns 0 if equal, nonzero otherwise. */
int mqvpn_auth_ct_compare(const char *a, size_t a_len, const char *b, size_t b_len);

/* Non-cryptographic diagnostic fingerprint of a PSK — FNV-1a over the raw
 * bytes, truncated to 32 bits and hex-encoded. For log lines ONLY: lets an
 * operator confirm "client and server loaded the byte-identical secret" by
 * diffing one short token between two log streams, without ever printing
 * the secret itself and without needing a live simultaneous capture on both
 * ends (log it once at startup on each side). NOT preimage- or
 * collision-hard — never use for an auth decision, and treat a mismatch as
 * informative only (a match is strong evidence of equality; astronomically
 * unlikely but not impossible to also hold for two different keys).
 * Writes exactly 8 hex chars + NUL; dst_len must be >= 9. No-op (dst
 * untouched) if dst_len < 9. */
void mqvpn_auth_debug_fingerprint(const char *key, size_t key_len, char *dst, size_t dst_len);

/* Base64 encode src into dst. Returns 0 on success, -1 if dst_len too small. */
int mqvpn_auth_b64_encode(char *dst, size_t dst_len, const unsigned char *src,
                          size_t src_len);

#endif /* MQVPN_AUTH_H */
