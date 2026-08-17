# mqvpn Control API — Wire Protocol Reference

The mqvpn server exposes a JSON control API over TCP. This document is the
authoritative wire-protocol contract for all clients, including the Go
Prometheus exporter.

---

## 1. Overview

- **Transport:** TCP (plaintext, no TLS)
- **Framing:** Newline-terminated JSON objects. The server also accepts a bare
  JSON object terminated by its closing `}` brace, whichever arrives first.
- **Request:** one JSON object per connection (max 4 KB)
- **Response:** one JSON object, newline-terminated; server closes the
  connection after writing the response.
- **Idle timeout:** 5 seconds — connections that do not deliver a complete
  request within 5 seconds are closed without a response.
- **Concurrency:** maximum 8 simultaneous control connections. A 9th connection
  receives `{"ok":false,"error":"too many connections"}` and is immediately
  closed.

---

## 2. Connection

### Default endpoint

`127.0.0.1:9090`

The bind address defaults to `127.0.0.1`. Both IPv4 and IPv6 loopback
addresses are accepted.

### Configuration

```bash
# Enable the control API on port 9090 (loopback only)
sudo mqvpn --mode server ... --control-port 9090

# Bind to a specific address
sudo mqvpn --mode server ... --control-port 9090 --control-addr 127.0.0.1
```

### Security

The control API has **no authentication and no TLS**. If you bind to a
non-loopback address, the server emits a startup `WARN` log:

```
control API: binding to non-loopback address <addr> — the control API has no authentication
```

Front with nginx or another authenticating proxy if you need to expose the
port beyond localhost.

---

## 3. Request Format

A single UTF-8 JSON object, terminated by its closing `}` brace or a newline
character (`\n`). Maximum size: **4 096 bytes** (`CTRL_MAX_REQ`).

All requests must contain a `"cmd"` string field. Unknown commands return an
error response rather than closing the connection.

```json
{"cmd":"<command>", ...}
```

---

## 4. Response Format

A single JSON object followed by a newline (`\n`). The server closes the TCP
connection immediately after writing the response.

```json
{"ok": true, ...}
{"ok": false, "error": "<reason>"}
```

- `ok` is always present and is a boolean.
- When `ok` is `false`, an `error` string is present describing the failure.
- Additional fields (documented per command) are present only when `ok` is
  `true`, unless noted otherwise.

**Overflow guard:** if an internal response would exceed 128 KB (worst-case
`get_status` with many clients), the server instead returns:

```json
{"ok":false,"error":"response too large"}
```

---

## 5. Commands

### 5.1 `add_user`

Add a user (name + pre-shared key) to the server's active auth table. If the
name already exists, the key is updated in place.

**Request**

```json
{"cmd":"add_user","name":"alice","key":"alice-secret"}
```

| Field  | Type   | Required | Description                          |
|--------|--------|----------|--------------------------------------|
| `name` | string | yes      | Username (max 63 chars)              |
| `key`  | string | yes      | Pre-shared key (max 255 chars)       |

**Response — success**

```json
{"ok":true}
```

**Response — errors**

```json
{"ok":false,"error":"name and key required"}
{"ok":false,"error":"add_user failed (-N)"}
```

- `"name and key required"` — one or both fields are missing or cannot be
  parsed.
- `"add_user failed (-N)"` — the underlying library call returned error code
  `-N`; check server logs for details.

---

### 5.2 `remove_user`

Remove a user from the auth table. Does not forcibly disconnect an active
session; the session expires naturally after authentication fails on the next
reconnect.

**Request**

```json
{"cmd":"remove_user","name":"alice"}
```

| Field  | Type   | Required | Description |
|--------|--------|----------|-------------|
| `name` | string | yes      | Username    |

**Response — success**

```json
{"ok":true}
```

**Response — errors**

```json
{"ok":false,"error":"name required"}
{"ok":false,"error":"user not found"}
```

- `"name required"` — the `name` field is missing or cannot be parsed.
- `"user not found"` — no user with that name is registered.

---

### 5.3 `list_users`

Return the list of usernames currently registered in the auth table.

**Request**

```json
{"cmd":"list_users"}
```

**Response**

```json
{"ok":true,"users":["alice","bob"]}
```

| Field   | Type            | Description                        |
|---------|-----------------|------------------------------------|
| `users` | array of string | Registered usernames; may be empty |

---

### 5.4 `get_stats`

Return aggregate server statistics. Extended in v0.5.0 to include QUIC
datagram counters and uptime.

**Request**

```json
{"cmd":"get_stats"}
```

**Response**

