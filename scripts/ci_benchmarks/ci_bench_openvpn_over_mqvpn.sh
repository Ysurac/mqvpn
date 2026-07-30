#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# ci_bench_openvpn_over_mqvpn.sh — Nested VPN feasibility check (OpenVPN over mqvpn)
#
# Brings up a single-path mqvpn tunnel, then runs an independent OpenVPN
# point-to-point tunnel (static-key mode) with its endpoints reachable only
# through the mqvpn tunnel — i.e. OpenVPN's own UDP traffic is itself
# encapsulated inside QUIC/MASQUE. This checks that double encapsulation
# (arbitrary UDP payload over the mqvpn TUN device) works end-to-end and
# is not blocked or mangled by MTU/fragmentation along the way.
#
# OpenVPN's own tun-mtu is set well below the mqvpn TUN MTU so its
# encapsulated packets fit without relying on IP fragmentation across the
# nested tunnels.
#
# Output: ci_bench_results/openvpn_over_mqvpn_<timestamp>.json
# Usage:  sudo ./ci_bench_openvpn_over_mqvpn.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"

MQVPN_MTU=1400
OVPN_PORT=1195
OVPN_TUN_MTU=1300
OVPN_MSSFIX=1250
OVPN_SERVER_IP="10.8.0.1"
OVPN_CLIENT_IP="10.8.0.2"
IPERF_DURATION=8

_OVPN_WORK_DIR=""
_OVPN_SERVER_PID=""
_OVPN_CLIENT_PID=""

# ── Cleanup ──

