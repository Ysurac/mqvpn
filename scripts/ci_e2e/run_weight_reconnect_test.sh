#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# run_weight_reconnect_test.sh — E2E: path weights survive a reconnect
#
# The weight and DSCP mask set on a path through the control API are kept in
# the client's path slot and applied to xquic when the path is activated.
# The primary path (xquic path_id 0) is created with the connection and is
# never "activated", so after a reconnect the path that becomes path 0 used
# to fall back to weight 1 while the other paths got their weights back.
#
# Topology (two paths, one server):
#   vpn-client                    vpn-server
#     veth-wa0 ─────────────────── veth-wa1    Path A  10.110.0.0/24
#     10.110.0.2/24                10.110.0.1/24
#     veth-wb0 ─────────────────── veth-wb1    Path B  10.120.0.0/24
#     10.120.0.2/24                10.120.0.1/24
#
# Test sequence (scheduler wrr, which splits datagrams by path weight):
#   Phase 1  Both paths up, weights A=4 B=4 set through the control API.
#            A burst of 1000-byte pings through the tunnel: path A must carry
#            about half of the client's full-size tunnel packets.
#   Phase 2  Restart the server: the client reconnects by itself and comes
#            back on both paths (no weight is pushed again).
#   Phase 3  The new server must have received both paths' weights again
#            (PATH_LABEL capsules, logged by the server), and the same burst
#            must still split about evenly. Equal weights other than 1 make a
#            lost weight visible whichever path ends up as path 0: 4:4 turns
#            into 1:4 or 4:1.
#
# Usage: sudo ./scripts/ci_e2e/run_weight_reconnect_test.sh [path-to-mqvpn-binary]
# Requires: root, iproute2, iptables, openssl, netcat (nc), python3

set -euo pipefail

source "$(dirname "$0")/sanitizer_check.sh"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MQVPN="${1:-${SCRIPT_DIR}/../../build/mqvpn}"

if [ ! -f "$MQVPN" ]; then
    echo "error: mqvpn binary not found at $MQVPN"
    echo "Build first: mkdir build && cd build && cmake .. && make"
    exit 1
fi
MQVPN="$(realpath "$MQVPN")"

WORK_DIR="$(mktemp -d)"

NS_SERVER="vpn-server-wr"
NS_CLIENT="vpn-client-wr"
VETH_A0="veth-wa0"  VETH_A1="veth-wa1"
VETH_B0="veth-wb0"  VETH_B1="veth-wb1"

IP_A_CLIENT="10.110.0.2/24"  IP_A_SERVER="10.110.0.1/24"
IP_B_CLIENT="10.120.0.2/24"  IP_B_SERVER="10.120.0.1/24"
SERVER_ADDR="10.110.0.1"
TUNNEL_IP="10.0.0.1"

CTRL_PORT="9183"
WEIGHT_A=4
WEIGHT_B=4
# Expected share of path A: 1/2. WRR splits datagrams by weight, but a path that
# is momentarily cwnd- or pacing-blocked hands its turn to the other one, which
# is why the burst below is paced and preceded by a warm-up. The full-size
# tunnel packets are counted (ACK-only packets, which go by RTT, are excluded).
# A lost weight gives 0.2 or 0.8.
SHARE_MIN="0.38"
SHARE_MAX="0.62"

SERVER_PID=""
CLIENT_PID=""
SANITIZER_FAIL=0

# ── Cleanup ──────────────────────────────────────────────────────────────────

cleanup() {
    echo ""
    echo "Cleaning up..."
    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    sleep 1
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    ip link del "$VETH_A0"    2>/dev/null || true
    ip link del "$VETH_B0"    2>/dev/null || true
    rm -rf "$WORK_DIR"
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: sanitizer errors detected"
        exit 1
    fi
}
trap cleanup EXIT

# ── Helpers ──────────────────────────────────────────────────────────────────

ctrl_send() {
    local json="$1"
    printf '%s\n' "$json" \
        | ip netns exec "$NS_CLIENT" timeout 5 nc 127.0.0.1 "$CTRL_PORT" 2>/dev/null \
        || true
}

ctrl_ok() {
    local resp
    resp=$(ctrl_send "$1")
    if echo "$resp" | grep -q '"ok":true'; then return 0; fi
    echo "  control API error: $resp"
    return 1
}

dump_logs() {
    echo ""
    echo "--- client log (tail) ---"
    tail -60 "${WORK_DIR}/client.log" 2>/dev/null || true
    echo ""
    echo "--- server log (tail) ---"
    tail -30 "${WORK_DIR}/server.log" 2>/dev/null || true
}