```json
{
  "ok":true,
  "n_clients":2,
  "bytes_tx":12345678,"bytes_rx":9876543,
  "dgram_sent":89012,"dgram_recv":84551,
  "dgram_lost":421,"dgram_acked":88341,
  "pkts_lane_tcp":0,"pkts_lane_dgram":0,
  "pkts_lane_raw":0,"pkts_lane_tcp_dropped":0,
  "tcp_flows_active":1,"tcp_flows_total":7,
  "tcp_flows_rejected":2,"raw_markers_active":0,
  "udp_tx_sends":54120,"udp_tx_datagrams":889304,
  "udp_rx_receives":812004,"udp_rx_datagrams":1031887,
  "uptime_sec":3601
}
```

| Field        | Type    | Description                                                    |
|--------------|---------|----------------------------------------------------------------|
| `n_clients`  | integer | Number of currently connected clients. Client mode: `1` if the tunnel is established, `0` otherwise (this side only ever has one upstream connection). |
| `bytes_tx`   | uint64  | Total bytes written to the TUN interface (server → clients)    |
| `bytes_rx`   | uint64  | Total bytes read from the TUN interface (clients → server)     |
| `dgram_sent` | uint64  | QUIC datagrams the server successfully sent (xret == XQC_OK), all sessions |
| `dgram_recv` | uint64  | QUIC datagrams the server forwarded to the TUN interface, all sessions. Excludes datagrams dropped before forwarding (TTL ≤ 1 → ICMP Time Exceeded, source-IP mismatch, malformed frame). |
| `dgram_lost` | uint64  | Total QUIC datagrams declared lost, all sessions               |
| `dgram_acked`| uint64  | Total QUIC datagrams acknowledged, all sessions                |
| `pkts_lane_tcp`   | uint64 | Hybrid mode: packets actually handed to the TCP lane (lwIP). Server-side: always 0 (the server does not classify TUN-ingress packets). |
| `pkts_lane_dgram` | uint64 | Hybrid mode: packets sent via the datagram lane. Server-side: always 0. |
| `pkts_lane_raw`   | uint64 | Hybrid mode: packets sent via the raw CONNECT-IP lane, including TCP candidates that fell back to RAW (sticky-RAW, cap-rejected, non-SYN on an unknown flow, lane-less build). `pkts_lane_tcp` + `pkts_lane_dgram` + `pkts_lane_raw` partitions every classified packet exactly once. Server-side: always 0. |
| `pkts_lane_tcp_dropped` | uint64 | Hybrid mode: TCP-lane packets lwIP refused (e.g. no matching pcb). Client-only; always 0 server-side. |
| `tcp_flows_active`   | uint64 | Hybrid mode: currently open TCP-lane flows. Client: the TCP-lane flow table's live count. Server: the whole-server count of open egress TCP flows. Both wired from real state. |
| `tcp_flows_total`    | uint64 | Hybrid mode: cumulative TCP-lane flows opened since start (never decrements). Client: SYNs the flow table admitted. Server: egress flows admitted. |
| `tcp_flows_rejected` | uint64 | Hybrid mode: cumulative flows refused by a cap. Client: SYNs rejected pre-lwIP (flow-table cap or alloc failure). Server: cap-503 rejections (whole-server fd-budget cap + per-session `TcpMaxFlows` cap; ACL 403s and 5xx syscall failures are not caps and are not counted). |
| `raw_markers_active` | uint64 | Hybrid mode: sticky-RAW markers currently held in the client's TCP-lane flow table (5-tuples pinned to RAW under `tcp=auto`). Client-only; always 0 server-side. |
| `udp_tx_sends` | uint64 | Outer-UDP send syscalls that placed at least one datagram on the wire. |
| `udp_tx_datagrams` | uint64 | Outer-UDP datagrams the kernel accepted. `udp_tx_datagrams / udp_tx_sends` is the achieved transmit batching factor; 1.0 means every datagram cost its own syscall. Exactly 1.0 with `UdpGso = false` and on platforms without the batched send path. |
| `udp_rx_receives` | uint64 | Outer-UDP receive syscalls that returned data. |
| `udp_rx_datagrams` | uint64 | Outer-UDP datagrams delivered to the library. `udp_rx_datagrams / udp_rx_receives` is the achieved receive coalescing factor. Exactly 1.0 with `UdpGro = false`; it also stays near 1.0 when the peer is not batching its sends, since the kernel then has nothing to coalesce, and over a loopback/veth path regardless of the peer. |
| `uptime_sec` | uint64  | Seconds since `mqvpn_server_create` was called (server) / `mqvpn_client_new` was called (client) — process uptime, not "seconds connected". |

Notes:
- **All fields in this command are now wired for both modes** (fixed after
  being silently server-only for several releases — see §7). Previously,
  every field except `udp_rx_receives`/`udp_rx_datagrams` (sourced from the
  platform layer, not `mqvpn_stats_t`) read `0` in client mode regardless of
  real traffic, because the handler unconditionally read the server handle,
  which is `NULL` by design in client mode. Confirmed live on an
  OpenMPTCProuter client-mode router with a traffic-carrying tunnel.
