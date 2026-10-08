#!/bin/bash
# run_primary_removal_test.sh — E2E: the tunnel keeps running over the other
# WAN when the initial path is removed through the control API.
#
# Removing the path that carries path_id 0 abandons that path like any other
# (mqvpn_client_remove_path); the connection, its tunnel address and the other
# path stay up, with no reconnect. OpenMPTCProuter's omr-tracker removes the
# path of a WAN that went down this way (005-mqvpn-path), so with two WANs
# this is the ordinary failover.
#
# Topology (two-path, single-server), as in run_path_bounce_test.sh:
#   vpn-client                    vpn-server
#     veth-ra0 ─────────────────── veth-ra1   Path A  10.100.0.0/24 (initial path)
#     veth-rb0 ─────────────────── veth-rb1   Path B  10.200.0.0/24
#
# Cases:
#   1. --path A --path B:        remove A, take A down → tunnel stays up over B.
#   2. --path A --backup-path B: remove A, take A down → tunnel stays up over B.
#
# Usage: sudo ./scripts/ci_e2e/run_primary_removal_test.sh [path-to-mqvpn-binary]
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

NS_SERVER="vpn-server-pr"
NS_CLIENT="vpn-client-pr"
VETH_A0="veth-ra0"  VETH_A1="veth-ra1"
VETH_B0="veth-rb0"  VETH_B1="veth-rb1"

IP_A_CLIENT="10.100.0.2/24"  IP_A_SERVER="10.100.0.1/24"
IP_B_CLIENT="10.200.0.2/24"  IP_B_SERVER="10.200.0.1/24"
SERVER_ADDR="10.100.0.1"
TUNNEL_IP="10.0.0.1"

CTRL_PORT="9183"
RECOVER_TIMEOUT=10

SERVER_PID=""
CLIENT_PID=""
SANITIZER_FAIL=0

cleanup() {
    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    sleep 1
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    ip link del "$VETH_A0" 2>/dev/null || true
    ip link del "$VETH_B0" 2>/dev/null || true
    rm -rf "$WORK_DIR"
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: sanitizer errors detected"
        exit 1
    fi
}
trap cleanup EXIT

ctrl_send() {
    (
        echo "$1"
        sleep 0.1
    ) | ip netns exec "$NS_CLIENT" timeout 5 nc 127.0.0.1 "$CTRL_PORT" 2>/dev/null
}

ctrl_ok() {
    local resp
    resp=$(ctrl_send "$1")
    if echo "$resp" | grep -q '"ok":true'; then return 0; fi
    echo "  control API error: $resp"
    return 1
}

ping_tunnel() {
    ip netns exec "$NS_CLIENT" ping -c 1 -W 2 "$TUNNEL_IP" >/dev/null 2>&1
}

wait_tunnel() {
    local timeout="$1" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if ping_tunnel; then return 0; fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    return 1
}

dump_logs() {
    echo "--- client log (reconnect lines) ---"
    grep -E "reconnect|primary path|removing initial path" "${WORK_DIR}/client.log" \
        2>/dev/null | tail -20 || true
    echo "--- client log (last 30 lines) ---"
    tail -30 "${WORK_DIR}/client.log" 2>/dev/null || true
    echo "--- server log (last 10 lines) ---"
    tail -10 "${WORK_DIR}/server.log" 2>/dev/null || true
}

setup_links() {
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
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
    ip netns exec "$NS_SERVER" sysctl -w net.ipv4.ip_forward=1 >/dev/null

    # The server address lives on lo so it stays reachable over path B, and
    # the client reaches it over B once A is down.
    ip netns exec "$NS_SERVER" ip addr add "${SERVER_ADDR}/32" dev lo
    ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 \
        via 10.200.0.1 dev "$VETH_B0" metric 200

    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$SERVER_ADDR" >/dev/null
    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.200.0.1 >/dev/null
}

start_server() {
    ip netns exec "$NS_SERVER" stdbuf -oL -eL "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:4433" \
        --subnet 10.0.0.0/24 \
        --cert "${WORK_DIR}/server.crt" \
        --key "${WORK_DIR}/server.key" \
        --auth-key "$PSK" \
        --log-level debug >"${WORK_DIR}/server.log" 2>&1 &
    SERVER_PID=$!
    sleep 2
    kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server died"; dump_logs; exit 1; }
}

# run_case NAME CLIENT-PATH-ARGS...
run_case() {
    local name="$1"
    shift
    echo ""
    echo "=== Case: $name ==="
    setup_links
    start_server

    ip netns exec "$NS_CLIENT" stdbuf -oL -eL "$MQVPN" \
        --mode client \
        --server "${SERVER_ADDR}:4433" \
        "$@" \
        --auth-key "$PSK" \
        --insecure \
        --control-port "$CTRL_PORT" \
        --log-level debug >"${WORK_DIR}/client.log" 2>&1 &
    CLIENT_PID=$!
    sleep 2
    kill -0 "$CLIENT_PID" 2>/dev/null || { echo "FAIL: client died"; dump_logs; exit 1; }

    wait_tunnel 30 || { echo "FAIL: tunnel not up after 30s"; dump_logs; exit 1; }
    echo "OK: tunnel up"
    # Let path B validate before the initial path goes away.
    sleep 5

    ctrl_ok "{\"cmd\":\"remove_path\",\"iface\":\"${VETH_A0}\"}" || {
        echo "FAIL: remove_path ${VETH_A0} rejected"
        dump_logs
        exit 1
    }
    ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" down
    echo "OK: path A removed and down"

    if ! wait_tunnel "$RECOVER_TIMEOUT"; then
        echo "FAIL: $name: tunnel not up over path B within ${RECOVER_TIMEOUT}s"
        dump_logs
        exit 1
    fi
    # The initial path is abandoned like any other: no reconnect.
    if grep -qE "removing initial path|→ RECONNECTING|reconnect: using" "${WORK_DIR}/client.log"; then
        echo "FAIL: $name: removing the initial path tore the connection down"
        dump_logs
        exit 1
    fi
    echo "OK: $name: tunnel kept over ${VETH_B0} without a reconnect"

    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    CLIENT_PID=""
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    SERVER_PID=""
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: $name: sanitizer errors detected"
        exit 1
    fi
}

echo ""
echo "================================================================"
echo "  mqvpn E2E: failover after the initial path is removed"
echo "  Binary:  $MQVPN"
echo "================================================================"

PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=mqvpn-primary-removal-test" 2>/dev/null

run_case "two paths" --path "$VETH_A0" --path "$VETH_B0"
run_case "path plus backup path" --path "$VETH_A0" --backup-path "$VETH_B0"

echo ""
echo "================================================================"
echo "  All tests PASSED"
echo "================================================================"
