// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * mqvpn_xquic_err_names.h — human-readable names for the QUIC/HTTP3/QPACK
 * *wire* error codes xquic prints in its terse `|err:NNN|` report fields
 * (xqc_process_conn_close_frame, xqc_conn_destroy, xqc_h3_request_destroy,
 * ...). These are the codes exchanged on the wire in a CONNECTION_CLOSE
 * frame (RFC 9000 §20.1 transport codes, RFC 9114 §8.1 H3 codes, QPACK draft
 * codes) — as opposed to xquic's *internal* 6xx/7xx/8xx/9xx return codes
 * (xqc_transport_error_t &c in xqc_errno.h), which are local library
 * function-return values, never serialised onto the wire, and never appear
 * in an "err:" report field. Those are out of scope here.
 *
 * Deliberately does NOT modify third_party/xquic: this only mirrors the
 * enum constants xqc_errno.h already defines into a name table, so a future
 * upstream sync that adds/renumbers a code just falls back to unannotated
 * (see mqvpn_xquic_wire_err_name's default case) instead of breaking.
 *
 * Used by cb_xqc_log_write in mqvpn_client.c / mqvpn_server.c to turn
 * "...|err:0x106|..." into "...|err:0x106(H3_FRAME_ERROR)|..." before the
 * line reaches the configured log sink.
 */
#ifndef MQVPN_XQUIC_ERR_NAMES_H
#define MQVPN_XQUIC_ERR_NAMES_H

#include <xquic/xquic.h>

#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* X-macro row: X(code, "SYMBOLIC_NAME"). Values/names mirror xqc_errno.h's
 * xqc_trans_err_code_t — update alongside any change there (see file
 * comment above). */
#define MQVPN_XQUIC_TRANS_ERR_LIST(X)                                     \
    X(TRA_NO_ERROR, "NO_ERROR")                                           \
    X(TRA_INTERNAL_ERROR, "INTERNAL_ERROR")                               \
    X(TRA_CONNECTION_REFUSED_ERROR, "CONNECTION_REFUSED")                 \
    X(TRA_FLOW_CONTROL_ERROR, "FLOW_CONTROL_ERROR")                       \
    X(TRA_STREAM_LIMIT_ERROR, "STREAM_LIMIT_ERROR")                       \
    X(TRA_STREAM_STATE_ERROR, "STREAM_STATE_ERROR")                       \
    X(TRA_FINAL_SIZE_ERROR, "FINAL_SIZE_ERROR")                           \
    X(TRA_FRAME_ENCODING_ERROR, "FRAME_ENCODING_ERROR")                   \
    X(TRA_TRANSPORT_PARAMETER_ERROR, "TRANSPORT_PARAMETER_ERROR")         \
    X(TRA_CONNECTION_ID_LIMIT_ERROR, "CONNECTION_ID_LIMIT_ERROR")         \
    X(TRA_PROTOCOL_VIOLATION, "PROTOCOL_VIOLATION")                       \
    X(TRA_INVALID_TOKEN, "INVALID_TOKEN")                                 \
    X(TRA_APPLICATION_ERROR, "APPLICATION_ERROR")                        \
    X(TRA_CRYPTO_BUFFER_EXCEEDED, "CRYPTO_BUFFER_EXCEEDED")               \
    X(TRA_0RTT_TRANS_PARAMS_ERROR, "0RTT_TRANSPORT_PARAMS_ERROR")         \
    X(TRA_AEAD_LIMIT_REACHED, "AEAD_LIMIT_REACHED")                       \
    X(TRA_VERSION_NEGOTIATION_ERROR, "VERSION_NEGOTIATION_ERROR")         \
    X(TRA_NO_APPLICATION_PROTOCOL, "NO_APPLICATION_PROTOCOL")

/* draft-ietf-quic-multipath-21 PATH_ABANDON error codes. These are #define'd
 * in xqc_errno.h, not xqc_trans_err_code_t enumerators, so they sit outside
 * MQVPN_XQUIC_TRANS_ERR_LIST's -Wswitch coverage net below — upstream pins
 * the wire codepoint itself (see the _Static_assert next to
 * TRA_PATH_UNSTABLE_OR_POOR in xqc_errno.h). */
#define MQVPN_XQUIC_MP_ABANDON_ERR_LIST(X)                                \
    X(TRA_APPLICATION_ABANDON_PATH, "MP_ABANDON_PATH")                    \
    X(TRA_PATH_RESOURCE_LIMIT_REACHED, "MP_PATH_RESOURCE_LIMIT_REACHED")  \
    X(TRA_PATH_UNSTABLE_OR_POOR, "MP_PATH_UNSTABLE_OR_POOR")              \
    X(TRA_NO_CID_AVAILABLE_FOR_PATH, "MP_NO_CID_AVAILABLE_FOR_PATH")

/* Mirrors xqc_errno.h's xqc_h3_err_code_t. */
#define MQVPN_XQUIC_H3_ERR_LIST(X)                                        \
    X(H3_NO_ERROR, "H3_NO_ERROR")                                         \
    X(H3_GENERAL_PROTOCOL_ERROR, "H3_GENERAL_PROTOCOL_ERROR")             \
    X(H3_INTERNAL_ERROR, "H3_INTERNAL_ERROR")                             \
    X(H3_STREAM_CREATION_ERROR, "H3_STREAM_CREATION_ERROR")               \
    X(H3_CLOSED_CRITICAL_STREAM, "H3_CLOSED_CRITICAL_STREAM")             \
    X(H3_FRAME_UNEXPECTED, "H3_FRAME_UNEXPECTED")                         \
    X(H3_FRAME_ERROR, "H3_FRAME_ERROR")                                   \
    X(H3_EXCESSIVE_LOAD, "H3_EXCESSIVE_LOAD")                             \
    X(H3_ID_ERROR, "H3_ID_ERROR")                                         \
    X(H3_SETTINGS_ERROR, "H3_SETTINGS_ERROR")                             \
    X(H3_MISSING_SETTINGS, "H3_MISSING_SETTINGS")                        \
    X(H3_REQUEST_REJECTED, "H3_REQUEST_REJECTED")                        \
    X(H3_REQUEST_CANCELLED, "H3_REQUEST_CANCELLED")                      \
    X(H3_REQUEST_INCOMPLETE, "H3_REQUEST_INCOMPLETE")                    \
    X(H3_MESSAGE_ERROR, "H3_MESSAGE_ERROR")                              \
    X(H3_CONNECT_ERROR, "H3_CONNECT_ERROR")                              \
    X(H3_VERSION_FALLBACK, "H3_VERSION_FALLBACK")                        \
    X(H3_DATAGRAM_ERROR, "H3_DATAGRAM_ERROR")

/* Mirrors xqc_errno.h's xqc_qpack_err_code_t. */
#define MQVPN_XQUIC_QPACK_ERR_LIST(X)                                     \
    X(QPACK_DECOMPRESSION_FAILED, "QPACK_DECOMPRESSION_FAILED")           \
    X(QPACK_ENCODER_STREAM_ERROR, "QPACK_ENCODER_STREAM_ERROR")           \
    X(QPACK_DECODER_STREAM_ERROR, "QPACK_DECODER_STREAM_ERROR")

#define MQVPN_XQUIC_WIRE_ERR_LIST(X)      \
    MQVPN_XQUIC_TRANS_ERR_LIST(X)         \
    MQVPN_XQUIC_MP_ABANDON_ERR_LIST(X)    \
    MQVPN_XQUIC_H3_ERR_LIST(X)            \
    MQVPN_XQUIC_QPACK_ERR_LIST(X)

/* mqvpn_xquic_wire_err_name: symbolic name for a QUIC/H3/QPACK wire error
 * code as it appears in xquic's own |err:NNN| report fields. Returns NULL
 * for any value outside this table — callers must treat NULL as "leave
 * unannotated", not "impossible": not every field literally named "err:" in
 * a log line is necessarily one of these namespaces. */
static inline const char *
mqvpn_xquic_wire_err_name(unsigned long code)
{
    switch (code) {
#define MQVPN_XQUIC_WIRE_ERR_CASE(val, str) case (unsigned long)(val): return str;
        MQVPN_XQUIC_WIRE_ERR_LIST(MQVPN_XQUIC_WIRE_ERR_CASE)
#undef MQVPN_XQUIC_WIRE_ERR_CASE
    default: return NULL;
    }
}

/* mqvpn_xquic_annotate_err_codes: copy `in` (an xquic log line of `in_len`
 * bytes, NOT necessarily NUL-terminated) into `out`, and wherever a
 * standalone "err:" field carries a value with a known wire-error name (see
 * mqvpn_xquic_wire_err_name above), append "(NAME)" right after the number —
 * turning "...|err:0x106|..." into "...|err:0x106(H3_FRAME_ERROR)|...".
 * Handles both hex ("0x106") and decimal ("262") spellings, since xquic logs
 * the same code either way depending on the call site.
 *
 * "Standalone" means "err:" is not the tail of a longer identifier: the
 * byte immediately before it (if any) must not be alphanumeric or '_'. This
 * deliberately excludes compound field names like "path_err:" or
 * "sched_err:" — those are unrelated xquic-internal counters, and
 * annotating them off a coincidental numeric overlap (a counter that
 * happens to equal 2, say) would be actively misleading rather than merely
 * unhelpful.
 *
 * Always NUL-terminates `out` within `out_sz` and never overflows it; falls
 * back to a straight (possibly truncated) copy once `out` fills up, so a
 * malformed/oversized line never loses log content — it just stops growing.
 * Returns the number of bytes written to `out`, excluding the terminator. */
static inline size_t
mqvpn_xquic_annotate_err_codes(const char *in, size_t in_len, char *out, size_t out_sz)
{
    size_t oi = 0, i = 0;

    if (out_sz == 0) return 0;

    while (i < in_len && oi < out_sz - 1) {
        int boundary_ok = (i == 0) ||
                           !(isalnum((unsigned char)in[i - 1]) || in[i - 1] == '_');

        if (boundary_ok && i + 4 <= in_len && memcmp(in + i, "err:", 4) == 0) {
            size_t k, num_start, digits = 0;
            int is_hex = 0;
            unsigned long val = 0;

            for (k = 0; k < 4 && oi < out_sz - 1; k++) out[oi++] = in[i + k];
            i += 4;

            num_start = i;
            if (i + 1 < in_len && in[i] == '0' && (in[i + 1] == 'x' || in[i + 1] == 'X')) {
                is_hex = 1;
                i += 2;
            }
            while (i < in_len) {
                char ch = in[i];
                int d = -1;
                if (ch >= '0' && ch <= '9') d = ch - '0';
                else if (is_hex && ch >= 'a' && ch <= 'f') d = 10 + (ch - 'a');
                else if (is_hex && ch >= 'A' && ch <= 'F') d = 10 + (ch - 'A');
                else break;
                val = val * (unsigned long)(is_hex ? 16 : 10) + (unsigned long)d;
                i++;
                digits++;
            }
            for (k = num_start; k < i && oi < out_sz - 1; k++) out[oi++] = in[k];

            if (digits > 0) {
                const char *name = mqvpn_xquic_wire_err_name(val);
                if (name && oi < out_sz - 1) {
                    int n = snprintf(out + oi, out_sz - oi, "(%s)", name);
                    if (n > 0 && (size_t)n < out_sz - oi) oi += (size_t)n;
                }
            }
            continue;
        }

        out[oi++] = in[i++];
    }

    out[oi] = '\0';
    return oi;
}

/* Compile-time coverage: -Wswitch (built -Werror, see AGENTS.md G11) turns a
 * new xqc_trans_err_code_t / xqc_h3_err_code_t / xqc_qpack_err_code_t
 * enumerator added upstream without a matching table row above into a build
 * failure. Never called; exists purely for the compiler to typecheck it. */
static inline void
mqvpn_xquic_trans_err_list_covers_enum_(xqc_trans_err_code_t v)
{
    switch (v) {
#define MQVPN_XQUIC_TRANS_ERR_COVERAGE_CASE(val, str) case val: break;
        MQVPN_XQUIC_TRANS_ERR_LIST(MQVPN_XQUIC_TRANS_ERR_COVERAGE_CASE)
#undef MQVPN_XQUIC_TRANS_ERR_COVERAGE_CASE
    }
}

static inline void
mqvpn_xquic_h3_err_list_covers_enum_(xqc_h3_err_code_t v)
{
    switch (v) {
#define MQVPN_XQUIC_H3_ERR_COVERAGE_CASE(val, str) case val: break;
        MQVPN_XQUIC_H3_ERR_LIST(MQVPN_XQUIC_H3_ERR_COVERAGE_CASE)
#undef MQVPN_XQUIC_H3_ERR_COVERAGE_CASE
    }
}

static inline void
mqvpn_xquic_qpack_err_list_covers_enum_(xqc_qpack_err_code_t v)
{
    switch (v) {
#define MQVPN_XQUIC_QPACK_ERR_COVERAGE_CASE(val, str) case val: break;
        MQVPN_XQUIC_QPACK_ERR_LIST(MQVPN_XQUIC_QPACK_ERR_COVERAGE_CASE)
#undef MQVPN_XQUIC_QPACK_ERR_COVERAGE_CASE
    }
}

#endif /* MQVPN_XQUIC_ERR_NAMES_H */
