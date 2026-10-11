// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * json_mini.h — Minimal JSON parsing helpers (static inline)
 *
 * Shared across config.c, mqvpn_config.c, control_socket.c, status.c.
 * No dependencies beyond libc.
 */
#ifndef MQVPN_JSON_MINI_H
#define MQVPN_JSON_MINI_H

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static inline const char *
json_skip_ws(const char *p)
{
    while (*p && isspace((unsigned char)*p))
        p++;
    return p;
}

/* Find a key of the JSON object that starts at `json` (leading whitespace is
 * skipped; text that does not start with '{' has no keys). Only the object's
 * own members match: a key of the same name inside a nested object or array
 * value is skipped, so {"a":{"cmd":"x"}} has no top-level "cmd". The scan
 * stops at the object's closing brace; a truncated object (no closing brace)
 * is searched to its end. Returns a pointer to the value (whitespace
 * skipped), or NULL. */
static inline const char *
json_find_key(const char *json, const char *key)
{
    if (!json || !key) return NULL;
    size_t key_len = strlen(key);
    const char *p = json_skip_ws(json);
    if (*p != '{') return NULL;

    int depth = 0;
    for (; *p; p++) {
        if (*p == '"') {
            const char *k = p + 1;
            const char *e = k;
            while (*e && *e != '"') {
                if (*e == '\\' && e[1]) e++;
                e++;
            }
            if (*e != '"') return NULL;
            if (depth == 1) {
                const char *c = json_skip_ws(e + 1);
                if (*c == ':' && (size_t)(e - k) == key_len &&
                    strncmp(k, key, key_len) == 0)
                    return json_skip_ws(c + 1);
            }
            p = e;
        } else if (*p == '{' || *p == '[') {
            depth++;
        } else if (*p == '}' || *p == ']') {
            if (--depth <= 0) return NULL;
        }
    }
    return NULL;
}

/* Pointer to the '}' that closes the object beginning at `obj` (must point at
 * '{'), accounting for nesting and braces inside string literals. NULL if
 * unterminated. */
static inline const char *
json_object_end(const char *obj)
{
    if (!obj || *obj != '{') return NULL;
    int depth = 0, in_str = 0;
    for (const char *p = obj; *p; p++) {
        if (in_str) {
            if (*p == '\\' && p[1]) {
                p++;
                continue;
            }
            if (*p == '"') in_str = 0;
            continue;
        }
        if (*p == '"')
            in_str = 1;
        else if (*p == '{')
            depth++;
        else if (*p == '}' && --depth == 0)
            return p;
    }
    return NULL;
}

/* json_find_key bounded to [obj, obj_end): returns the value pointer only if it
 * lies before obj_end (i.e. belongs to this object, not a sibling). */
static inline const char *
json_find_key_bounded(const char *obj, const char *obj_end, const char *key)
{
    const char *v = json_find_key(obj, key);
    return (v && obj_end && v < obj_end) ? v : NULL;
}

/* True when a parsed JSON scalar stops before a legal delimiter. */
static inline int
json_value_has_valid_end(const char *p)
{
    p = json_skip_ws(p);
    return *p == '\0' || *p == ',' || *p == '}' || *p == ']';
}

static inline int
json_hex4(const char *p, unsigned *out)
{
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        v <<= 4;
        if (c >= '0' && c <= '9')
            v |= (unsigned)(c - '0');
        else if (c >= 'a' && c <= 'f')
            v |= (unsigned)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F')
            v |= (unsigned)(c - 'A' + 10);
        else
            return -1;
    }
    *out = v;
    return 0;
}

#define JSON_STR_MALFORMED (-1) /* not a string, bad escape, unterminated */
#define JSON_STR_TRUNCATED (-2) /* well-formed, but longer than out_len - 1 */

/* Read a JSON string value, decoding the standard escapes (\" \\ \/ \b \f
 * \n \r \t and \uXXXX, written out as UTF-8; surrogate pairs combine, a lone
 * surrogate or \u0000 is malformed).
 *
 * Returns 0 on success, JSON_STR_MALFORMED, or JSON_STR_TRUNCATED when the
 * decoded value does not fit: `out` then holds the NUL-terminated prefix, so
 * display-only callers may ignore the code, while callers storing names, keys
 * or paths must treat any non-zero return as an error rather than keep a
 * silently shortened value. */