- `bytes_tx`/`bytes_rx` are post-tunnel (TUN-layer) byte counts, not raw UDP
  wire bytes.
- `dgram_*` counters are server-wide aggregates across all active sessions
  (server mode); in client mode they are this connection's own counters.
- The eight hybrid-mode fields (`pkts_lane_*`, `tcp_flows_*`,
  `raw_markers_active`) are additive and stay 0 whenever the hybrid
  classifier is disabled. `tcp_flows_active`, `tcp_flows_total`, and
  `tcp_flows_rejected` are wired on both client and server (with the
  per-side semantics in the table above); `pkts_lane_*`,
  `pkts_lane_tcp_dropped`, and `raw_markers_active` are client-only
  concepts and always report 0 on the server. See §9 for the `[Hybrid]`
  config keys that control this behavior.

---

### 5.5 `get_status`

Return per-client and per-path detail for all currently connected clients.

> **Monitoring caveat — per-user keys required for multi-client setups.**
> The `user` field reports the auth identity that matched. Clients
> authenticating with the server's global `auth_key` are reported as
> `"(global)"`. If multiple clients share a single key, every session is
> emitted with `user="(global)"`, so any consumer that uses `user` as a
> series label (Prometheus exporter, Grafana JSON datasource) cannot tell
> them apart and the duplicate label set typically drops the entire scrape.
>
> For multi-client monitoring give each client its own entry under
> `users` in `server.conf`, or register them at runtime via `add_user`
> (§5.1). Sharing one `auth_key` across multiple clients still works for
> the VPN data plane but is not supported by the monitoring path.

> **Client mode.** `clients` describes this client's own (single) upstream
> connection to the server, using the exact same shape the server uses for
> each of its connected clients: `user` is this client's own configured
> `auth_username` (or `"(self)"` if none is set), `endpoint` the server's
> resolved address, `connected_sec` seconds since *this* connection reached
> `MQVPN_STATE_ESTABLISHED` (resets on reconnect). `n_clients` is `0`
> (empty `clients` array — not an error, same as a server nobody has
> connected to) while not yet connected. This was fixed after being
> silently server-only for several releases — see §7.

**Request**

```json
{"cmd":"get_status"}
```

**Response**

```json
{
  "ok":true,
  "n_clients":1,
  "clients":[
    {
      "user":"alice","endpoint":"1.2.3.4:443",
      "connected_sec":42,
      "bytes_tx":1000,"bytes_rx":2000,
      "paths":[
        {
          "path_id":0,"srtt_ms":31,"min_rtt_ms":18,
          "cwnd":196608,"in_flight":1024,
          "bytes_tx":900,"bytes_rx":1900,
          "pkt_sent":50,"pkt_recv":49,"pkt_lost":1,
          "state":2,"state_label":"active"
        }
      ]
    }
  ]
}
```

**Top-level fields**

| Field      | Type    | Description                         |
|------------|---------|-------------------------------------|
| `n_clients`| integer | Number of active client entries     |
| `clients`  | array   | Per-client objects (see below)      |

**Client object**

| Field           | Type   | Description                                        |
|-----------------|--------|----------------------------------------------------|
| `user`          | string | Username                                           |
| `endpoint`      | string | Client's remote address and port (`ip:port`)       |
| `connected_sec` | uint64 | Seconds since the session was established          |
| `bytes_tx`      | uint64 | TUN bytes sent to this client                      |
| `bytes_rx`      | uint64 | TUN bytes received from this client                |
| `paths`         | array  | Per-path objects (see below)                       |

**Path object**

| Field        | Type   | Description                                              |
|--------------|--------|----------------------------------------------------------|
| `path_id`    | uint64 | xquic path identifier                                    |
| `srtt_ms`    | uint64 | Smoothed RTT in milliseconds (derived from µs)           |
| `min_rtt_ms` | uint64 | Minimum observed RTT in milliseconds (derived from µs)   |
| `cwnd`       | uint64 | Congestion window in bytes                               |
| `in_flight`  | uint64 | Bytes currently in flight                                |
| `bytes_tx`   | uint64 | Bytes sent on this path                                  |
| `bytes_rx`   | uint64 | Bytes received on this path                              |
| `pkt_sent`   | uint64 | QUIC packets sent on this path                           |
| `pkt_recv`   | uint64 | QUIC packets received on this path                       |
| `pkt_lost`     | uint64 | QUIC packets declared lost on this path                                                                            |
| `state`        | uint   | xquic transport path state (numeric). **Legacy** — prefer `state_label` for new code; the numeric value is the raw `xqc_path_state_t` and may shift if xquic re-orders the enum. |
| `state_label`  | string | Stable label for the state — one of `init`, `validating`, `active`, `closing`, `closed`, `unknown`. Added in v0.5.0. |

