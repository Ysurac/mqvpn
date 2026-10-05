// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * cert_pin_ssl.c — PinnedPubkey: the BoringSSL side (see cert_pin.h)
 */
#include "cert_pin.h"
#include "auth.h" /* mqvpn_auth_b64_encode */

#include <openssl/mem.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

int
mqvpn_cert_pin_spki_sha256(const uint8_t *der, size_t der_len, uint8_t out[MQVPN_PIN_LEN])
{
    if (!der || der_len == 0 || !out) return -1;

    const uint8_t *p = der;
    X509 *x = d2i_X509(NULL, &p, (long)der_len);
    if (!x) return -1;

    int rc = -1;
    uint8_t *spki = NULL;
    int spki_len = i2d_X509_PUBKEY(X509_get_X509_PUBKEY(x), &spki);
    if (spki_len > 0) {
        SHA256(spki, (size_t)spki_len, out);
        rc = 0;
    }
    OPENSSL_free(spki);
    X509_free(x);
    return rc;
}

int
mqvpn_cert_pin_check_ssl(void *ssl, const uint8_t pins[][MQVPN_PIN_LEN], int n,
                         char *got_b64, size_t got_b64_len)
{
    if (got_b64 && got_b64_len > 0) got_b64[0] = '\0';
    if (!ssl) return 0;

    /* The leaf is first; it is the certificate whose key signed the TLS 1.3
     * CertificateVerify, so matching it proves the peer holds a pinned key. */
    const STACK_OF(CRYPTO_BUFFER) *chain = SSL_get0_peer_certificates((SSL *)ssl);
    if (!chain || sk_CRYPTO_BUFFER_num(chain) == 0) return 0;
    const CRYPTO_BUFFER *leaf = sk_CRYPTO_BUFFER_value(chain, 0);

    uint8_t digest[MQVPN_PIN_LEN];
    if (mqvpn_cert_pin_spki_sha256(CRYPTO_BUFFER_data(leaf), CRYPTO_BUFFER_len(leaf),
                                   digest) < 0)
        return 0;

    if (got_b64 && got_b64_len > MQVPN_PIN_B64_LEN)
        mqvpn_auth_b64_encode(got_b64, got_b64_len, digest, sizeof(digest));
    return mqvpn_cert_pin_match(digest, pins, n);
}