static inline int
json_read_string(const char *p, char *out, size_t out_len)
{
    if (!p || !out || out_len == 0 || *p != '"') return JSON_STR_MALFORMED;
    p++;
    size_t j = 0;
    int truncated = 0;
    while (*p && *p != '"') {
        unsigned char tmp[4];
        size_t n = 1;
        tmp[0] = (unsigned char)*p;
        if (*p == '\\') {
            p++;
            switch (*p) {
            case '"': tmp[0] = '"'; break;
            case '\\': tmp[0] = '\\'; break;
            case '/': tmp[0] = '/'; break;
            case 'b': tmp[0] = '\b'; break;
            case 'f': tmp[0] = '\f'; break;
            case 'n': tmp[0] = '\n'; break;
            case 'r': tmp[0] = '\r'; break;
            case 't': tmp[0] = '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (json_hex4(p + 1, &cp) < 0) return JSON_STR_MALFORMED;
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF) {
                    unsigned lo = 0;
                    if (p[1] != '\\' || p[2] != 'u' || json_hex4(p + 3, &lo) < 0 ||
                        lo < 0xDC00 || lo > 0xDFFF)
                        return JSON_STR_MALFORMED;
                    p += 6;
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                } else if ((cp >= 0xDC00 && cp <= 0xDFFF) || cp == 0) {
                    return JSON_STR_MALFORMED;
                }
                if (cp < 0x80) {
                    tmp[0] = (unsigned char)cp;
                } else if (cp < 0x800) {
                    tmp[0] = (unsigned char)(0xC0 | (cp >> 6));
                    tmp[1] = (unsigned char)(0x80 | (cp & 0x3F));
                    n = 2;
                } else if (cp < 0x10000) {
                    tmp[0] = (unsigned char)(0xE0 | (cp >> 12));
                    tmp[1] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[2] = (unsigned char)(0x80 | (cp & 0x3F));
                    n = 3;
                } else {
                    tmp[0] = (unsigned char)(0xF0 | (cp >> 18));
                    tmp[1] = (unsigned char)(0x80 | ((cp >> 12) & 0x3F));
                    tmp[2] = (unsigned char)(0x80 | ((cp >> 6) & 0x3F));
                    tmp[3] = (unsigned char)(0x80 | (cp & 0x3F));
                    n = 4;
                }
                break;
            }
            default: return JSON_STR_MALFORMED;
            }
        }
        if (!truncated && j + n < out_len) {
            memcpy(out + j, tmp, n);
            j += n;
        } else {
            truncated = 1;
        }
        p++;
    }
    out[j] = '\0';
    if (*p != '"') return JSON_STR_MALFORMED;
    return truncated ? JSON_STR_TRUNCATED : 0;
}

/* Pointer just past the closing quote of the JSON string starting at `p`
 * (which must point at '"'), or NULL if unterminated. */
static inline const char *
json_string_end(const char *p)
{
    if (!p || *p != '"') return NULL;
    p++;
    while (*p && *p != '"') {
        if (*p == '\\' && p[1]) p++;
        p++;
    }
    return *p == '"' ? p + 1 : NULL;
}

/* Read a JSON boolean value. Returns 0 on success, -1 on error (including a
 * literal followed by junk, e.g. `truex`). */
static inline int
json_read_bool(const char *p, int *out)
{
    if (!p || !out) return -1;
    if (strncmp(p, "true", 4) == 0 && json_value_has_valid_end(p + 4)) {
        *out = 1;
        return 0;
    }
    if (strncmp(p, "false", 5) == 0 && json_value_has_valid_end(p + 5)) {
        *out = 0;
        return 0;
    }
    return -1;
}

/* Strict JSON integer reader: returns 0 on success, -1 on error. Rejects
 * non-numeric prefixes, overflow, and trailing junk such as `123abc`. */
static inline int
json_read_int_strict(const char *p, int *out)
{
    if (!p || !out) return -1;
    p = json_skip_ws(p);
    if (*p != '-' && (*p < '0' || *p > '9')) return -1;

    errno = 0;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p || errno == ERANGE || v < INT_MIN || v > INT_MAX ||
        !json_value_has_valid_end(end)) {
        return -1;
    }

    *out = (int)v;
    return 0;
}

/* Read a JSON integer value (int). Returns 0 on success, -1 on error. Same
 * strictness as json_read_int_strict: overflow, values outside int, a
 * fraction/exponent and trailing junk are errors, never truncated. */
static inline int
json_read_int(const char *p, int *out)
{
    return json_read_int_strict(p, out);
}

/* Strict int64 reader: returns 0 on success, -1 on a non-number, overflow or
 * trailing junk. */
static inline int
json_read_int64_strict(const char *p, int64_t *out)
{
    if (!p || !out) return -1;
    p = json_skip_ws(p);
    if (*p != '-' && (*p < '0' || *p > '9')) return -1;

    errno = 0;
    char *end = NULL;
    long long v = strtoll(p, &end, 10);
    if (end == p || errno == ERANGE || !json_value_has_valid_end(end)) return -1;

    *out = (int64_t)v;
    return 0;
}