> **Note on `state`:** the numeric value is the raw xquic transport-layer path
> state, **not** the mqvpn scheduler's logical role (primary / standby / etc.).
> Treat it as an opaque integer for diagnostic purposes; do not map it to
> scheduler roles without consulting the xquic source.

---

### 5.6 `get_build_info` *(new in v0.5.0)*

Return build-time metadata: version string, active scheduler, and whether FEC
support was compiled in.

**Request**

```json
{"cmd":"get_build_info"}
```

**Response**

```json
{"ok":true,"version":"0.6.0","scheduler":"backup_fec","fec_enabled":1}
```

| Field        | Type    | Description                                                    |
|--------------|---------|----------------------------------------------------------------|
| `version`    | string  | Version string from `mqvpn_version_string()`                   |
| `scheduler`  | string  | Active multipath scheduler: `minrtt`, `wlb`, `wlb_udp_pin`, `backup_fec`, or `unknown`. Client mode: the client's own configured `Scheduler`/`scheduler` setting (previously always reported `"unknown"` regardless of the real configured value — fixed, see §7). |
| `fec_enabled`| integer | `1` if built with `XQC_ENABLE_FEC`; `0` otherwise             |

---

### 5.7 `get_fec_stats` *(new in v0.5.0)*

Return per-user FEC and multipath statistics for an active session. Requires
that the server was built with `XQC_ENABLE_FEC`.

> **Client mode.** There is only ever one possible identity from this side —
> this client's own upstream connection — so `user` must match this
> client's own configured `auth_username` (or `"(self)"` if unset); any
> other value returns `"user not found"`, same as a genuinely unknown
> username would server-side. `mp_state`/`mp_state_label` are derived from
> this client's own `xqc_conn_stats_t` snapshot, same derivation as the
> server uses. Previously always failed with `"fec not built"` regardless
> of whether FEC was actually compiled in — fixed, see §7.

**Request**

```json
{"cmd":"get_fec_stats","user":"alice"}
```

| Field  | Type   | Required | Description                  |
|--------|--------|----------|------------------------------|
| `user` | string | yes      | Username of the active session |

**Response — success**

```json
{
  "ok":true,"user":"alice",
  "enable_fec":1,"mp_state":1,"mp_state_label":"active_with_standby",
  "fec_send_cnt":142,"fec_recover_cnt":17,
  "lost_dgram_cnt":23,
  "total_app_bytes":9123456,"standby_app_bytes":421337
}
```

| Field               | Type    | Description                                                       |
|---------------------|---------|-------------------------------------------------------------------|
| `user`              | string  | Echoed username from the request                                  |
| `enable_fec`        | uint    | `1` if FEC is enabled for this session, `0` otherwise            |
| `mp_state`          | uint    | Raw `xqc_conn_stats_t.mp_state` (xquic). Takes only the values `0` (no multipath attempted: `create_path_count <= 1`), `1` (multipath established and validated: `create_path_count > 1 && validated_path_count > 1`), or `2` (multipath attempted but not validated: `create_path_count > 1 && validated_path_count <= 1`). **Legacy diagnostic only — prefer `mp_state_label` for alerts and dashboards.** |
| `mp_state_label`    | string  | mqvpn-derived label that walks `xqc_conn_stats_t.paths_info[]` and classifies the active paths by `path_app_status`. Independent of the numeric `mp_state`. One of `single_path` (no multipath or only one active path), `active_with_standby` (≥ 2 active paths, mix of available + standby — full redundancy), `standby_only` (≥ 1 standby and zero available — primary down, **degraded**), `active_only` (≥ 2 active paths, all available, no standby designated), or `unknown` (NULL stats). Added in v0.5.0. |
| `fec_send_cnt`      | uint64  | FEC repair packets sent (xquic uint32 widened to uint64)          |
| `fec_recover_cnt`   | uint64  | Packets recovered via FEC (xquic uint32 widened to uint64)        |
| `lost_dgram_cnt`    | uint64  | QUIC datagrams lost for this session                              |
| `total_app_bytes`   | uint64  | Total application bytes delivered via all paths                   |
| `standby_app_bytes` | uint64  | Application bytes delivered via the standby path                  |

**Response — errors**

```json
{"ok":false,"error":"user required"}
{"ok":false,"error":"user not found"}
{"ok":false,"error":"fec not built"}
```

- `"user required"` — the `user` field is missing or cannot be parsed.
- `"user not found"` — no active session exists for that username.
- `"fec not built"` — the server was compiled without `XQC_ENABLE_FEC`, or an
  internal null-pointer guard was triggered. Check `get_build_info` first.

