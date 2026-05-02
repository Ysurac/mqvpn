#!/bin/bash
# run_path_api_test.sh — E2E regression test for issue #4271
#
# Verifies that a second path added at runtime via the control-socket API:
#   1. Becomes ACTIVE (not stuck PENDING) — Bug 1 fix in client_activate_path()
#   2. Keeps the tunnel alive after the initial path is removed — Bug 2 fix
#      in tick_reconnect() which now rotates away from the dead path.
#
# Topology (dual-path, single-server):
#   vpn-client                    vpn-server
#     veth-a0 ──────────────────── veth-a1    Path A  10.100.0.0/24
#     10.100.0.2/24                10.100.0.1/24
#     veth-b0 ──────────────────── veth-b1    Path B  10.200.0.0/24
#     10.200.0.2/24                10.200.0.1/24
#
# Test sequence:
#   Phase 1  Client starts with path A only — single-path tunnel established.
#   Phase 2  Path B added via {"cmd":"add_path","iface":"veth-b0"} control API.
#            Wait for library to activate it (verifies Bug 1 fix).
#   Phase 3  Path A removed via {"cmd":"remove_path","iface":"veth-a0"}.
#            Verify tunnel still works on path B alone (verifies Bug 2 fix).
#
# Usage: sudo ./scripts/ci_e2e/run_path_api_test.sh [path-to-mqvpn-binary]
# Requires: root, iproute2, openssl, netcat (nc)

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

NS_SERVER="vpn-server-pa"
NS_CLIENT="vpn-client-pa"
VETH_A0="veth-a0"  VETH_A1="veth-a1"
VETH_B0="veth-b0"  VETH_B1="veth-b1"

IP_A_CLIENT="10.100.0.2/24"  IP_A_SERVER="10.100.0.1/24"
IP_B_CLIENT="10.200.0.2/24"  IP_B_SERVER="10.200.0.1/24"
SERVER_ADDR="10.100.0.1"
TUNNEL_IP="10.0.0.1"

CTRL_PORT="9181"   # control socket port for this test (avoid collisions)

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

wait_for_log() {
    local file="$1" pattern="$2" timeout="${3:-20}" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if grep -qiE "$pattern" "$file" 2>/dev/null; then return 0; fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

# Send a JSON command to the client control socket; print the response.
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
    echo "--- client log ---"
    cat "${WORK_DIR}/client.log" 2>/dev/null || true
    echo ""
    echo "--- server log ---"
    cat "${WORK_DIR}/server.log" 2>/dev/null || true
}

# ── Setup: network namespaces ────────────────────────────────────────────────

echo "================================================================"
echo "  mqvpn E2E: runtime path add/remove via control API (#4271)"
echo "  Binary: $MQVPN"
echo "================================================================"

# Remove any leftovers from previous runs
ip netns del "$NS_SERVER" 2>/dev/null || true
ip netns del "$NS_CLIENT" 2>/dev/null || true
ip link del "$VETH_A0" 2>/dev/null || true
ip link del "$VETH_B0" 2>/dev/null || true

# Generate PSK and TLS certificate
PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=mqvpn-path-api-test" 2>/dev/null
echo "PSK and certificate generated."

echo "=== Setting up network namespaces ==="
ip netns add "$NS_SERVER"
ip netns add "$NS_CLIENT"

# Path A: 10.100.0.0/24
ip link add "$VETH_A0" type veth peer name "$VETH_A1"
ip link set "$VETH_A0" netns "$NS_CLIENT"
ip link set "$VETH_A1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_A_CLIENT" dev "$VETH_A0"
ip netns exec "$NS_SERVER" ip addr add "$IP_A_SERVER" dev "$VETH_A1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_A1" up

# Path B: 10.200.0.0/24
ip link add "$VETH_B0" type veth peer name "$VETH_B1"
ip link set "$VETH_B0" netns "$NS_CLIENT"
ip link set "$VETH_B1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_B_CLIENT" dev "$VETH_B0"
ip netns exec "$NS_SERVER" ip addr add "$IP_B_SERVER" dev "$VETH_B1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_B0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_B1" up

# Loopback
ip netns exec "$NS_CLIENT" ip link set lo up
ip netns exec "$NS_SERVER" ip link set lo up
ip netns exec "$NS_SERVER" sysctl -w net.ipv4.ip_forward=1 >/dev/null

# Server address also on loopback so it stays reachable from path B
# after path A is removed.
ip netns exec "$NS_SERVER" ip addr add "${SERVER_ADDR}/32" dev lo

# Path B is a valid route to the server (metric 200 = lower priority than A)
ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.200.0.1 dev "$VETH_B0" metric 200

ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$SERVER_ADDR" >/dev/null
ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.200.0.1 >/dev/null
echo "OK: underlay connectivity verified"

# ── Start VPN server ─────────────────────────────────────────────────────────

echo "=== Starting VPN server ==="
ip netns exec "$NS_SERVER" "$MQVPN" \
    --mode server \
    --listen "0.0.0.0:4433" \
    --subnet 10.0.0.0/24 \
    --cert "${WORK_DIR}/server.crt" \
    --key  "${WORK_DIR}/server.key" \
    --auth-key "$PSK" \
    --log-level debug > "${WORK_DIR}/server.log" 2>&1 &
SERVER_PID=$!
sleep 2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FAIL: server process died"
    cat "${WORK_DIR}/server.log"
    exit 1
fi
echo "Server running (PID $SERVER_PID)"

# ── Phase 1: single-path tunnel (path A only) ────────────────────────────────