/* Read a JSON integer value (int64_t). Returns the value, or 0 when the text
 * is absent or not a valid integer (callers that need to tell 0 from an
 * error use json_read_int64_strict). */
static inline int64_t
json_read_int64(const char *p)
{
    int64_t v = 0;
    return json_read_int64_strict(p, &v) == 0 ? v : 0;
}

static inline int
json_read_u64_strict(const char *p, uint64_t *out)
{
    if (!p || !out) return -1;
    p = json_skip_ws(p);
    if (*p < '0' || *p > '9') return -1;

    errno = 0;
    char *end = NULL;
    unsigned long long v = strtoull(p, &end, 10);
    if (end == p || errno == ERANGE || !json_value_has_valid_end(end)) {
        return -1;
    }

    *out = (uint64_t)v;
    return 0;
}

/* Safe string copy with NUL termination. */
static inline void
mqvpn_copy_str(char *dst, size_t dst_len, const char *src)
{
    if (!dst || dst_len == 0) return;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    strncpy(dst, src, dst_len - 1);
    dst[dst_len - 1] = '\0';
}

/* The user-name rule shared by every intake path (CLI --user, INI/JSON
 * config, control API, library setters): non-empty, shorter than `cap`
 * bytes (the destination buffer size, NUL included), and free of '"', '\\'
 * and control bytes (< 0x20), because names are echoed into control-API
 * JSON and log lines without escaping. Returns 1 if acceptable. */
static inline int
mqvpn_user_name_ok(const char *s, size_t cap)
{
    if (!s || s[0] == '\0') return 0;
    size_t n = 0;
    for (const char *p = s; *p; p++, n++) {
        if (*p == '"' || *p == '\\' || (unsigned char)*p < 0x20) return 0;
    }
    return n < cap;
}

/* Parse a "users" JSON array shared by config.c (INI/JSON file config) and
 * mqvpn_config.c (control-API JSON config). Accepts two element forms:
 *   - "name:key" string form
 *   - {"name":"..","key":"..","fixed_ip":".."} object form (fixed_ip optional)
 * `arr` must point at the array's '[' character. Each parsed entry is handed
 * to `add_user(ctx, name, key, fixed_ip)` — `fixed_ip` is NULL for the string
 * form or when the object has no usable "fixed_ip"; a non-zero return aborts
 * the parse. Returns 0 on success, -1 on parse or add failure. */
static inline int
mqvpn_json_parse_users(const char *arr, void *ctx,
                       int (*add_user)(void *ctx, const char *name,
                                       const char *key, const char *fixed_ip))
{
    if (!arr || *arr != '[' || !add_user) return -1;
    const char *p = json_skip_ws(arr + 1);

    while (*p && *p != ']') {
        char name[64] = {0};
        char key[256] = {0};
        char fixed_ip[20] = {0};

        if (*p == '"') {
            char pair[360] = {0};
            if (json_read_string(p, pair, sizeof(pair)) != 0) return -1;
            char *sep = strchr(pair, ':');
            if (!sep) return -1;
            *sep = '\0';
            /* Over-long fields are errors, not silently shortened names/keys. */
            if (strlen(pair) >= sizeof(name) || strlen(sep + 1) >= sizeof(key)) return -1;
            mqvpn_copy_str(name, sizeof(name), pair);
            mqvpn_copy_str(key, sizeof(key), sep + 1);

            const char *e = json_string_end(p);
            if (!e) return -1;
            p = json_skip_ws(e);
        } else if (*p == '{') {
            const char *end = json_object_end(p);
            if (!end) return -1;

            char obj[512];
            size_t len = (size_t)(end - p + 1);
            if (len >= sizeof(obj)) return -1;
            memcpy(obj, p, len);
            obj[len] = '\0';

            const char *name_v = json_find_key(obj, "name");
            const char *key_v = json_find_key(obj, "key");
            if (!name_v || !key_v) return -1;
            if (json_read_string(name_v, name, sizeof(name)) != 0) return -1;
            if (json_read_string(key_v, key, sizeof(key)) != 0) return -1;

            /* A non-string fixed_ip (e.g. null) means "dynamic", as before; a
             * string that does not fit is an error, not a shortened address. */
            const char *fip_v = json_find_key(obj, "fixed_ip");
            if (fip_v && *fip_v == '"' &&
                json_read_string(fip_v, fixed_ip, sizeof(fixed_ip)) != 0)
                return -1;

            p = json_skip_ws(end + 1);
        } else {
            return -1;
        }

        if (add_user(ctx, name, key, fixed_ip[0] ? fixed_ip : NULL) != 0)
            return -1;

        if (*p == ',')
            p = json_skip_ws(p + 1);
        else if (*p != ']')
            return -1;
    }

    return (*p == ']') ? 0 : -1;
}

#endif /* MQVPN_JSON_MINI_H */