---

### 5.8 `get_all_fec_stats` *(new in v0.5.0)*

Bulk variant of `get_fec_stats`: returns one entry per active (tunnel-
established) session in a single response. Designed for scrapers (e.g. the
Prometheus exporter) that would otherwise issue one TCP connection per user
and risk hitting the `CTRL_MAX_CONNS=8` concurrency cap.

Requires that the server was built with `XQC_ENABLE_FEC`.

> **Client mode.** `n_clients`/`clients` describe this client's own
> connection (0 or 1 entries), same semantics as `get_status`'s client-mode
> behavior. Previously always failed with `"fec not built"` regardless of
> whether FEC was actually compiled in — fixed, see §7.

**Request**

```json
{"cmd":"get_all_fec_stats"}
```

**Response — success**

```json
{
  "ok":true,"n_clients":2,
  "clients":[
    {"user":"alice","enable_fec":1,"mp_state":1,"mp_state_label":"active_with_standby",
     "fec_send_cnt":142,"fec_recover_cnt":17,"lost_dgram_cnt":23,
     "total_app_bytes":9123456,"standby_app_bytes":421337},
    {"user":"bob","enable_fec":1,"mp_state":0,"mp_state_label":"single_path",
     "fec_send_cnt":0,"fec_recover_cnt":0,"lost_dgram_cnt":0,
     "total_app_bytes":555555,"standby_app_bytes":0}
  ]
}
```

