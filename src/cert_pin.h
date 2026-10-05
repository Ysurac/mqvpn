// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * cert_pin.h — server public-key pinning for the client (PinnedPubkey)
 *
 * A pin is the SHA-256 of the server certificate's DER SubjectPublicKeyInfo,
 * the same value as curl --pinnedpubkey and HPKP. It is written in base64,
 * optionally prefixed "sha256//"; several pins are separated by ';' so a key
 * rotation can list the old and the new key. Generate it from a certificate:
 *
 *   openssl x509 -in server.crt -pubkey -noout | openssl pkey -pubin -outform der \
 *     | openssl dgst -sha256 -binary | openssl enc -base64
 *
 * When a pin is set it replaces CA, hostname and expiry validation: the
 * client accepts exactly the servers that prove possession of a pinned key
 * (TLS 1.3 CertificateVerify, always checked by BoringSSL), whatever their
 * certificate says, so a self-signed server needs no Insecure.
 *
 * This header has no BoringSSL dependency. Parsing and matching
 * (cert_pin.c) do not use it either, so mqvpn_config.c stays linkable
 * without it; only cert_pin_ssl.c talks to BoringSSL.
 */
#ifndef MQVPN_CERT_PIN_H
#define MQVPN_CERT_PIN_H

#include <stddef.h>
#include <stdint.h>

#define MQVPN_PIN_LEN            32 /* SHA-256 */
#define MQVPN_MAX_PINNED_PUBKEYS 4
#define MQVPN_PIN_B64_LEN        44 /* base64 of 32 bytes, no NUL */

/* Parse a pin list ("sha256//<b64>;<b64>"). Whitespace around each pin is
 * ignored; empty entries are not. Writes up to MQVPN_MAX_PINNED_PUBKEYS
 * digests to out and the count to *n_out. Returns 0, or -1 when spec is
 * empty, malformed, a pin does not decode to 32 bytes, or there are too
 * many pins (out and *n_out are then unspecified). */
int mqvpn_cert_pin_parse(const char *spec, uint8_t out[][MQVPN_PIN_LEN], int *n_out);

/* SHA-256 of the DER SubjectPublicKeyInfo of a DER certificate. Returns 0,
 * or -1 when the certificate does not parse. */
int mqvpn_cert_pin_spki_sha256(const uint8_t *der, size_t der_len,
                               uint8_t out[MQVPN_PIN_LEN]);

/* Returns 1 when digest equals one of the n pins, 0 otherwise. */
int mqvpn_cert_pin_match(const uint8_t digest[MQVPN_PIN_LEN],
                         const uint8_t pins[][MQVPN_PIN_LEN], int n);

/* Check the leaf certificate the peer of ssl (an SSL *, after the handshake)
 * presented against the pins. Returns 1 on a match, 0 on a mismatch or when
 * the peer sent no parseable certificate. When got_b64 is non-NULL (at least
 * MQVPN_PIN_B64_LEN + 1 bytes) it receives the peer's pin in base64, or ""
 * when there is none, for the log. */
int mqvpn_cert_pin_check_ssl(void *ssl, const uint8_t pins[][MQVPN_PIN_LEN], int n,
                             char *got_b64, size_t got_b64_len);

#endif /* MQVPN_CERT_PIN_H */
