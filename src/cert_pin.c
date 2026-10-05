// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * cert_pin.c — PinnedPubkey parsing and matching (see cert_pin.h)
 *
 * No BoringSSL here: mqvpn_config.c calls the parser and is linked into unit
 * tests that do not link it. The certificate side is in cert_pin_ssl.c.
 */
#include "cert_pin.h"

#include <string.h>

#define PIN_PREFIX     "sha256//"
#define PIN_PREFIX_LEN (sizeof(PIN_PREFIX) - 1)

static int
is_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

static int
b64_val(char ch)
{
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
    if (ch >= '0' && ch <= '9') return ch - '0' + 52;
    if (ch == '+') return 62;
    if (ch == '/') return 63;
    return -1;
}

/* Decode one pin [p, e), whitespace already trimmed. A 32-byte digest is
 * exactly 43 base64 characters and one '='; anything else is rejected. */
static int
parse_one(const char *p, const char *e, uint8_t out[MQVPN_PIN_LEN])
{
    if ((size_t)(e - p) >= PIN_PREFIX_LEN && memcmp(p, PIN_PREFIX, PIN_PREFIX_LEN) == 0)
        p += PIN_PREFIX_LEN;
    if (e - p != MQVPN_PIN_B64_LEN || e[-1] != '=') return -1;

    uint32_t acc = 0;
    int bits = 0;
    size_t n = 0;
    for (const char *q = p; q < e - 1; q++) {
        int v = b64_val(*q);
        if (v < 0) return -1;
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    /* 43 * 6 = 258 bits: 32 bytes and 2 padding bits, which must be zero. */
    if (n != MQVPN_PIN_LEN || (acc & ((1u << bits) - 1)) != 0) return -1;
    return 0;
}

int
mqvpn_cert_pin_parse(const char *spec, uint8_t out[][MQVPN_PIN_LEN], int *n_out)
{
    if (!spec || !out || !n_out) return -1;

    int n = 0;
    const char *p = spec;
    for (;;) {
        const char *e = strchr(p, ';');
        if (!e) e = p + strlen(p);

        const char *s = p, *t = e;
        while (s < t && is_space(*s))
            s++;
        while (t > s && is_space(t[-1]))
            t--;
        if (n >= MQVPN_MAX_PINNED_PUBKEYS) return -1;
        if (parse_one(s, t, out[n]) < 0) return -1;
        n++;

        if (*e == '\0') break;
        p = e + 1;
    }
    *n_out = n;
    return 0;
}

int
mqvpn_cert_pin_match(const uint8_t digest[MQVPN_PIN_LEN],
                     const uint8_t pins[][MQVPN_PIN_LEN], int n)
{
    /* Pins are public; the full scan just keeps the loop branch-free. */
    int match = 0;
    for (int i = 0; i < n; i++) {
        uint8_t diff = 0;
        for (int j = 0; j < MQVPN_PIN_LEN; j++)
            diff |= digest[j] ^ pins[i][j];
        match |= diff == 0;
    }
    return match;
}