| Field        | Type    | Description                                                                 |
|--------------|---------|-----------------------------------------------------------------------------|
| `n_clients`  | integer | Number of entries in `clients`. May be `0` if no sessions are active. Same nomenclature as `get_status` (a connected user is a "client"; "users" is reserved for `list_users`'s registered auth-table superset). |
| `clients`    | array   | One entry per active session. Each entry has the same fields as `get_fec_stats` minus the request echo, plus `mp_state_label`. |

**Response — errors**

```json
{"ok":false,"error":"fec not built"}
{"ok":false,"error":"response too large"}
```

- `"fec not built"` — same semantics as `get_fec_stats`. Consumers should
  stop probing FEC for the rest of this scrape.
- `"response too large"` — only possible if the user count grows beyond what
  fits in the 128 KB internal response buffer (currently
  `MQVPN_MAX_USERS=64`, well within budget).

---

### 5.9 `get_reorder_stats`

Return aggregate reorder-shim RX statistics (§17) — a fixed-shape object
covering every live connection that has a reorder engine active
(`[Reorder]`/`reorder.mode != OFF`). Available in both server and client
mode.

**Request**

```json
{"cmd":"get_reorder_stats"}
```

**Response — success**

```json
{
  "ok":true,
  "reorder":{
    "gap_count":12,"gap_filled_count":9,
    "gap_timeout_count":2,"gap_overflow_count":1,
    "gap_demote_count":0,"gap_reset_count":0,
    "ack_demote_count":3,
    "too_late_drop_count":0,"too_far_ahead_drop_count":0,
    "duplicate_drop_count":0,"pool_drop_count":0,
    "per_flow_limit_drop_count":0,"reset_discard_count":0,
    "delivered_count":10234,
    "added_latency_p99_ms":4.230,"added_latency_max_ms":18.500,
    "added_latency_buffered_p99_ms":2.100
  }
}
```

| Field                          | Type    | Description                                             |
|--------------------------------|---------|----------------------------------------------------------|
| `gap_count`                    | uint64  | Gap episodes opened (an out-of-order arrival started a hold) |
| `gap_filled_count`             | uint64  | Gaps closed by the missing packet arriving in time        |
| `gap_timeout_count`            | uint64  | Gaps closed by the hold timer expiring                    |
| `gap_overflow_count`           | uint64  | Gaps closed because the reorder buffer pool overflowed     |
| `gap_demote_count`             | uint64  | Gaps closed by an ACK-driven demotion                      |
| `gap_reset_count`              | uint64  | Gaps closed by a connection/flow reset                     |
| `ack_demote_count`             | uint64  | Flows demoted by ACK signal (not gap-scoped)                |
| `too_late_drop_count`          | uint64  | Packets dropped: arrived after their sequence window closed |
| `too_far_ahead_drop_count`     | uint64  | Packets dropped: too far ahead of the expected sequence      |
| `duplicate_drop_count`         | uint64  | Duplicate packets dropped                                    |
| `pool_drop_count`              | uint64  | Packets dropped: reorder buffer pool exhausted                |
| `per_flow_limit_drop_count`    | uint64  | Packets dropped: per-flow buffering cap reached                |
| `reset_discard_count`          | uint64  | Buffered packets discarded on a flow/connection reset            |
| `delivered_count`              | uint64  | Packets successfully delivered through the reorder shim (in-order or after a filled gap) |
| `added_latency_p99_ms`         | double  | P99 latency added by buffering (milliseconds)                    |
| `added_latency_max_ms`         | double  | Max latency added by buffering (milliseconds)                    |
| `added_latency_buffered_p99_ms`| double  | P99 latency for packets that were actually held (excludes pass-through), milliseconds |

A server/client with no reorder-enabled connection returns an all-zero
`reorder` object — this is a valid result, not an error.

**Response — errors**

```json
{"ok":false,"error":"internal error"}
```

- `"internal error"` — the underlying getter returned failure (a NULL
  server/client handle; should not occur in practice). **Client mode: fixed
  in this release** — every client-mode `get_reorder_stats` request
  previously failed with this exact error unconditionally, regardless of
  whether reorder buffering was configured or a tunnel was even up. See §7.

---

## 6. Error Reference

| Error string             | Returned by                                    |
|--------------------------|------------------------------------------------|
| `"missing cmd"`          | any request (no `cmd` field present)           |
| `"unknown cmd"`          | any request with an unrecognised `cmd` value   |
| `"too many connections"` | connection accepted when 8 slots already used  |
| `"response too large"`   | any command (internal buffer overflow guard)   |
| `"name and key required"`| `add_user`                                     |
| `"add_user failed (-N)"` | `add_user` (N = underlying error code)         |
| `"name required"`        | `remove_user`                                  |
| `"user not found"`       | `remove_user`, `get_fec_stats`                 |
| `"user required"`        | `get_fec_stats`                                |
| `"fec not built"`        | `get_fec_stats`, `get_all_fec_stats`           |
| `"internal error"`       | `get_reorder_stats`                            |

---

## 7. Compatibility

mqvpn follows semantic versioning for the control API:

- **Additive changes** (new commands, new optional response fields) are
  backward-compatible and do not require a version bump beyond a minor version
  increment.
- **Existing field names and JSON key order** within a response are stable
  across patch and minor releases. Consumers must still tolerate unknown fields
  (future-proofing).
- **Removing or renaming** a command or a response field is a
  **breaking change** and requires a major version bump.
- `get_stats` was extended in v0.5.0 with `dgram_sent`, `dgram_recv`,
  `dgram_lost`, `dgram_acked`, and `uptime_sec`. Consumers that only read
  `n_clients`, `bytes_tx`, and `bytes_rx` remain unaffected.
- `get_stats` was extended again in v0.9.0 with the hybrid-mode
  lane/flow counters (`pkts_lane_tcp`, `pkts_lane_dgram`, `pkts_lane_raw`,
  `pkts_lane_tcp_dropped`, `tcp_flows_active`, `tcp_flows_total`,
  `tcp_flows_rejected`, `raw_markers_active`). All eight now report real
  values on the client with `Tcp = stream` or `Tcp = auto`
  (`tcp_flows_active`, `tcp_flows_total`, `tcp_flows_rejected`, and
  `raw_markers_active` were previously stubbed at 0); `tcp_flows_active`,
  `tcp_flows_total`, and `tcp_flows_rejected` are also wired server-side.
  In the same change, `pkts_lane_tcp` changed meaning — from "packets
  classified into the TCP lane" (classify-time) to "packets actually handed
  to the TCP lane / lwIP" (post-sticky-verdict), so under `Tcp = auto` a
  sticky-RAW flow's packets now count in `pkts_lane_raw` instead. See
  §5.4's field table for which fields stay 0 server-side by design.
  Existing JSON consumers remain unaffected. Note for C API consumers: this
  is the first time `mqvpn_stats_t` has grown since its introduction — the
  struct producers write `sizeof(mqvpn_stats_t)` bytes, so binaries linked
  against the shared library must be recompiled against the new header. The
  shared-library SOVERSION was bumped 1 → 2 for exactly this reason.
- `get_stats` was extended in v0.16.0 with the outer-UDP offload counters
  `udp_tx_sends`, `udp_tx_datagrams`, `udp_rx_receives`, and
  `udp_rx_datagrams`. Existing JSON consumers remain unaffected. The two
  transmit-side fields also grow `mqvpn_stats_t`, so the same C-API caveat as
  the v0.9.0 entry above applies: `mqvpn_client_get_stats` /
  `mqvpn_server_get_stats` write `sizeof(mqvpn_stats_t)` bytes without
  consulting the caller's `struct_size`, so binaries linked against the shared
  library must be recompiled against the new header — the shared-library
  SOVERSION was bumped 2 → 3 for exactly this reason.
  The receive-side pair is deliberately NOT in `mqvpn_stats_t`:
  UDP GRO is configured and un-coalesced entirely in the platform layer, which
  the library never observes, so those two are read straight from the platform
  by the control socket (`ctrl_socket_create`) instead.
- `get_status` paths gained a `state_label` string in v0.5.0 alongside the
  existing numeric `state`; `get_fec_stats` gained `mp_state_label` similarly.
  Consumers that only read the numeric fields remain unaffected, but new code
  should prefer the labels — the numeric values are raw xquic enums and may
  shift if upstream re-orders them.
- `get_all_fec_stats` was added in v0.5.0 as a bulk variant of `get_fec_stats`.
- **Bugfix, all client-mode consumers:** `get_stats`, `get_status`,
  `get_build_info`, `get_fec_stats`, `get_all_fec_stats`, and
  `get_reorder_stats` unconditionally read the server handle (`NULL` by
  design in client mode — only `add_path`/`remove_path`/`list_paths`/
  `set_path_weight`/`set_path_dscp_mask` correctly gated on client mode
  before this fix). Every field documented above as "client-side" or
  "both modes" was therefore silently wrong on a client-mode process:
  `get_stats` read all-zero except the two platform-sourced UDP-receive
  counters, `get_status` always reported `n_clients:0`, `get_build_info`'s
  `scheduler` always read `"unknown"`, and `get_fec_stats`/
  `get_all_fec_stats`/`get_reorder_stats` always failed outright
  (`"fec not built"` / `"internal error"`) regardless of real
  configuration or connection state. Confirmed live on an OpenMPTCProuter
  client-mode router with a real, traffic-carrying tunnel (`get_stats`
  showed real `udp_rx_receives` alongside all-zero `bytes_tx`/`dgram_*`/
  `uptime_sec`; `get_reorder_stats` returned `{"ok":false,"error":
  "internal error"}` unconditionally). `mqvpn_client_get_reorder_stats()`
  already existed and was wired into the Android/iOS bridges but was never
  connected to the Linux control socket — a merge-artifact gap (that
  handler's origin branch predates `cli_ctx`; the merge that introduced
  `cli_ctx` elsewhere never added a branch here). This is purely additive
  on the wire (no field renamed or removed; `n_clients`/`clients`/`user`
  keep their existing meaning, just now correctly populated in client mode
  too) — no version bump required for JSON consumers. New internal C
  helpers (`mqvpn_client_get_info`, `mqvpn_client_get_reinject`,
  `mqvpn_client_get_fec_stats`, `mqvpn_client_scheduler_label`,
  `mqvpn_client_uptime_seconds`) are `MQVPN_INTERNAL`, not part of the
  public `libmqvpn.h` ABI.

---

## 8. Examples (CLI)

These commands can be pasted directly into a terminal on the server host. The
`-q1` flag tells `nc` to close the write side 1 second after EOF, allowing the
server's response to be read before the connection drops.

```bash
# Query overall health
echo '{"cmd":"get_status"}' | nc -q1 127.0.0.1 9090

# Query build metadata (scheduler, FEC support)
echo '{"cmd":"get_build_info"}' | nc -q1 127.0.0.1 9090

# Query aggregate server stats (bytes, datagrams, uptime)
echo '{"cmd":"get_stats"}' | nc -q1 127.0.0.1 9090

# Query per-user FEC counters
echo '{"cmd":"get_fec_stats","user":"alice"}' | nc -q1 127.0.0.1 9090

# Query FEC counters for ALL active sessions (bulk; preferred for scrapers)
echo '{"cmd":"get_all_fec_stats"}' | nc -q1 127.0.0.1 9090

# List registered users
echo '{"cmd":"list_users"}' | nc -q1 127.0.0.1 9090

# Add a user at runtime
echo '{"cmd":"add_user","name":"carol","key":"carol-secret"}' | nc -q1 127.0.0.1 9090

# Remove a user at runtime
echo '{"cmd":"remove_user","name":"carol"}' | nc -q1 127.0.0.1 9090
```

On macOS, replace `-q1` with `-G 1` (BSD netcat).

---

## 9. Hybrid mode configuration keys

Hybrid mode terminates inner TCP connections locally (client-side, via an
embedded lwIP stack) and relays them over a dedicated HTTP/3 request stream
instead of the datagram CONNECT-IP path. This section is the config-key
reference; see the README's "Hybrid mode (TCP lane)" section for the lane
diagram and a minimal example. Same `[Hybrid]` section in both INI
(`server.conf`/`client.conf`) and JSON (`"hybrid": {...}`) config — JSON
keys are the snake_case column below.

| INI key                | JSON key                  | Type    | Default | Applies to     | Description |
|-------------------------|---------------------------|---------|---------|----------------|-------------|
| `Enabled`               | `enabled`                 | bool    | `false` | client + server | Turn hybrid mode on. Disabled by default — existing deployments see no behavior change. |
| `Tcp`                   | `tcp`                     | string  | `auto`  | client only     | Per-flow TCP lane policy: `stream` (always relay via the TCP lane), `raw` (never — inner TCP stays on the CONNECT-IP datagram path, byte-identical to hybrid disabled), or `auto` (per-flow: TCP lane once ≥2 paths are active at SYN time, RAW otherwise; the decision is latched for the flow's lifetime, never re-evaluated). |
| `TcpMaxFlows`           | `tcp_max_flows`           | uint32  | `256`   | client + server | Cap on concurrently open TCP-lane flows. Client: caps the local flow table (a SYN over the cap falls back to RAW pre-lwIP, counted in `tcp_flows_rejected`). Server: caps concurrent egress flows per client session (a SYN over the cap gets an HTTP `503`). On the client the honored value is additionally clamped to half the build profile's lwIP pcb pool — 4096 on desktop/router, 256 on Android, 64 on iOS — and the clamp is logged at startup and at lane creation. Raising it for a router deployment needs the matching raise on the **server** too, since the per-session cap here is enforced independently on each side. |
| `TcpIdleTimeoutSec`     | `tcp_idle_timeout_sec`    | uint32  | `300`   | client + server | Idle-eviction timeout for TCP-lane flows (no activity for this long tears the flow down). `0` disables idle eviction (flows live for the whole connection lifetime). |
| `TcpConnectTimeoutSec`  | `tcp_connect_timeout_sec` | uint32  | `10`    | server only     | Timeout for the server's egress `connect()` to the requested target; on expiry the client gets an HTTP `504`. |
| `TcpMaxGlobalFlows`     | `tcp_max_global_flows`    | uint32  | `4096`  | server only     | Whole-server cap on concurrent egress TCP flows, across all client sessions (independent of the per-session `TcpMaxFlows`). A SYN over the cap gets an HTTP `503`. |
| `EgressAllow`           | `egress_allow`            | CIDR list (repeatable) | *(none)* | server only | Punches a hole through the mandatory default-deny for egress targets inside RFC1918/loopback/link-local ranges (e.g. `EgressAllow = 10.222.0.0/24`). Up to 32 entries. |
| `EgressDeny`            | `egress_deny`             | CIDR list (repeatable) | *(none)* | server only | Additional egress targets to block, evaluated after `EgressAllow`. Up to 32 entries. |

Notes:
- `Tcp`/`tcp` is a client-only knob — the server has no concept of a
  per-flow lane policy, it only relays whatever the client's classifier
  routes to it.
- The egress ACL default-denies RFC1918 (`10.0.0.0/8`, `172.16.0.0/12`,
  `192.168.0.0/16`), loopback, and link-local ranges even with no
  `EgressAllow`/`EgressDeny` configured — this is a safety default against a
  compromised or misconfigured client using the server as an internal-network
  pivot, not an opt-in feature.
- `TcpBpHighWater`/`TcpBpLowWater` (uplink backpressure watermarks) are
  internal compile-time constants, not configurable — see
  `src/hybrid/tcp_lane.h` if you need to know their values.
- See §5.4 (`get_stats`) for the runtime counters this config surfaces.

### Known limitations

- **TCP to private targets needs an explicit `EgressAllow`.** The client's
  TCP-lane classifier has no visibility into the server's `EgressAllow` list,
  so it cannot decide up front whether a given RFC1918/CGNAT/link-local/
  loopback target will actually be allowed egress — it always tries the TCP
  lane for such targets when `Tcp = stream` or `Tcp = auto` picks TCP. If the
  server's default-deny egress ACL (see the Notes above) then rejects the
  connect, the failure surfaces *after* the inner app already saw its
  `connect()` succeed: lwIP answers the inner SYN with a SYN-ACK locally
  (the TCP-lane accept happens before the server's egress `connect()` is
  attempted), so the app only finds out via a subsequent RST once the
  server's `connect()` is denied — not a clean, immediate refusal. UDP and
  RAW-lane traffic to the same targets are unaffected (no local SYN-ACK
  step). Operators who need TCP to a private target through the hybrid lane
  must add an `EgressAllow` entry covering it.
- **Client-address pools wider than `/24` can deny intra-VPN TCP between
  clients.** The client only exempts its *own* `/24` from the TCP lane (to
  keep client-to-client traffic within that `/24` off the egress-ACL path);
  with a wider pool, two clients in different `/24`s of the same pool hit the
  server's default-deny egress ACL for TCP between them, same as any other
  RFC1918 target. Use a `/24`-or-narrower client-address pool, or an
  `EgressAllow` entry covering the pool, if you need TCP between clients on
  different `/24`s. The server logs a startup warning when its configured
  pool is wider than `/24` for this reason.
