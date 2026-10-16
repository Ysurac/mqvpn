# Configuration

mqvpn supports both INI and JSON config files. If the file content starts with `{`, it is parsed as JSON; otherwise as INI. CLI arguments override config values.

## INI Format

Syntax rules:

- Section names and keys are case-insensitive.
- A line whose first non-space character is `#` or `;` is a comment. A `#` or `;` that follows a space or tab also starts a comment, so `Scheduler = wlb   # default` sets `wlb`. A `#` or `;` written directly after other characters stays part of the value (`a#b` is read as `a#b`). Keys from `mqvpn --genkey` (base64) and normal paths never contain a space followed by `#` or `;`; a value that really needs one cannot be written in INI (use JSON).
- Booleans accept `true`/`false`, `yes`/`no`, `on`/`off` and `1`/`0`, in any case. Any other value logs a warning and the key keeps its default.
- A value longer than the setting allows is ignored with a warning (it is never cut short). Unknown keys and sections log a warning with the line number.
- If a scalar key appears twice, the last line wins. In JSON, the first occurrence of a key wins.
- If the file holds a pre-shared key (`[Auth] Key` or `User`) and other users can read it, mqvpn logs a warning at startup. Restrict it with `chmod 600`.

### Server

```ini
# /etc/mqvpn/server.conf
[Interface]
Listen = 0.0.0.0:443
Subnet = 10.0.0.0/24
Subnet6 = 2001:db8:1::/112
# MTU = 1280

[TLS]
Cert = /etc/mqvpn/server.crt
Key = /etc/mqvpn/server.key

[Auth]
Key = mPyVpoQWcp/5gr404xvS19aRC03o0XS2mrb2tZJ1Ii4=
User = alice:<ALICE_PSK>
User = bob:<BOB_PSK>

[Multipath]
Scheduler = wlb
# CC = bbr2                     # Congestion control (bbr2|bbr|cubic|new_reno|copa|unlimited|none)

[Hybrid]
Enabled = true

[Reorder]
Enabled = on
```

When you use a CA-issued certificate, `Cert` (`cert_file` in JSON) must contain
the full chain: the server certificate followed by its intermediates (for Let's
Encrypt, `fullchain.pem`, not `cert.pem`). Clients verify the chain exactly as
the server presents it and do not fetch missing intermediates, so a leaf-only
file fails verification on any client that does not already trust the
intermediate.

### Client

```ini
# /etc/mqvpn/client.conf
[Server]
Address = 203.0.113.1:443
# ServerName = vpn.example.com  # TLS SNI / cert verify name (default: use Address host)
# PinnedPubkey = sha256//<base64>  # accept only this server key (replaces CA checks, overrides Insecure)

[Auth]
Key = mPyVpoQWcp/5gr404xvS19aRC03o0XS2mrb2tZJ1Ii4=

[Interface]
TunName = mqvpn0
DNS = 1.1.1.1, 8.8.8.8
LogLevel = info
# MTU = 1280

[Multipath]
Scheduler = wlb
# CC = bbr2                     # Congestion control (bbr2|bbr|cubic|new_reno|copa|unlimited|none)
Path = eth0
Path = wlan0

# [Hybrid]                      # opt in to carry inner TCP over QUIC streams; see the [Hybrid] section
# Enabled = true

# [Reorder]                     # opt in for bulk inner QUIC; see the [Reorder] section
# Enabled = on
```

## JSON Format

JSON config is useful for structured management and automation tooling.

### Server

```json
{
  "mode": "server",
  "listen": "0.0.0.0:443",
  "tun_name": "mqvpn0",
  "log_level": "info",
  "subnet": "10.0.0.0/24",
  "subnet6": "2001:db8:1::/112",
  "cert_file": "/etc/mqvpn/server.crt",
  "key_file": "/etc/mqvpn/server.key",
  "auth_key": "<YOUR_PSK_HERE>",
  "users": [
    { "name": "alice", "key": "<ALICE_PSK>" },
    { "name": "bob", "key": "<BOB_PSK>" }
  ],
  "max_clients": 64,
  "scheduler": "wlb",
  "cc": "bbr2",
  "hybrid": { "enabled": true },
  "reorder": { "enabled": "on" }
}
```

`cert_file` takes the full chain as well (see the note under the INI server example).

### Client

```json
{
  "mode": "client",
  "server_addr": "203.0.113.1:443",
  "tls_server_name": "vpn.example.com",
  "tun_name": "mqvpn0",
  "log_level": "info",
  "auth_key": "<YOUR_PSK_HERE>",
  "insecure": false,
  "pinned_pubkey": "",
  "dns": ["1.1.1.1", "8.8.8.8"],
  "kill_switch": false,
  "reconnect": true,
  "reconnect_interval": 5,
  "scheduler": "wlb",
  "cc": "bbr2",
  "paths": ["eth0", "wlan0"]
}
```

## Multi-User Authentication