# Both OpenVPN and mqvpn install their own SIGTERM handler for graceful
# shutdown, and under this nested-tunnel workload that handler can hang
# indefinitely (e.g. waiting on a QUIC/OpenVPN close handshake with a peer
# that's already gone) rather than ever actually exiting. Killing the PIDs
# one at a time with a per-process grace period stacks up badly — 4
# processes x a few seconds of grace each can add up to well over any
# reasonable cleanup budget. Instead: SIGTERM everything at once, poll
# them together for a short shared window, then SIGKILL any survivors.
_kill_all() {
    local pid alive
    for pid in "$@"; do
        [ -n "$pid" ] && kill "$pid" 2>/dev/null || true
    done
    for _ in 1 2 3; do
        alive=0
        for pid in "$@"; do
            [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null && alive=1
        done
        [ "$alive" -eq 0 ] && return 0
        sleep 1
    done
    for pid in "$@"; do
        [ -n "$pid" ] && kill -9 "$pid" 2>/dev/null || true
    done
}

_ovpn_cleanup() {
    # Preserve the script's real exit status — otherwise the last command
    # run inside this trap silently becomes the process's exit code.
    local exit_code=$?

    echo ""
    echo "Cleaning up..."

    _kill_all "$_OVPN_CLIENT_PID" "$_OVPN_SERVER_PID" "$_CB_CLIENT_PID" "$_CB_SERVER_PID"
    _CB_CLIENT_PID=""
    _CB_SERVER_PID=""
    ip netns exec "$NS_CLIENT" pkill -9 -f "openvpn|mqvpn" 2>/dev/null || true
    ip netns exec "$NS_SERVER" pkill -9 -f "openvpn|mqvpn" 2>/dev/null || true

    [ -n "$_OVPN_WORK_DIR" ] && rm -rf "$_OVPN_WORK_DIR" || true

    ci_bench_cleanup

    # By now every process that could hold the namespaces open is dead, so
    # `ip netns del` (already attempted once inside ci_bench_cleanup) should
    # succeed; retry briefly for any lingering kernel-side socket teardown,
    # then fall back to a lazy unmount, which detaches the name immediately
    # (unblocking any run that reuses it) while the kernel finishes freeing
    # the namespace asynchronously once the last references actually drop.
    local ns
    for _ in 1 2 3; do
        ip netns list 2>/dev/null | grep -qE "^(${NS_SERVER}|${NS_CLIENT})( |$)" || break
        sleep 1
        ip netns del "$NS_SERVER" 2>/dev/null || true
        ip netns del "$NS_CLIENT" 2>/dev/null || true
    done
    for ns in "$NS_SERVER" "$NS_CLIENT"; do
        [ -e "/var/run/netns/$ns" ] || continue
        umount -l "/var/run/netns/$ns" 2>/dev/null || true
        rm -f "/var/run/netns/$ns" 2>/dev/null || true
    done

    exit "$exit_code"
}

trap _ovpn_cleanup EXIT

# ── Preflight ──

ci_bench_check_deps

if ! command -v openvpn &>/dev/null; then
    echo "error: openvpn not found (required for this test)"
    exit 1
fi
OVPN_VERSION="$(openvpn --version 2>&1 | head -1)"

echo "================================================================"
echo "  OpenVPN-over-mqvpn Nested VPN Feasibility Check (CI)"
echo "  mqvpn binary: $MQVPN"
echo "  OpenVPN:      $OVPN_VERSION"
echo "  mqvpn MTU:    $MQVPN_MTU"
echo "  OpenVPN MTU:  $OVPN_TUN_MTU (mssfix $OVPN_MSSFIX)"
echo "  Commit:       $CI_BENCH_COMMIT"
echo "  Date:         $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

ci_bench_setup_netns

# ── Bring up mqvpn tunnel (single path) ──

echo ""
echo "==> Starting mqvpn tunnel"

_CB_WORK_DIR="$(mktemp -d)"
_CB_PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${_CB_WORK_DIR}/server.key" -out "${_CB_WORK_DIR}/server.crt" \
    -days 365 -nodes -subj "/CN=ci-bench-ovpn" 2>/dev/null

ip netns exec "$NS_SERVER" "$MQVPN" \
    --mode server \
    --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
    --subnet 10.0.0.0/24 \
    --cert "${_CB_WORK_DIR}/server.crt" \
    --key  "${_CB_WORK_DIR}/server.key" \
    --auth-key "$_CB_PSK" \
    --scheduler wlb \
    --cc bbr2 \
    --mtu "$MQVPN_MTU" \
    --log-level "$CI_BENCH_LOG_LEVEL" &
_CB_SERVER_PID=$!
sleep 2

if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
    echo "ERROR: mqvpn server died"
    exit 1
fi

ip netns exec "$NS_CLIENT" "$MQVPN" \
    --mode client \
    --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
    --path "$VETH_A0" \
    --auth-key "$_CB_PSK" \
    --scheduler wlb \
    --cc bbr2 \
    --mtu "$MQVPN_MTU" \
    --insecure \
    --log-level "$CI_BENCH_LOG_LEVEL" &
_CB_CLIENT_PID=$!
sleep 3

MQVPN_UP=0
if kill -0 "$_CB_CLIENT_PID" 2>/dev/null && ci_bench_wait_tunnel 15; then
    MQVPN_UP=1
    echo "OK: mqvpn tunnel up (server $TUNNEL_SERVER_IP)"
else
    echo "ERROR: mqvpn tunnel did not come up"
fi

# ── Bring up OpenVPN over the mqvpn tunnel ──

OVPN_UP=0
OVPN_PING_RTT_MS=""
OVPN_IPERF_MBPS="0.0"

if [ "$MQVPN_UP" -eq 1 ]; then
    echo ""
    echo "==> Starting OpenVPN static-key tunnel routed over mqvpn"

    _OVPN_WORK_DIR="$(mktemp -d)"
    openvpn --genkey secret "${_OVPN_WORK_DIR}/static.key" >/dev/null 2>&1

    # "Server" side: listens for the OpenVPN peer. Reachable at the mqvpn
    # server's tunnel address ($TUNNEL_SERVER_IP), so client-side OpenVPN
    # packets travel client-tun -> mqvpn(QUIC) -> server-tun -> OpenVPN.
    ip netns exec "$NS_SERVER" openvpn \
        --dev tun --proto udp4 --port "$OVPN_PORT" \
        --secret "${_OVPN_WORK_DIR}/static.key" \
        --ifconfig "$OVPN_SERVER_IP" "$OVPN_CLIENT_IP" \
        --tun-mtu "$OVPN_TUN_MTU" --mssfix "$OVPN_MSSFIX" \
        --cipher AES-256-CBC --auth SHA256 \
        --verb 3 --log "${_OVPN_WORK_DIR}/server.log" &
    _OVPN_SERVER_PID=$!
    sleep 2

    if ! kill -0 "$_OVPN_SERVER_PID" 2>/dev/null; then
        echo "ERROR: OpenVPN server side died"
    else
        ip netns exec "$NS_CLIENT" openvpn \
            --dev tun --proto udp4 --port "$OVPN_PORT" \
            --remote "$TUNNEL_SERVER_IP" \
            --secret "${_OVPN_WORK_DIR}/static.key" \
            --ifconfig "$OVPN_CLIENT_IP" "$OVPN_SERVER_IP" \
            --tun-mtu "$OVPN_TUN_MTU" --mssfix "$OVPN_MSSFIX" \
            --cipher AES-256-CBC --auth SHA256 \
            --verb 3 --log "${_OVPN_WORK_DIR}/client.log" &
        _OVPN_CLIENT_PID=$!
        sleep 3

        if ! kill -0 "$_OVPN_CLIENT_PID" 2>/dev/null; then
            echo "ERROR: OpenVPN client side died"
        else
            for _ in $(seq 1 15); do
                if ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$OVPN_SERVER_IP" >/dev/null 2>&1; then
                    OVPN_UP=1
                    break
                fi
                sleep 1
            done
        fi
    fi

    if [ "$OVPN_UP" -eq 1 ]; then
        echo "OK: OpenVPN tunnel up over mqvpn ($OVPN_CLIENT_IP <-> $OVPN_SERVER_IP)"

        PING_OUT="$(ip netns exec "$NS_CLIENT" ping -c 5 -W 1 "$OVPN_SERVER_IP" 2>/dev/null || true)"
        OVPN_PING_RTT_MS="$(echo "$PING_OUT" | grep -oP 'rtt min/avg/max/mdev = [0-9.]+/\K[0-9.]+' || echo "")"

        echo ""
        echo "==> Measuring throughput through the nested tunnel"
        ip netns exec "$NS_SERVER" iperf3 -s -B "$OVPN_SERVER_IP" -1 &>/dev/null &
        _iperf_srv=$!
        sleep 1
        IPERF_JSON="$(mktemp)"
        ip netns exec "$NS_CLIENT" iperf3 -c "$OVPN_SERVER_IP" -t "$IPERF_DURATION" -R --json \
            > "$IPERF_JSON" 2>&1 || true
        wait "$_iperf_srv" 2>/dev/null || true
        OVPN_IPERF_MBPS="$(ci_bench_parse_throughput "$IPERF_JSON")"
        rm -f "$IPERF_JSON"
        echo "    throughput over OpenVPN-over-mqvpn: ${OVPN_IPERF_MBPS} Mbps"
    else
        echo "ERROR: OpenVPN tunnel did not come up over mqvpn"
        echo "--- server log ---"; tail -n 30 "${_OVPN_WORK_DIR}/server.log" 2>/dev/null || true
        echo "--- client log ---"; tail -n 30 "${_OVPN_WORK_DIR}/client.log" 2>/dev/null || true
    fi
fi

# ── Generate output JSON ──

TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${CI_BENCH_RESULTS}/openvpn_over_mqvpn_$(date +%Y%m%d_%H%M%S).json"

python3 <<PYEOF
import json

_rtt_raw = "${OVPN_PING_RTT_MS}"
rtt_ms = float(_rtt_raw) if _rtt_raw else None

output = {
    'test': 'openvpn_over_mqvpn',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'openvpn_version': '${OVPN_VERSION}',
    'mqvpn': {'mtu': ${MQVPN_MTU}, 'scheduler': 'wlb', 'cc': 'bbr2'},
    'openvpn': {
        'mode': 'static-key',
        'proto': 'udp',
        'tun_mtu': ${OVPN_TUN_MTU},
        'mssfix': ${OVPN_MSSFIX},
    },
    'mqvpn_tunnel_established': bool(${MQVPN_UP}),
    'openvpn_tunnel_established': bool(${OVPN_UP}),
    'openvpn_ping_avg_rtt_ms': rtt_ms,
    'iperf3_over_openvpn_mbps': float('${OVPN_IPERF_MBPS}'),
    'possible': bool(${MQVPN_UP}) and bool(${OVPN_UP}),
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(output, f, indent=2)

print()
print(json.dumps(output, indent=2))
PYEOF

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"

if [ "$MQVPN_UP" -eq 1 ] && [ "$OVPN_UP" -eq 1 ]; then
    echo "PASS: OpenVPN over mqvpn is possible"
else
    echo "FAIL: OpenVPN over mqvpn did not come up (mqvpn_up=$MQVPN_UP openvpn_up=$OVPN_UP)"
    exit 1
fi
