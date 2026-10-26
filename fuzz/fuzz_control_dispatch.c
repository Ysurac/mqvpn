// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/* libFuzzer target: control-API request bytes -> dispatch() -> JSON command
 * parsing (yyjson) + per-command argument extraction + error-response
 * building.
 *
 * Scope: the control socket accepts TCP connections (127.0.0.1 by default,
 * but --control-addr can bind any address and the API is unauthenticated),
 * so the request parser must be crash-safe on ARBITRARY bytes. This TU
 * #include's control_socket.c — the test_reorder_rx idiom for reaching
 * file-local statics — to fuzz the real dispatch() and command table, with
 * a zeroed ctrl_socket_t: every mqvpn_server_* entry point the handlers
 * call NULL-guards (verified per function when this target was added), so
 * server == NULL exercises the dispatch + argument-extraction surface (cmd
 * lookup, add_user/remove_user/get_fec_stats argument extraction, error
 * envelopes) without a live server. Deliberately NOT covered: the library
 * side of those calls — with a real server, request-derived strings also
 * reach mqvpn_server_add_user's own validation/copying etc.; here they
 * short-circuit at the NULL guard (and mqvpn_lib is uninstrumented
 * anyway). Session-loop response bodies are likewise out of scope: they
 * are built from server state, not from request bytes.
 *
 * Also covered: the request framing (ctrl_frame_request: '{'-only rule and
 * brace-counting completeness scan), which is pure over the connection's
 * buffer. Each input is dispatched twice: raw (dispatch() must survive any
 * bytes) and as the framed object ctrl_on_read would hand over. The fd /
 * event-loop side of ctrl_on_read is not covered.
 *
 * Input contract: dispatch() takes a NUL-terminated request; ctrl_on_read
 * caps a request at CTRL_MAX_REQ (4096) bytes, so longer fuzz inputs are
 * rejected up front to keep the corpus in-contract.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "platform/linux/control_socket.c"

int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > CTRL_MAX_REQ) return 0;

    /* Exact-size heap copies (size + NUL): a parser read past the
     * terminator lands outside the allocation, where ASan reports it; a
     * fixed 4097-byte buffer would hide such over-reads. */
    char *req = malloc(size + 1);
    if (!req) return 0;
    memcpy(req, data, size);
    req[size] = '\0';

    /* server=NULL / gro pointers NULL: both are handled (NULL guards in every
     * mqvpn_server_* call; get_stats reports 0 for NULL counter pointers). */
    ctrl_socket_t cs;
    memset(&cs, 0, sizeof(cs));

    /* static: CTRL_MAX_RESP_BYTES is 256 KiB — keep it off the per-call
     * stack. libFuzzer runs inputs sequentially, so no aliasing. */
    static char resp[CTRL_MAX_RESP_BYTES];
    (void)dispatch(req, resp, sizeof(resp), &cs);

    /* Framed path, as ctrl_on_read runs it on a complete object; the cut
     * object is again dispatched from an exact-size copy. */
    static ctrl_conn_t conn;
    memcpy(conn.req, data, size);
    conn.req[size] = '\0';
    conn.req_len = size;
    if (ctrl_frame_request(&conn) == 1) {
        size_t n = strlen(conn.req);
        char *framed = malloc(n + 1);
        if (framed) {
            memcpy(framed, conn.req, n + 1);
            (void)dispatch(framed, resp, sizeof(resp), &cs);
            free(framed);
        }
    }
    free(req);
    return 0;
}
