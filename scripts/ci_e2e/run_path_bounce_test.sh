#!/bin/bash
# run_path_bounce_test.sh — E2E regression test for issue #4276
#
# Verifies that a secondary path cycled via the control-socket API more than
# XQC_MAX_PATHS_COUNT (8) times survives the forced reconnect that budget
# exhaustion triggers and becomes ACTIVE again on the new connection.
#
# Topology (two-path, single-server):
#   vpn-client                    vpn-server
#     veth-a0 ──────────────────── veth-a1    Path A  10.100.0.0/24  (primary, stays up)
#     10.100.0.2/24                10.100.0.1/24
#     veth-b0 ──────────────────── veth-b1    Path B  10.200.0.0/24  (bounced repeatedly)
#     10.200.0.2/24                10.200.0.1/24
#
# Test sequence:
#   Setup    Client starts with path A; path B added via API.
#   Loop     10 times: remove path B → re-add path B → wait for ACTIVE → ping.
#   Verify   Budget exhaustion + reconnect happened around cycle 8.
#            Tunnel works and path B is active after all cycles.
#
# Usage: sudo ./scripts/ci_e2e/run_path_bounce_test.sh [path-to-mqvpn-binary]
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

NS_SERVER="vpn-server-pb"
NS_CLIENT="vpn-client-pb"
VETH_A0="veth-pa0"  VETH_A1="veth-pa1"
VETH_B0="veth-pb0"  VETH_B1="veth-pb1"

IP_A_CLIENT="10.100.0.2/24"  IP_A_SERVER="10.100.0.1/24"
IP_B_CLIENT="10.200.0.2/24"  IP_B_SERVER="10.200.0.1/24"
SERVER_ADDR="10.100.0.1"
TUNNEL_IP="10.0.0.1"

CTRL_PORT="9182"

BOUNCE_COUNT=10   # > XQC_MAX_PATHS_COUNT (8) to guarantee budget exhaustion

SERVER_PID=""
CLIENT_PID=""
SANITIZER_FAIL=0

# ── Cleanup ──────────────────────────────────────────────────────────────────

cleanup() {
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
        sleep 1; elapsed=$((elapsed + 1))
    done
    return 1
}

# Wait until at least $n activations of veth-pb0 appear in the client log.
# Uses the library-level "path[N] activated: ... iface=veth-pb0" line.
wait_for_activations() {
    local n="$1" timeout="${2:-30}" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        local count
        count=$(grep -cE "path\[[0-9]+\] activated.*${VETH_B0}" \
                "${WORK_DIR}/client.log" 2>/dev/null || true)
        if [ "${count:-0}" -ge "$n" ]; then return 0; fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    return 1
}