The server can authenticate multiple users, each with their own PSK. In JSON config, add a `users` array where each entry is either an object (`{"name":"alice","key":"..."}`) or a shorthand string (`"alice:key"`). In INI config, use repeatable `User = NAME:KEY` lines in the `[Auth]` section. You can also use the [Control API](#control-api) to manage users at runtime.

When both `auth_key` (global key) and `users` are set, clients can authenticate with either. To restrict access to named users only, remove `auth_key` from the config.

Removing a user via the Control API also disconnects any active sessions authenticated with that username.

::: warning Monitoring requires per-user keys
Sharing a single `auth_key` across multiple clients works for the VPN data plane, but the Control API and the Prometheus exporter identify clients by their `user` label. Sessions authenticated with the global `auth_key` are reported as `user="(global)"`, so multiple clients collide on the same label and the Prometheus scrape is dropped. For multi-client monitoring give each client its own entry under `users` (or register them at runtime via `add_user`).
:::

## Running with Config Files

```bash
sudo mqvpn --config /etc/mqvpn/server.conf
sudo mqvpn --config /etc/mqvpn/server.json
```

## Config Reference

### `[Server]` (client only)

| Key | Description | Default |
|-----|-------------|---------|
| `Address` | Server address (`HOST:PORT`, e.g. `[2001:db8::1]:443` for IPv6) | Required |
| `ServerName` | TLS SNI and certificate verification name. Use when connecting by IP but verifying against a domain certificate | Address host |
| `Insecure` | Skip TLS certificate verification (self-signed test setups only). Any server then receives the auth key; prefer `PinnedPubkey` for a self-signed server. With `false` the certificate is verified against the system store (`/etc/ssl`; override with `SSL_CERT_FILE` / `SSL_CERT_DIR`); on Linux/macOS (and on Windows with a custom trust path), when connecting by IP address set `ServerName` to the certificate's DNS name; with the Windows certificate store an IP literal matches an `iPAddress` SAN directly. On Windows the certificate is verified against the Local Machine trust store only (a private CA root must be installed there, or use `SSL_CERT_FILE`), and a certificate the local revocation cache lists as revoked is rejected; if `SSL_CERT_FILE` (a PEM bundle) or `SSL_CERT_DIR` (a hashed certificate directory) is set, that custom trust path is used instead, as on Linux. On Android the certificate is verified against the device's CA store and the app's network security config; `tlsServerName` (the SDK counterpart of `ServerName`) is the name it must match, and it must be a bare host name or IP literal (no brackets). | `false` |
| `PinnedPubkey` | Accept only a server holding this public key: base64 SHA-256 of the certificate's SubjectPublicKeyInfo (`openssl x509 -pubkey -noout \| openssl pkey -pubin -outform der \| openssl dgst -sha256 -binary \| openssl enc -base64`), `sha256//` prefix optional, up to 4 separated by `;`. Replaces CA, hostname and expiry validation and overrides `Insecure`; checked before the auth key is sent | (none) |

### `[Interface]`

| Key | Description | Default |
|-----|-------------|---------|
| `Listen` | Listen address (`HOST:PORT`, server only) | `0.0.0.0:443` |
| `Subnet` | Client IPv4 pool (server only) | `10.0.0.0/24` |
| `Subnet6` | Client IPv6 pool (server only). Must be large enough for the IPv4 pool: each client gets the IPv6 address at the same offset as its IPv4 one, so a prefix too small for `Subnet` is refused at startup | — |
| `TunName` | TUN device name | `mqvpn0` |
| `DNS` | DNS servers (comma-separated, client only, up to 4). Each entry must be an IP address (IPv4, or IPv6 optionally with `%scope`); other values are ignored with a warning. On Windows mqvpn also adds a name resolution policy (NRPT) rule so that every DNS query goes to these servers and none to other network adapters' servers; the rule is removed when mqvpn exits, and one left behind by a crashed mqvpn is removed at its next start (or at reboot). | — |
| `LogLevel` | Log level (`debug`, `info`, `warn`, `error`) | `info` |
| `KillSwitch` | Block traffic outside the VPN tunnel (client only). Active from startup until the process exits, including while disconnected or reconnecting and after the connection is given up (`Reconnect = false`). Always blocks IPv6 outside the tunnel too, even when the tunnel carries IPv4 only; DHCP and IPv6 neighbor discovery stay allowed. Also stops applications that bind to a specific network adapter from bypassing the tunnel. With the kill switch on, re-resolving a server host name during a reconnect only works if the DNS resolver is reachable while blocked; otherwise the previously resolved address is kept. | `false` |
| `Reconnect` | Enable automatic reconnection (client only) | `true` |
| `ReconnectInterval` | Wait in seconds before the first reconnection attempt (client only). Each further failed attempt doubles the wait, up to 60 seconds; values above 60 act as 60. The wait starts again from this value after a successful connection. | `5` |
| `ManageRoutes` | Manage the host routing table (VPN routes and server pin route). Set to `false` (or pass `--no-manage-routes`) to handle routing yourself; the log then shows `manage_routes=off: host routing table left untouched`. Same switch as `NoRoutes = true`: routes are installed only when `ManageRoutes` is true and `NoRoutes` is false. Routing alone does not stop applications that bind their sockets to a specific adapter — see the note below. | `true` |
| `NoRoutes` | Skip all automatic route setup and manage routes yourself (client only; CLI `--no-routes`). Equivalent to `ManageRoutes = false`. | `false` |
| `RouteViaServer` | Route all traffic through the server's tunnel address instead of the tunnel interface: installs the catch-all routes `0.0.0.0/1` and `128.0.0.0/1` via the server's tunnel IP (client only; CLI `--route-via-server`). The host's own default route is left untouched. | `false` |
| `MTU` | TUN MTU (1280–9000). Client: cap — if the negotiated MTU is lower, the negotiated value is used. Server: sets the TUN MTU directly. | auto (client ~1382 negotiated, server 1382) |

Give `Subnet` and `Subnet6` as network addresses (`10.0.0.0/24`, `2001:db8:1::/112`). An address that is not the start of its network is used as given, not rounded down: the server takes the next address and clients the ones after it, so the whole pool is shifted. For IPv6, a shifted pool that no longer fits in the prefix is refused at startup; for IPv4 the pool is cut short at the prefix's last host (with a warning), so fewer client addresses are available.

> **Note: route management does not cover interface-bound applications**
>
> mqvpn adds a split default route (`0.0.0.0/1` + `128.0.0.0/1`) instead of replacing the system default route, so the original default route stays in place. Applications that pick a network adapter by looking up the *system default route* — rather than the route to the destination — keep using the physical adapter and bypass the tunnel. Tailscale on Windows does this for its own sockets.
>
> This is independent of `ManageRoutes`. To stop such applications, set `KillSwitch = true`. Note that they are then **blocked outright rather than routed through the tunnel**.

### `[TLS]`

| Key | Description | Default |
|-----|-------------|---------|
| `Cert` | TLS certificate path (PEM, server only) | `server.crt` in the working directory |
| `Key` | TLS private key path (PEM, server only) | `server.key` in the working directory |
| `Cipher` / `Ciphers` | TLS cipher suites, colon-separated (e.g. `TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384`); client and server; CLI `--cipher` | TLS library default |

Always set `Cert` and `Key` explicitly for a server: the defaults are file names relative to the directory mqvpn is started from.

### `[Auth]`

| Key | Description | Default |
|-----|-------------|---------|
| `Key` | Pre-shared key (base64, generate with `mqvpn --genkey`) | Required unless `User` is set |
| `User` | Server: per-user PSK in `NAME:KEY` or `NAME:KEY:FIXED_IP` format (repeatable). Client: `NAME:KEY` uses that name and key; a bare `NAME` sets the client username like `Username`. | — |
| `Username` | Client only: name sent to the server to identify this client in its status output and logs; it does not affect authentication | — |
| `MaxClients` | Maximum concurrent clients (server only) | `64` |
| `MaxSessionsPerUser` | Server only: maximum concurrent tunnels per credential, so one key cannot take every `MaxClients` slot. `0` = no limit | `0` |

User names are 1–63 bytes and may not contain `"`, `\` or control characters; keys are at most 255 bytes. An invalid `--user` on the command line stops startup with an error; an invalid `User` line in a file is skipped with a warning.

In JSON, use `auth_key` on both client and server (as in the examples above).

`MaxSessionsPerUser` counts tunnels per credential: each `User` is counted on its own, and every client that connects with the shared `Key` counts against one common budget, whatever name it sends with `Username` (that name is not authenticated). It only applies when authentication is configured. A client over the limit is refused like a client of a full server (HTTP 403, reported as an authentication failure) and retries later. A user with a fixed IP who reconnects replaces its previous tunnel and is never refused by this limit. Other clients that reconnect while their old tunnel still exists on the server (for example after a network change) can be refused until the server notices the old tunnel is gone, so leave some headroom.

### `[Control]`

Enables the [Control API](#control-api), in both client and server mode. Disabled when no port is set.

| Key | Description | Default |
|-----|-------------|---------|
| `Listen` | Address and port in one value (`127.0.0.1:9090`, `[::1]:9090`) | — |
| `Port` (alias `ControlPort`) | TCP port | — |
| `Addr` (alias `ControlAddr`) | Bind address | `127.0.0.1` |

`Port` and `Addr` override the matching half of `Listen`; `--control-port` and `--control-addr` override both, and `--control-port 0` disables the API.

### `[Multipath]`

| Key | Description | Default |
|-----|-------------|---------|
| `Scheduler` | Scheduler algorithm: `minrtt`, `wlb`, `backup`, `wlb_udp_pin`, `backup_fec`, `rap`, `wrtt`, `wrr`, `redundant` or `dscp` | `wlb` |
| `AllowClientScheduler` | Server only (`allow_client_scheduler` in JSON, or pass `--allow-client-scheduler`). When `true`, a client that asks for a scheduler gets it for its own connection's download direction; other clients keep the server's `Scheduler`. The request is not applied, and the connection keeps the scheduler it already has (at first, the server's `Scheduler`), when the client asks for `backup_fec`, when the server's `Scheduler` is `backup_fec`, when the same connection already changed its scheduler once, or when the connection already uses more than one path. | `false` |
| `RequestServerScheduler` | Client only (`request_server_scheduler` in JSON, or pass `--request-server-scheduler`). When `true`, the client asks the server to use this client's `Scheduler` for its download direction too. Setting `Scheduler` alone still affects only the upload direction, as before. Takes effect only on servers with `AllowClientScheduler = true`. | `false` |
| `CC` (alias `CongestionControl`) | Congestion control algorithm: `bbr2`, `bbr`, `cubic`, `new_reno`, `copa`, `unlimited` or `none` | `bbr2` |
| `Path` | Network interface to bind (repeatable, client only, up to 8) | Default interface |
| `BackupPath` | Failover interface (repeatable, client only). It is brought up in standby and carries no traffic while a `Path` interface works; it takes over when every `Path` is down or failing. Exceptions: `redundant` also sends its copies over it, `backup_fec` sends its repair data over it, and `backup` may also use it while the `Path` interfaces are slow or overloaded | — |
| `Fec` / `FecEnable` | Enable forward error correction (needs an FEC-enabled build) | `false` |
| `FecScheme` | FEC scheme: `galois_calculation`, `packet_mask`, `reed_solomon` or `xor` | `reed_solomon` |
| `SyncPathLabels` | Share per-path weight / DSCP mask between client and server. Client: announce its own values to the server. Server: adopt the values the client announces for its downlink. Either side set to `false` turns the sharing off | `true` |
| `PushPathLabels` | Push a weight / DSCP mask pinned on the server down to the client, so it also governs the client's uplink. Server: send it. Client: accept it. Both sides must enable it | `false` |
| `InitMaxPathId` | MP-QUIC draft-21 test knob: initial Maximum Path Identifier advertised in transport parameters (`1`–`4294967295`; `0` = xquic default `8`) | `0` |
| `Reinjection` | Speculative duplication mode (`off`, `deadline`, `idle`, or `dgram`) | `off` |
| `ReinjectionSrttFactorPct` | `deadline` mode: duplicate unacked packets older than factor × min_srtt, in percent (`100`–`1000`) | `110` |
| `ReinjectionHardDeadlineMs` | `deadline` mode: upper clamp on the duplication deadline (`1`–`60000`) | `500` |
| `ReinjectionDeadlineLowerBoundMs` | `deadline` mode: lower clamp; values above the hard deadline are clamped down to it (`1`–`60000`) | `20` |

A server of this version tells each connected client which scheduler it uses for that client, even when `AllowClientScheduler` is `false`. A client that sets `RequestServerScheduler` logs `server scheduler: <name> (requested <name>)` after it connects.

See [Multipath](./multipath) for scheduler details and
[Reinjection](./multipath#reinjection-speculative-duplication) for per-mode guidance.

Older configs may use `ReinjectionControl = true` with `ReinjectionMode` (alias `ReinjCtl`, values `default`, `deadline` or `dgram`). These are still read, but only when `Reinjection` is left at `off`; `default` maps to `idle`. New configs should use `Reinjection`.

> `backup_fec` is experimental and requires both peers to run mqvpn ≥ 0.4.0
> with FEC build enabled (`-DXQC_ENABLE_FEC=ON -DXQC_ENABLE_XOR=ON`).
> See [Multipath](./multipath#backup-fec-experimental).

> `CC = none` (no congestion control) requires xquic built with `-DXQC_ENABLE_UNLIMITED=ON`.

### `[Reorder]`

A flow-aware reorder buffer for inner UDP traffic. It targets a single inner connection (e.g. inner QUIC) that is itself spread across multiple paths by mqvpn's multipath aggregation: by holding briefly out-of-order datagrams and delivering them in order, it reduces the reordering the inner endpoint sees. Disabled by default (`Enabled = off`); when off the section has no effect and packets are forwarded unchanged. It only takes effect when both the server and the client set `Enabled = on`; the client log then shows `peer advertised mqvpn-reorder`.

> **Scope:** the reorder buffer applies to **inner UDP flows only. Inner TCP is not handled by the reorder buffer.** For inner TCP, enable hybrid mode ([`[Hybrid]`](#hybrid) below) instead — the QUIC stream layer restores ordering.

| Key | Description | Default |
|-----|-------------|---------|
| `Enabled` | Master switch (`on` / `off`) | `off` |
| `MaxWaitMs` | How long to hold a gap before skipping the missing datagram (ms) | `30` |
| `CapPackets` | Max buffered datagrams per flow (power of two, at most `65536`) | `1024` |
| `MaxBytesPerFlow` | Max buffered bytes per flow | `1572864` |
| `ClassifyWindow` | Datagrams observed to classify a flow's direction (`0` disables ACK-direction demotion) | `64` |
| `AckDemoteMaxLarge` | Large-packet count at or below which a flow is judged ACK-direction | `3` |
| `SmallPacketThreshold` | Inner payload bytes splitting small vs. large | `200` |
| `ResetMarkPackets` | FLOW_RESET marks emitted when a flow restarts | `8` |
| `ResetIdleGraceMs` | Honor a FLOW_RESET only after the flow has been idle this long (ms) | `10000` |
| `MaxFlows` | Max tracked flows | `65536` |
| `GlobalMaxBytes` | Shared buffer byte budget across all flows. It also covers the reorder index memory (up to 32 bytes per window slot in use) | `67108864` |
| `IngressIdleSec` | Receiver idle eviction timeout (must be `< EgressIdleSec`) | `30` |
| `EgressIdleSec` | Sender idle eviction timeout | `300` |

Demotion in the ACK direction is automatic: a flow that looks like an ACK/control stream (mostly small packets) is moved to pass-through so it is never delayed. This is internal behaviour, not a configurable knob — tune it indirectly via `ClassifyWindow` / `AckDemoteMaxLarge` / `SmallPacketThreshold`.

Reorder is a throughput-vs-latency trade-off — it helps bulk transfers but delays every flow it touches: `MaxWaitMs` is how long the receiver waits for a missing datagram before skipping it; a gap that opens while another is already being waited for is deferred to the next wait, so a hold is nominally at most twice `MaxWaitMs` (buffer-limit flushes can release a gap earlier, and because the deadline is checked periodically, a wait can run slightly past it). Datagrams that arrive in order are never delayed. Since one tunnel usually carries both kinds of traffic, per-port `[ReorderRule]` sections let you enable it where it helps (bulk inner QUIC) and pass latency-sensitive traffic through (DNS, NTP, real-time UDP). Unmatched UDP is pass-through by default, so you only need rules for the ports you want reordered:

```ini
[Reorder]
Enabled = on

[ReorderRule]          # bulk inner QUIC: reorder on
Proto = udp
Port = 443
Profile = fiber_lte

[ReorderRule]          # DNS: never add latency
Proto = udp
Port = 53
Profile = default_udp
```

The same works in JSON. The `reorder` object uses snake_case keys mapping 1:1 to the INI keys above, and `reorder_rules` is an array of `{proto, port, profile}` objects (each rule may also carry optional `max_wait_ms` / `cap_packets` overrides):

```json
{
  "reorder": {
    "enabled": "on",
    "max_wait_ms": 30,
    "cap_packets": 1024,
    "max_bytes_per_flow": 1572864,
    "classify_window": 64,
    "ack_demote_max_large": 3,
    "small_packet_threshold": 200,
    "reset_mark_packets": 8,
    "reset_idle_grace_ms": 10000,
    "max_flows": 65536,
    "global_max_bytes": 67108864,
    "ingress_idle_sec": 30,
    "egress_idle_sec": 300
  },
  "reorder_rules": [
    { "proto": "udp", "port": 443, "profile": "cellular_bond" },
    { "proto": "udp", "port": 4500, "profile": "fiber_lte", "max_wait_ms": 50, "cap_packets": 2048 }
  ]
}
```

### Profile presets

Each profile carries an empirically-tuned `(MaxWaitMs, CapPackets)` preset. The values were chosen from a multipath `netem` sweep across 16 link environments — full methodology and per-environment data are in the [reorder-only multipath report](https://github.com/mp0rta/mqvpn/blob/main/docs/report/2026-06-18-reorder-only-datagram-multipath-connect-ip-en.md):

| Profile | `MaxWaitMs` | `CapPackets` | Notes |
|---------|------------:|-------------:|-------|
| `cellular_bond` | `50` | `1024` | Cellular bonding (e.g. dual-LTE) |
| `fiber_lte` | `50` | `2048` | Mixed fiber + LTE; larger cap for higher BDP |
| `quic_bulk` | `50` | `1024` | Back-compat alias of `cellular_bond` |
| `low_latency` | — | — | Reserved; no preset (inert) |
| `default_udp` | — | — | Matched but **not** reordered (pass-through / OFF) |

**Precedence** for a rule's effective `(MaxWaitMs, CapPackets)`, highest first:

1. The rule's own explicit `MaxWaitMs` / `CapPackets` key.
2. A global `[Reorder]` explicit `MaxWaitMs` / `CapPackets`.
3. The rule's profile preset (above).
4. The builtin default (`MaxWaitMs = 30`, `CapPackets = 1024`).

In short, a number written explicitly always beats the profile. This is why a config that sets a global `[Reorder] MaxWaitMs` alongside `Profile = quic_bulk` keeps using the explicit global value unchanged.

### When to enable reorder

Reorder is **off by default** and is meant to be opt-in only within its useful range. Empirically that range is an RTT spread of roughly **15–100 ms**, paths with jitter, or **asymmetric bandwidth**.

- **Strong bandwidth asymmetry (≳ 8:1):** consider raising `MaxWaitMs` to `150`–`200` (unverified — treat as a follow-up to validate on your link).
- **Extreme RTT spread (≥ 285 ms, GEO-satellite class):** reorder is a net loss here; leave it off for that traffic with `Profile = default_udp`.

### `[ReorderRule]` (repeatable)

| Key | Description | Default |
|-----|-------------|---------|
| `Proto` | Matched L4 protocol (`udp`) | `udp` |
| `Port` | Matched port (source or destination) | — |
| `Profile` | `cellular_bond`, `fiber_lte`, `quic_bulk`, `low_latency`, or `default_udp` | `quic_bulk` |
| `MaxWaitMs` | Per-rule override of the hold time (ms). `0` is rejected with a warning — to pass a port through untouched use `Profile = default_udp` instead | profile preset |
| `CapPackets` | Per-rule override of the per-flow buffer cap. Must be a non-zero power of two no larger than `65536`, or it is rejected with a warning | profile preset |

### `[Hybrid]`

Terminates inner TCP locally and relays it over an HTTP/3 request stream so a single TCP flow can aggregate multiple paths. Disabled by default. See [Hybrid Mode](./hybrid-mode) for the lane diagram, egress ACL semantics, and known limitations.

| Key | Description | Applies to | Default |
|-----|-------------|------------|---------|
| `Enabled` | Master switch | client + server | `false` |
| `Tcp` | Per-flow TCP lane policy: `stream` (always), `raw` (never — byte-identical to hybrid disabled), or `auto` (TCP lane once ≥2 paths are active at SYN time; latched for the flow's lifetime) | client | `auto` |
| `TcpMaxFlows` | Concurrent TCP-lane flow cap. **The client and the server enforce it on different machinery and fail differently** — see **Notes on `TcpMaxFlows`** right below this table | client + server | `256` |
| `TcpIdleTimeoutSec` | Idle-eviction timeout for TCP-lane flows; `0` disables idle eviction | client + server | `300` |
| `TcpConnectTimeoutSec` | Timeout for the server's egress `connect()`; on expiry the client gets HTTP `504` | server | `10` |
| `TcpMaxGlobalFlows` | Whole-server cap on concurrent egress TCP flows across all sessions | server | `4096` |
| `EgressAllow` | CIDR allowed through the default-deny egress ACL for private ranges (repeatable, up to 32) | server | — |
| `EgressDeny` | Additional CIDR to block, evaluated after `EgressAllow` (repeatable, up to 32) | server | — |

#### Notes on `TcpMaxFlows`

`TcpMaxFlows` behaves differently on the client and the server.
The two sides share the key name and nothing else: what is counted, and what happens when
the cap is hit, both differ.

| | client | server |
|---|---|---|
| Counted in | the lane's own flow table | per client session |
| Checked | **before lwIP ever sees the SYN** | when the CONNECT-TCP request arrives |
| Inner TCP terminated | yes, by lwIP inside the lane | no — relayed over ordinary kernel sockets |
| **Over the cap** | **degraded to the RAW lane** — the connection still works | **HTTP `503`**, and the client then **resets** that inner connection; no RAW fallback at that point |
| Memory per flow | **~0.75 MiB** (uplink-queue ceiling) | **~32 KiB** (two lazily-allocated 16 KiB relay chunks) plus one fd |
| Binding resource | RAM — worst case `TcpMaxFlows` × 0.75 MiB (≈ 192 MiB at the default 256, ≈ 3.0 GiB at 4096) | the **fd budget** — `TcpMaxGlobalFlows` is checked first, but size the RAM too: worst case `TcpMaxGlobalFlows` × 32 KiB (≈ 128 MiB at 4096) once every admitted flow has paused once in each direction |
| Clamped | to **half** the lwIP TCP pcb pool (below) | not clamped (no lwIP involved) |

On both sides, a flow whose receiver has stopped reading (a slow LAN device on the client, a slow target on the server) also holds data inside the QUIC stack: at most 256 KiB of unread data plus that stream's QUIC receive window (sized to the flow's throughput, up to 16 MiB). Past that, QUIC flow control stops the sender, so memory stays bounded.

**Client-side clamp:** the honored value is capped at half the lwIP TCP pcb pool —
`4096` on desktop/router builds (Linux, Windows, macOS), `256` on Android, `64`
with the [iOS lwIP profile](./hybrid-mode#ios-builds).

**It does not halve your quota.** You may run the full honored number of flows at once.
The other pool half is reserved as headroom for pcbs the flow table has stopped
counting — TIME_WAIT, LAST_ACK and CLOSING outlive the flow itself.


In JSON, the section is a `"hybrid"` object with snake_case keys (`enabled`, `tcp`, `tcp_max_flows`, `tcp_idle_timeout_sec`, `tcp_connect_timeout_sec`, `tcp_max_global_flows`, `egress_allow`, `egress_deny`).

> The egress ACL default-denies RFC1918, loopback, and link-local targets even
> with no `EgressAllow`/`EgressDeny` configured — a safety default against a
> compromised client using the server as an internal-network pivot.

### `[Advanced]`

| Key | Description | Default |
|-----|-------------|---------|
| `RecvRateLimit` | Conn-level receive-rate cap in bytes/sec; bounds the aggregate QUIC receive window to `rate x RTT`. Client-side only — the server ignores it (a server-side cap would throttle client upload). Maximum `10000000000` (10 GB/s); larger values are rejected with a warning and the key falls back to `0`. Leave `0` unless memory-constrained (mobile clients set this internally) | `0` (off) |
| `UdpGso` | Batches UDP sends with kernel GSO for lower CPU use at high throughput. Automatically falls back to regular batched sends on kernels without support (< 4.18). Linux and Android only; no effect elsewhere. Set to `false` to restore the previous per-packet send path | `true` |
| `UdpGro` | Lets the kernel hand over received packets in batches, lowering CPU use at high throughput. Automatically falls back to one-at-a-time receives on kernels without support (< 5.0). Linux only; no effect elsewhere. Set to `false` to restore the previous receive path | `true` |

In JSON, the section is an `"advanced"` object with snake_case keys (`recv_rate_limit`, `udp_gso`, `udp_gro`).

### JSON key names

JSON configs are flat (except the `reorder`, `hybrid` and `advanced` objects above). The keys map to INI as follows:

| INI | JSON |
|-----|------|
| `[Interface]` `TunName`, `Listen`, `Subnet`, `Subnet6`, `LogLevel`, `DNS`, `MTU` | `tun_name`, `listen`, `subnet`, `subnet6`, `log_level`, `dns` (array), `mtu` |
| `[Interface]` `KillSwitch`, `Reconnect`, `ReconnectInterval` | `kill_switch`, `reconnect`, `reconnect_interval` |
| `[Interface]` `ManageRoutes`, `NoRoutes`, `RouteViaServer` | `manage_routes`, `no_routes`, `route_via_server` |
| `[Server]` `Address`, `ServerName`, `Insecure`, `PinnedPubkey` | `server_addr`, `tls_server_name`, `insecure`, `pinned_pubkey` |
| `[TLS]` `Cert`, `Key`, `Cipher` / `Ciphers` | `cert_file`, `key_file`, `cipher` / `ciphers` / `tls_ciphers` |
| `[Auth]` `Key`, `Username`, `MaxClients`, `MaxSessionsPerUser`, `User` | `auth_key`, `auth_username`, `max_clients`, `max_sessions_per_user`, `users` (array) |
| `[Control]` `Listen`, `Port`, `Addr` | `control_listen`, `control_port`, `control_addr` |
| `[Multipath]` `Scheduler`, `CC` / `CongestionControl`, `Path`, `BackupPath` | `scheduler`, `cc` / `congestion_control`, `paths` (array), `backup_paths` (array) |
| `[Multipath]` `InitMaxPathId`, `Reinjection`, `ReinjectionSrttFactorPct`, `ReinjectionHardDeadlineMs`, `ReinjectionDeadlineLowerBoundMs` | `init_max_path_id`, `reinjection`, `reinjection_srtt_factor_pct`, `reinjection_hard_deadline_ms`, `reinjection_deadline_lower_bound_ms` |
| `[Multipath]` `Fec` / `FecEnable`, `FecScheme`, `SyncPathLabels`, `PushPathLabels` | `fec` / `fec_enable`, `fec_scheme`, `sync_path_labels`, `push_path_labels` |

`mode` (`"client"` or `"server"`) exists only in JSON. Without it, `listen` implies server mode; an explicit `mode` always wins. `path_policy` (server-side persisted per-path weights) and `reorder_rules` are JSON-only arrays.

## MTU Guidelines

### Default (auto)

For most setups, leave `MTU` unset. The auto-negotiated value (~1382) works on standard Ethernet (1500), PPPoE (1492), and mobile networks.

### When to set MTU explicitly

| Scenario | Recommendation |
|----------|----------------|
| Standard Ethernet / mobile | Leave unset (auto ~1382) |
| Deeply nested tunnels (mqvpn → WG → another tunnel) | Calculate remaining MTU; set if near 1280 |

On the client, if `MTU` is set, mqvpn uses `min(config MTU, negotiated MTU)`; a warning is logged when the config value exceeds the negotiated value. On the server, `MTU` sets the TUN MTU directly (default 1382).

::: tip
On the client, setting `MTU` above the negotiated value (~1382) has no effect — the negotiated value is always the upper bound. On the server, the configured value is applied to the TUN device as-is; packets exceeding a client's negotiated MSS are answered with ICMP Packet Too Big so that the sender's Path MTU Discovery can adjust.
:::

### How mqvpn determines TUN MTU

mqvpn negotiates the TUN MTU from the QUIC DATAGRAM Maximum Segment Size (MSS) at connection time. With the default `max_pkt_out_size` of 1400, the overhead breakdown is:

```
max_pkt_out_size           1400 bytes
 − QUIC short header         13 bytes
 − DATAGRAM frame header      3 bytes
 − MASQUE datagram header      2 bytes
                           ─────────
 = TUN MTU                  1382 bytes
```

This negotiation happens at connection time on the **client**, and the client TUN MTU follows it. The **server** sets its TUN MTU once at startup (1382 by default, or the configured value). When a packet exceeds a particular client's negotiated MSS, the server returns ICMP Packet Too Big to the original sender, carrying that client's MSS as the MTU value; the sender's Path MTU Discovery then lowers its packet size. The shared server TUN MTU never needs to shrink for individual clients.

### Running other tunnels inside mqvpn

When running a tunnel protocol (WireGuard, IPsec, GRE, etc.) inside the mqvpn tunnel, the inner tunnel's overhead reduces the effective MTU. Verify that the remaining MTU meets the inner protocol's requirements.

**Example: WireGuard inside mqvpn**

```
mqvpn TUN MTU                    1382 bytes
 − WireGuard overhead (IPv6)        80 bytes
                                 ─────────
 = WireGuard inner MTU            1302 bytes
   → IPv6 minimum (1280)            ✓
   → QUIC/HTTP3 UDP payload        1254 bytes > 1200  ✓
```

### Constraints

| Constraint | Value | Source |
|------------|-------|--------|
| Config minimum | 1280 | IPv6 minimum MTU (RFC 8200) |
| Config maximum | 9000 | Jumbo frame MTU |
| QUIC minimum UDP payload | 1200 | RFC 9000 §14 (handshake requirement) |
| Auto value | ~1382 (client: negotiated; server: fixed default) | Derived from `max_pkt_out_size` (1400) |

## Control API

A running server can be managed at runtime over a local TCP socket using JSON commands. This is useful for adding or removing users without restarting the server.

### Enable

```bash
sudo mqvpn --mode server ... --control-port 9090
```

The control API binds to `127.0.0.1` by default. It has no authentication, so only bind to trusted interfaces. A client can enable it the same way, for its own path commands and statistics.

### Enable from config file

The control API can also be enabled from the config file, in client or server mode:

```ini
[Control]
Listen = 127.0.0.1:9090
```

The address and port can also be given as separate keys:

```ini
[Control]
Port = 9090
Addr = 127.0.0.1
```

…or in JSON:

```json
{
  "control_listen": "127.0.0.1:9090"
}
```

(`"control_port"` / `"control_addr"` are the JSON form of the separate keys.)

`Port` / `Addr` override the matching half of `Listen`. CLI flags (`--control-port`, `--control-addr`) override the config-file values per field. `--control-port 0` explicitly disables the API even if the config sets a port.

### Request format

Send one JSON object per connection, for example with `echo '{"cmd":"list_users"}' | nc 127.0.0.1 9090`. The first non-space character must be `{`; anything else (an HTTP request, for example) is answered with `{"ok":false,"error":"invalid request"}` and the connection is closed. A request may be at most 4 KB, and the whole exchange must finish within 5 seconds. See [docs/control-api.md](https://github.com/mp0rta/mqvpn/blob/main/docs/control-api.md) for every command.

### Commands

Add a user:

```bash
echo '{"cmd":"add_user","name":"carol","key":"carol-secret"}' | nc 127.0.0.1 9090
```

Remove a user:

```bash
echo '{"cmd":"remove_user","name":"carol"}' | nc 127.0.0.1 9090
```

Removing a user also disconnects any active sessions authenticated with that username.

List users:

```bash
echo '{"cmd":"list_users"}' | nc 127.0.0.1 9090
```

Get stats:

```bash
echo '{"cmd":"get_stats"}' | nc 127.0.0.1 9090
```

Get detailed status (per-client, per-path):

```bash
echo '{"cmd":"get_status"}' | nc 127.0.0.1 9090
```

Or use the built-in status command for human-readable output:

```bash
mqvpn --status --control-port 9090
```

All commands return a JSON response with an `"ok"` field. Each connection handles one command, then the server closes the connection.

## systemd

If you installed via the deb package or install.sh, the systemd units are already in place. For source builds, install manually:

```bash
sudo cmake --install build --prefix /usr/local
```

### Server

If you used install.sh, `/etc/mqvpn/server.conf` is already generated. To configure manually, copy the example:

```bash
sudo cp /etc/mqvpn/server.conf.example /etc/mqvpn/server.conf
sudo vi /etc/mqvpn/server.conf   # edit cert paths, auth key, etc.
sudo systemctl enable --now mqvpn-server
```

### Client (template unit)

The client uses a template unit — the instance name maps to the config file:

```bash
sudo cp /etc/mqvpn/client.conf.example /etc/mqvpn/client-home.conf
sudo vi /etc/mqvpn/client-home.conf   # edit server address, auth key, etc.
sudo systemctl enable --now mqvpn-client@home
# → reads /etc/mqvpn/client-home.conf
```

::: info
The systemd units read fixed paths ending in `.conf` (`/etc/mqvpn/server.conf`, `/etc/mqvpn/client-<instance>.conf`). The content may be INI or JSON: mqvpn and the server unit's NAT helper both detect the format from the first non-space character, so a JSON config works when saved under that `.conf` name.
:::