echo ""
echo "=== Phase 1: start client with path A only ==="
ip netns exec "$NS_CLIENT" "$MQVPN" \
    --mode client \
    --server "${SERVER_ADDR}:4433" \
    --path "$VETH_A0" \
    --auth-key "$PSK" \
    --insecure \
    --control-port "$CTRL_PORT" \
    --log-level debug > "${WORK_DIR}/client.log" 2>&1 &
CLIENT_PID=$!
sleep 2
if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "FAIL: client process died"
    cat "${WORK_DIR}/client.log"
    exit 1
fi
echo "Client running (PID $CLIENT_PID)"

echo "Waiting for single-path tunnel..."
ELAPSED=0
while [ "$ELAPSED" -lt 20 ]; do
    if ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$TUNNEL_IP" >/dev/null 2>&1; then break; fi
    sleep 1; ELAPSED=$((ELAPSED + 1))
done
if [ "$ELAPSED" -ge 20 ]; then
    echo "FAIL: tunnel not reachable after 20s"
    dump_logs; exit 1
fi
echo "OK: single-path tunnel up on path A (${ELAPSED}s)"

# Wait for control socket to be ready
ELAPSED=0
while [ "$ELAPSED" -lt 10 ]; do
    if ip netns exec "$NS_CLIENT" nc -z 127.0.0.1 "$CTRL_PORT" 2>/dev/null; then break; fi
    sleep 1; ELAPSED=$((ELAPSED + 1))
done
if [ "$ELAPSED" -ge 10 ]; then
    echo "FAIL: control socket not ready after 10s"
    dump_logs; exit 1
fi
echo "OK: control socket ready"

# ── Phase 2: add path B via API ──────────────────────────────────────────────

echo ""
echo "=== Phase 2: add path B via control API ==="
if ! ctrl_ok '{"cmd":"add_path","iface":"veth-b0"}'; then
    echo "FAIL: add_path command rejected"
    dump_logs; exit 1
fi
echo "add_path veth-b0: accepted"

# Bug 1 assertion: path B must become ACTIVE, not stay PENDING.
# The library now transitions failed activations to DEGRADED+retry instead of
# silently leaving the path in PENDING.  In a normal test environment
# xqc_conn_create_path() should succeed and the path should activate directly.
echo "Waiting for path B activation (Bug 1 regression check)..."
if ! wait_for_log "${WORK_DIR}/client.log" "path.*activated.*veth-b0|activated.*path_id.*veth-b0" 20; then
    # Tolerate the DEGRADED→retry path: path may still activate within the
    # 5 s backoff window.  A second wait catches that case.
    if ! wait_for_log "${WORK_DIR}/client.log" "path added.*veth-b0|veth-b0.*active" 10; then
        echo "FAIL: path B never activated within 30s (Bug 1 still present?)"
        dump_logs; exit 1
    fi
fi
echo "OK: path B is active (not stuck PENDING)"

if ip netns exec "$NS_CLIENT" ping -c 3 -W 2 "$TUNNEL_IP" >/dev/null 2>&1; then
    echo "OK: tunnel ping works with both paths"
else
    echo "FAIL: tunnel ping failed after adding path B"
    dump_logs; exit 1
fi

# Verify list_paths shows both interfaces
LIST_RESP=$(ctrl_send '{"cmd":"list_paths"}')
if ! echo "$LIST_RESP" | grep -q "veth-b0"; then
    echo "FAIL: list_paths does not include veth-b0: $LIST_RESP"
    dump_logs; exit 1
fi
echo "OK: list_paths shows veth-b0"

# ── Phase 3: remove path A via API ───────────────────────────────────────────

echo ""
echo "=== Phase 3: remove path A via control API ==="
if ! ctrl_ok '{"cmd":"remove_path","iface":"veth-a0"}'; then
    echo "FAIL: remove_path command rejected"
    dump_logs; exit 1
fi
echo "remove_path veth-a0: accepted"

if ! wait_for_log "${WORK_DIR}/client.log" "path removed.*veth-a0" 10; then
    echo "FAIL: path A removal not confirmed in log within 10s"
    dump_logs; exit 1
fi
echo "OK: path A removed"

# Allow xquic a moment to reroute packets to path B
sleep 2

# Bug 2 assertion: tunnel must survive on path B alone.
echo "Verifying tunnel on path B alone (Bug 2 regression check)..."
if ip netns exec "$NS_CLIENT" ping -c 5 -W 2 "$TUNNEL_IP" >/dev/null 2>&1; then
    echo "OK: tunnel ping works on path B alone"
else
    echo "FAIL: tunnel ping failed after removing path A (Bug 2 still present?)"
    dump_logs; exit 1
fi

# Verify list_paths shows only path B
LIST_RESP=$(ctrl_send '{"cmd":"list_paths"}')
if echo "$LIST_RESP" | grep -q "veth-a0"; then
    echo "FAIL: list_paths still shows veth-a0 after removal: $LIST_RESP"
    dump_logs; exit 1
fi
if ! echo "$LIST_RESP" | grep -q "veth-b0"; then
    echo "FAIL: list_paths does not show veth-b0: $LIST_RESP"
    dump_logs; exit 1
fi
echo "OK: list_paths shows only veth-b0"

echo ""
echo "================================================================"
echo "  All tests PASSED"
echo "  Phase 1: single-path tunnel established on path A"
echo "  Phase 2: path B activated via API (Bug 1 fix verified)"
echo "  Phase 3: tunnel survives on path B after removing path A (Bug 2)"
echo "================================================================"