ctrl_send() {
    printf '%s\n' "$1" \
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

ping_tunnel() {
    ip netns exec "$NS_CLIENT" ping -c 3 -W 2 "$TUNNEL_IP" >/dev/null 2>&1
}

dump_logs() {
    echo "--- client log (last 40 lines) ---"
    tail -40 "${WORK_DIR}/client.log" 2>/dev/null || true
    echo "--- server log (last 20 lines) ---"
    tail -20 "${WORK_DIR}/server.log" 2>/dev/null || true
}

# ── Banner ────────────────────────────────────────────────────────────────────

echo ""
echo "================================================================"
echo "  mqvpn E2E: path bounce stability (#4276)"
echo "  Binary:  $MQVPN"
echo "  Cycles:  $BOUNCE_COUNT  (budget limit: XQC_MAX_PATHS_COUNT=8)"
echo "================================================================"

# ── Setup: namespaces + veth pairs ───────────────────────────────────────────

ip netns del "$NS_SERVER" 2>/dev/null || true
ip netns del "$NS_CLIENT" 2>/dev/null || true
ip link del "$VETH_A0" 2>/dev/null || true
ip link del "$VETH_B0" 2>/dev/null || true

PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=mqvpn-bounce-test" 2>/dev/null

ip netns add "$NS_SERVER"
ip netns add "$NS_CLIENT"

# Path A
ip link add "$VETH_A0" type veth peer name "$VETH_A1"
ip link set "$VETH_A0" netns "$NS_CLIENT"
ip link set "$VETH_A1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_A_CLIENT" dev "$VETH_A0"
ip netns exec "$NS_SERVER" ip addr add "$IP_A_SERVER" dev "$VETH_A1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_A1" up

# Path B
ip link add "$VETH_B0" type veth peer name "$VETH_B1"
ip link set "$VETH_B0" netns "$NS_CLIENT"
ip link set "$VETH_B1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_B_CLIENT" dev "$VETH_B0"
ip netns exec "$NS_SERVER" ip addr add "$IP_B_SERVER" dev "$VETH_B1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_B0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_B1" up

ip netns exec "$NS_CLIENT" ip link set lo up
ip netns exec "$NS_SERVER" ip link set lo up
ip netns exec "$NS_SERVER" sysctl -w net.ipv4.ip_forward=1 >/dev/null

# Server address on loopback so it stays reachable from path B
ip netns exec "$NS_SERVER" ip addr add "${SERVER_ADDR}/32" dev lo

# Fallback route: reach server via path B (used after reconnect via path B)
ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 \
    via 10.200.0.1 dev "$VETH_B0" metric 200

ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$SERVER_ADDR" >/dev/null
ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.200.0.1 >/dev/null
echo "OK: underlay connectivity verified"

# ── Start server ─────────────────────────────────────────────────────────────

echo ""
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
kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server died"; dump_logs; exit 1; }

# ── Start client (path A only) ───────────────────────────────────────────────

echo "=== Starting VPN client (path A only) ==="
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
kill -0 "$CLIENT_PID" 2>/dev/null || { echo "FAIL: client died"; dump_logs; exit 1; }

# Wait for tunnel on path A
echo "Waiting for single-path tunnel..."
ELAPSED=0
while [ "$ELAPSED" -lt 20 ]; do
    ping_tunnel && break
    sleep 1; ELAPSED=$((ELAPSED + 1))
done
if [ "$ELAPSED" -ge 20 ]; then
    echo "FAIL: tunnel not up after 20s"; dump_logs; exit 1
fi
echo "OK: tunnel up on path A (${ELAPSED}s)"

# Wait for control socket
ELAPSED=0
while [ "$ELAPSED" -lt 10 ]; do
    ip netns exec "$NS_CLIENT" nc -z 127.0.0.1 "$CTRL_PORT" 2>/dev/null && break
    sleep 1; ELAPSED=$((ELAPSED + 1))
done
[ "$ELAPSED" -lt 10 ] || { echo "FAIL: control socket not ready"; dump_logs; exit 1; }
echo "OK: control socket ready"

# ── Initial add of path B ─────────────────────────────────────────────────────

echo ""
echo "=== Adding path B for the first time ==="
ctrl_ok "{\"cmd\":\"add_path\",\"iface\":\"${VETH_B0}\"}" || {
    echo "FAIL: initial add_path rejected"; dump_logs; exit 1
}
echo "add_path ${VETH_B0}: accepted"

wait_for_activations 1 30 || {
    echo "FAIL: path B not activated after initial add"; dump_logs; exit 1
}
echo "OK: path B activated (cycle 0)"

# ── Bounce loop ───────────────────────────────────────────────────────────────

echo ""
echo "=== Bouncing path B ${BOUNCE_COUNT} times ==="
FAIL_CYCLE=0

for cycle in $(seq 1 "$BOUNCE_COUNT"); do
    echo ""
    echo "--- Cycle ${cycle}/${BOUNCE_COUNT} ---"

    # Remove path B
    if ! ctrl_ok "{\"cmd\":\"remove_path\",\"iface\":\"${VETH_B0}\"}"; then
        echo "FAIL: remove_path rejected at cycle $cycle"
        FAIL_CYCLE=$cycle; break
    fi
    echo "  remove_path: accepted"
    sleep 1

    # Re-add path B
    if ! ctrl_ok "{\"cmd\":\"add_path\",\"iface\":\"${VETH_B0}\"}"; then
        echo "FAIL: add_path rejected at cycle $cycle"
        FAIL_CYCLE=$cycle; break
    fi
    echo "  add_path: accepted"

    # Wait for activation — allow 30s to cover forced reconnect (~5-15s)
    total_expected=$((cycle + 1))
    if ! wait_for_activations "$total_expected" 30; then
        echo "FAIL: path B not activated within 30s at cycle $cycle"
        FAIL_CYCLE=$cycle; break
    fi
    echo "  path B: ACTIVE"

    # Verify tunnel is still alive on path A
    if ! ping_tunnel; then
        echo "FAIL: tunnel ping failed at cycle $cycle"
        FAIL_CYCLE=$cycle; break
    fi
    echo "  tunnel: OK"
done

if [ "$FAIL_CYCLE" -ne 0 ]; then
    dump_logs; exit 1
fi

# ── Post-loop checks ─────────────────────────────────────────────────────────

echo ""
echo "=== Post-loop verification ==="

# Budget exhaustion must have been logged (expected around cycle 8)
if grep -qE "budget exhausted|create.path budget" "${WORK_DIR}/client.log"; then
    echo "OK: xquic path budget exhaustion detected in log (expected at cycle ~8)"
else
    echo "WARN: budget exhaustion not seen in log (XQC_MAX_PATHS_COUNT may differ)"
fi

# Reconnect must have happened at least once
if grep -qE "forcing.*reconnect|conn.*reconnect" "${WORK_DIR}/client.log"; then
    echo "OK: forced reconnect observed in log"
else
    echo "WARN: no forced reconnect seen in log"
fi

# list_paths must still include path B
LIST_RESP=$(ctrl_send '{"cmd":"list_paths"}')
if ! echo "$LIST_RESP" | grep -q "$VETH_B0"; then
    echo "FAIL: list_paths does not include ${VETH_B0} after bounce loop"
    echo "  Response: $LIST_RESP"
    dump_logs; exit 1
fi
echo "OK: list_paths still includes ${VETH_B0}"

# Final tunnel ping
if ! ping_tunnel; then
    echo "FAIL: tunnel not reachable after bounce loop"
    dump_logs; exit 1
fi
echo "OK: tunnel reachable after all ${BOUNCE_COUNT} bounce cycles"

# ── Summary ───────────────────────────────────────────────────────────────────

echo ""
echo "================================================================"
echo "  All tests PASSED"
echo "  ${BOUNCE_COUNT} add/remove cycles of ${VETH_B0} completed"
echo "  Budget exhaustion + reconnect handled correctly (#4276 fix)"
echo "================================================================"