# Full-size tunnel packets sent on a path: iptables counter on the client's
# OUTPUT chain for packets of 900 bytes or more leaving that interface.
setup_counters() {
    local dev
    for dev in "$VETH_A0" "$VETH_B0"; do
        ip netns exec "$NS_CLIENT" iptables -A OUTPUT -o "$dev" -p udp \
            -m length --length 900:65535 -j ACCEPT
    done
}

big_pkts() {
    ip netns exec "$NS_CLIENT" iptables -L OUTPUT -v -x -n \
        | awk -v dev="$1" '$7 == dev {print $1; exit}'
}

# Number of ACTIVE paths in the client's status, 0 when unavailable.
active_paths() {
    ctrl_send '{"cmd":"get_status"}' | python3 -c '
import json, sys
try:
    d = json.load(sys.stdin)
    print(sum(1 for p in d["clients"][0]["paths"] if p.get("state_label") == "active"))
except Exception:
    print(0)'
}

wait_active_paths() {
    local want="$1" timeout="$2" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if [ "$(active_paths)" -ge "$want" ]; then return 0; fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# Share of the client's full-size tunnel packets that left on path A during
# 600 pings of 1000 bytes through the tunnel, 50 per second (well below what
# either path can carry, so neither is cwnd- or pacing-blocked), after a short
# warm-up burst that gets both paths' congestion controllers out of startup.
measure_share() {
    local a0 b0 a1 b1
    ip netns exec "$NS_CLIENT" ping -q -c 200 -i 0.01 -s 1000 -W 1 "$TUNNEL_IP" >/dev/null 2>&1 || true
    a0=$(big_pkts "$VETH_A0"); b0=$(big_pkts "$VETH_B0")
    ip netns exec "$NS_CLIENT" ping -q -c 600 -i 0.02 -s 1000 -W 1 "$TUNNEL_IP" >/dev/null 2>&1 || true
    a1=$(big_pkts "$VETH_A0"); b1=$(big_pkts "$VETH_B0")
    python3 -c "a=$a1-$a0; b=$b1-$b0; print(f'{a/(a+b):.3f}' if a+b else 'nan')"
}

share_ok() {
    python3 -c "import sys; s=float('$1'); sys.exit(0 if $SHARE_MIN <= s <= $SHARE_MAX else 1)"
}

start_server() {
    local log_suffix="${1:-}"
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:4433" \
        --subnet 10.0.0.0/24 \
        --cert "${WORK_DIR}/server.crt" \
        --key  "${WORK_DIR}/server.key" \
        --auth-key "$PSK" \
        --scheduler wrr \
        --log-level debug > "${WORK_DIR}/server${log_suffix}.log" 2>&1 &
    SERVER_PID=$!
    sleep 2
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "FAIL: server process died"
        cat "${WORK_DIR}/server${log_suffix}.log"
        exit 1
    fi
    echo "Server running (PID $SERVER_PID)"
}

# ── Setup: network namespaces ────────────────────────────────────────────────

echo ""
echo "================================================================"
echo "  mqvpn E2E: path weights survive a reconnect"
echo "  Binary: $MQVPN"
echo "================================================================"

ip netns del "$NS_SERVER" 2>/dev/null || true
ip netns del "$NS_CLIENT" 2>/dev/null || true
ip link del "$VETH_A0" 2>/dev/null || true
ip link del "$VETH_B0" 2>/dev/null || true

PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=mqvpn-weight-reconnect-test" 2>/dev/null
echo "PSK and certificate generated."

echo "=== Setting up network namespaces ==="
ip netns add "$NS_SERVER"
ip netns add "$NS_CLIENT"

ip link add "$VETH_A0" type veth peer name "$VETH_A1"
ip link set "$VETH_A0" netns "$NS_CLIENT"
ip link set "$VETH_A1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_A_CLIENT" dev "$VETH_A0"
ip netns exec "$NS_SERVER" ip addr add "$IP_A_SERVER" dev "$VETH_A1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_A1" up

ip link add "$VETH_B0" type veth peer name "$VETH_B1"
ip link set "$VETH_B0" netns "$NS_CLIENT"
ip link set "$VETH_B1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_B_CLIENT" dev "$VETH_B0"
ip netns exec "$NS_SERVER" ip addr add "$IP_B_SERVER" dev "$VETH_B1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_B0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_B1" up

ip netns exec "$NS_CLIENT" ip link set lo up
ip netns exec "$NS_SERVER" ip link set lo up

# Server address also on loopback and reachable over path B, so that the
# reconnect can use either path.
ip netns exec "$NS_SERVER" ip addr add "${SERVER_ADDR}/32" dev lo
ip netns exec "$NS_CLIENT" ip route add 10.110.0.0/24 via 10.120.0.1 dev "$VETH_B0" metric 200

ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$SERVER_ADDR" >/dev/null
ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.120.0.1 >/dev/null
echo "OK: underlay connectivity verified"
setup_counters

echo "=== Starting VPN server ==="
start_server ""

# ── Phase 1: weights 3:2 ─────────────────────────────────────────────────────

echo ""
echo "=== Phase 1: client on paths A and B, weights A=${WEIGHT_A} B=${WEIGHT_B} ==="
ip netns exec "$NS_CLIENT" "$MQVPN" \
    --mode client \
    --server "${SERVER_ADDR}:4433" \
    --path "$VETH_A0" \
    --path "$VETH_B0" \
    --auth-key "$PSK" \
    --insecure \
    --scheduler wrr \
    --control-port "$CTRL_PORT" \
    --log-level debug > "${WORK_DIR}/client.log" 2>&1 &
CLIENT_PID=$!
sleep 2
if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "FAIL: client process died"
    cat "${WORK_DIR}/client.log"
    exit 1
fi

ELAPSED=0
while [ "$ELAPSED" -lt 20 ]; do
    if ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$TUNNEL_IP" >/dev/null 2>&1; then break; fi
    sleep 1; ELAPSED=$((ELAPSED + 1))
done
if [ "$ELAPSED" -ge 20 ]; then
    echo "FAIL: tunnel not reachable after 20s"
    dump_logs; exit 1
fi
if ! wait_active_paths 2 20; then
    echo "FAIL: two paths not active after 20s"
    dump_logs; exit 1
fi
echo "OK: tunnel up on two paths"

ctrl_ok "{\"cmd\":\"set_path_weight\",\"iface\":\"$VETH_A0\",\"weight\":$WEIGHT_A}" || { dump_logs; exit 1; }
ctrl_ok "{\"cmd\":\"set_path_weight\",\"iface\":\"$VETH_B0\",\"weight\":$WEIGHT_B}" || { dump_logs; exit 1; }
sleep 1

SHARE1=$(measure_share)
echo "Path A share of full-size packets: $SHARE1 (expected ${SHARE_MIN}..${SHARE_MAX})"
if ! share_ok "$SHARE1"; then
    echo "FAIL: Phase 1 split does not follow the weights"
    dump_logs; exit 1
fi
echo "=== Phase 1: PASS ==="

# ── Phase 2: server restart → client reconnect ───────────────────────────────

echo ""
echo "=== Phase 2: restart the server, wait for the client to reconnect ==="
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
sleep 3
start_server "-2"

ELAPSED=0
while [ "$ELAPSED" -lt 60 ]; do
    if [ "$(grep -c 'ADDRESS_ASSIGN' "${WORK_DIR}/client.log")" -ge 2 ] \
        && ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$TUNNEL_IP" >/dev/null 2>&1; then
        break
    fi
    sleep 1; ELAPSED=$((ELAPSED + 1))
done
if [ "$ELAPSED" -ge 60 ]; then
    echo "FAIL: client did not reconnect within 60s"
    dump_logs; exit 1
fi
if ! wait_active_paths 2 30; then
    echo "FAIL: two paths not active again after the reconnect"
    dump_logs; exit 1
fi
sleep 2
echo "OK: reconnected on two paths (${ELAPSED}s)"
grep -E 'reconnect: using|activated: path_id' "${WORK_DIR}/client.log" | tail -3 || true

# ── Phase 3: same weights, no new set_path_weight ───────────────────────────

echo ""
echo "=== Phase 3: weights after the reconnect ==="
# The client re-announces each path's weight to the server (PATH_LABEL); the
# path that became xquic path 0 used to be left out, along with its weight.
for dev in "$VETH_A0" "$VETH_B0"; do
    if ! grep -qE "path_label: user=[^ ]+ iface=${dev} path_id=[0-9]+ client_weight=${WEIGHT_A} " \
            "${WORK_DIR}/server-2.log"; then
        echo "FAIL: after the reconnect the server got no weight ${WEIGHT_A} for ${dev}"
        grep -E "path_label: user=" "${WORK_DIR}/server-2.log" | tail -5 || true
        dump_logs; exit 1
    fi
done
echo "OK: both paths re-announced with weight ${WEIGHT_A}"
SHARE2=$(measure_share)
echo "Path A share of full-size packets: $SHARE2 (expected ${SHARE_MIN}..${SHARE_MAX})"
if ! share_ok "$SHARE2"; then
    echo "FAIL: after the reconnect the split no longer follows the weights"
    echo "      (the path that became xquic path 0 lost its weight)"
    dump_logs; exit 1
fi
echo "=== Phase 3: PASS ==="

echo ""
echo "================================================================"
echo "  All tests PASSED"
echo "  Phase 1: weights ${WEIGHT_A}:${WEIGHT_B} applied (share $SHARE1)"
echo "  Phase 3: still applied after a reconnect (share $SHARE2)"
echo "================================================================"
