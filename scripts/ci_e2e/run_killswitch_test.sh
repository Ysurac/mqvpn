#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# run_killswitch_test.sh — Kill switch E2E test using network namespaces
#
# Verifies that --kill-switch blocks traffic leaking outside the tunnel
# when the VPN connection drops: the block (IPv4 and IPv6) stays up while the
# client is disconnected or reconnecting and goes away only when the client
# exits, rules left by a SIGKILLed client are removed by the next client, and
# the kill switch works without routes (--no-routes).
#
# Topology (same as run_nat_test.sh):
#   vpn-client                vpn-server                  vpn-external
#     veth-c ──────────────── veth-s
#     192.168.100.1/24        192.168.100.2/24
#                              veth-wan-s ─────────────── veth-wan-e
#                              172.16.0.1/24              172.16.0.2/24
#
# Usage: sudo ./scripts/run_killswitch_test.sh [path-to-mqvpn-binary]

set -e

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

# Generate PSK
PSK=$("$MQVPN" --genkey 2>/dev/null)
echo "Generated PSK: ${PSK}"

# Generate self-signed cert
echo "Generating self-signed certificate..."
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 365 -nodes -subj "/CN=mqvpn-ks-test" 2>/dev/null

SUBNET="10.0.0.0/24"
IPTABLES_COMMENT="mqvpn-ks-test:$$"

SERVER_PID=""
CLIENT_PID=""
SANITIZER_FAIL=0
HAVE_IP6TABLES=0
command -v ip6tables >/dev/null 2>&1 && HAVE_IP6TABLES=1

cleanup() {
    echo ""
    echo "Cleaning up..."
    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    sleep 1
    # Remove iptables rules from server namespace (best-effort)
    ip netns exec vpn-server iptables -t nat -D POSTROUTING -s "$SUBNET" -o veth-wan-s -j MASQUERADE \
        -m comment --comment "$IPTABLES_COMMENT" 2>/dev/null || true
    ip netns exec vpn-server iptables -D FORWARD -i mqvpn0 -s "$SUBNET" -j ACCEPT \
        -m comment --comment "$IPTABLES_COMMENT" 2>/dev/null || true
    ip netns exec vpn-server iptables -D FORWARD -o mqvpn0 -d "$SUBNET" -j ACCEPT \
        -m comment --comment "$IPTABLES_COMMENT" 2>/dev/null || true
    # Remove any stale kill switch rules in client namespace
    ip netns exec vpn-client iptables -F OUTPUT 2>/dev/null || true
    ip netns exec vpn-client ip6tables -F OUTPUT 2>/dev/null || true
    # Remove namespaces
    ip netns del vpn-server 2>/dev/null || true
    ip netns del vpn-client 2>/dev/null || true
    ip netns del vpn-external 2>/dev/null || true
    ip link del veth-c 2>/dev/null || true
    ip link del veth-wan-s 2>/dev/null || true
    rm -rf "$WORK_DIR"
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: sanitizer errors detected"
        exit 1
    fi
}
trap cleanup EXIT

# Clean any leftover namespaces from previous runs
ip netns del vpn-server 2>/dev/null || true
ip netns del vpn-client 2>/dev/null || true
ip netns del vpn-external 2>/dev/null || true
ip link del veth-c 2>/dev/null || true
ip link del veth-wan-s 2>/dev/null || true

echo "=== Setting up network namespaces ==="
ip netns add vpn-server
ip netns add vpn-client
ip netns add vpn-external

# veth pair 1: client <-> server (underlay)
ip link add veth-c type veth peer name veth-s
ip link set veth-c netns vpn-client
ip link set veth-s netns vpn-server

ip netns exec vpn-client ip addr add 192.168.100.1/24 dev veth-c
ip netns exec vpn-server ip addr add 192.168.100.2/24 dev veth-s
ip netns exec vpn-client ip link set veth-c up
ip netns exec vpn-server ip link set veth-s up
ip netns exec vpn-client ip link set lo up
ip netns exec vpn-server ip link set lo up

# veth pair 2: server <-> external (WAN side)
ip link add veth-wan-s type veth peer name veth-wan-e
ip link set veth-wan-s netns vpn-server
ip link set veth-wan-e netns vpn-external

ip netns exec vpn-server ip addr add 172.16.0.1/24 dev veth-wan-s
ip netns exec vpn-external ip addr add 172.16.0.2/24 dev veth-wan-e
ip netns exec vpn-server ip link set veth-wan-s up
ip netns exec vpn-external ip link set veth-wan-e up
ip netns exec vpn-external ip link set lo up

# Routing
ip netns exec vpn-server ip route add default via 172.16.0.2 dev veth-wan-s
ip netns exec vpn-client ip route add default via 192.168.100.2 dev veth-c

# Enable IP forwarding in server namespace
ip netns exec vpn-server sysctl -w net.ipv4.ip_forward=1 >/dev/null

# Set up NAT in server namespace
echo "=== Setting up NAT rules in server namespace ==="
ip netns exec vpn-server iptables -t nat -A POSTROUTING -s "$SUBNET" -o veth-wan-s -j MASQUERADE \
    -m comment --comment "$IPTABLES_COMMENT"
ip netns exec vpn-server iptables -I FORWARD -i mqvpn0 -s "$SUBNET" -j ACCEPT \
    -m comment --comment "$IPTABLES_COMMENT"
ip netns exec vpn-server iptables -I FORWARD -o mqvpn0 -d "$SUBNET" -j ACCEPT \
    -m comment --comment "$IPTABLES_COMMENT"

# Verify underlay connectivity
echo "=== Verifying underlay connectivity ==="
ip netns exec vpn-client ping -c 1 -W 1 192.168.100.2 >/dev/null
echo "OK: client -> server underlay"
ip netns exec vpn-server ping -c 1 -W 1 172.16.0.2 >/dev/null
echo "OK: server -> external WAN"

echo "=== Starting VPN server ==="
ip netns exec vpn-server "$MQVPN" \
    --mode server \
    --listen 192.168.100.2:4433 \
    --subnet "$SUBNET" \
    --cert "${WORK_DIR}/server.crt" \
    --key "${WORK_DIR}/server.key" \
    --auth-key "$PSK" \
    --log-level debug > "${WORK_DIR}/server.log" 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "=== FAIL: Server process died ==="
    cat "${WORK_DIR}/server.log"
    exit 1
fi
echo "Server running (PID $SERVER_PID)"

echo "=== Starting VPN client with --kill-switch --no-reconnect ==="
ip netns exec vpn-client "$MQVPN" \
    --mode client \
    --server 192.168.100.2:4433 \
    --auth-key "$PSK" \
    --insecure \
    --kill-switch \
    --no-reconnect \
    --log-level debug > "${WORK_DIR}/client.log" 2>&1 &
CLIENT_PID=$!
sleep 3

if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "=== FAIL: Client process died ==="
    cat "${WORK_DIR}/client.log"
    exit 1
fi
echo "Client running (PID $CLIENT_PID)"

echo ""
echo "=== Test 1: Tunnel works (ping through NAT) ==="
if ip netns exec vpn-client ping -c 3 -W 2 172.16.0.2; then
    echo ""
    echo "=== Test 1 (Tunnel works): PASS ==="
else
    echo ""
    echo "=== Test 1 (Tunnel works): FAIL ==="
    echo "--- Client log ---"
    cat "${WORK_DIR}/client.log"
    exit 1
fi

echo ""
echo "=== Test 2: Kill switch iptables rules exist ==="
KS_RULES=$(ip netns exec vpn-client iptables -L OUTPUT -v -n 2>/dev/null | grep "mqvpn-ks:" || true)
if [ -z "$KS_RULES" ]; then
    echo "=== Test 2 (Kill switch rules exist): FAIL ==="
    echo "--- iptables OUTPUT ---"
    ip netns exec vpn-client iptables -L OUTPUT -v -n 2>/dev/null
    exit 1
fi
echo "Kill switch rules found:"
echo "$KS_RULES"
echo ""
echo "=== Test 2 (Kill switch rules exist): PASS ==="

echo ""
echo "=== Test 2b: Kill switch blocks IPv6 too and sits on top of OUTPUT ==="
FIRST_RULE=$(ip netns exec vpn-client iptables -S OUTPUT 2>/dev/null | grep '^-A OUTPUT' | head -1)
case "$FIRST_RULE" in
    *"-j MQVPN-KS-"*) echo "OK: first OUTPUT rule jumps to the kill switch chain" ;;
    *)
        echo "=== Test 2b (Kill switch on top): FAIL — first rule: $FIRST_RULE ==="
        ip netns exec vpn-client iptables -S 2>/dev/null
        exit 1
        ;;
esac
if [ "$HAVE_IP6TABLES" -eq 1 ]; then
    if ! ip netns exec vpn-client ip6tables -S OUTPUT 2>/dev/null | grep -q "mqvpn-ks:"; then
        echo "=== Test 2b (IPv6 blocked): FAIL — no ip6tables kill switch rules ==="
        ip netns exec vpn-client ip6tables -S 2>/dev/null
        exit 1
    fi
    echo "OK: ip6tables kill switch rules present for an IPv4-only tunnel"
fi
echo "=== Test 2b: PASS ==="

echo ""
echo "=== Test 3: Kill server, verify traffic is blocked ==="
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
echo "Server killed. Waiting for client to detect disconnect..."
sleep 5

# With --no-reconnect the client reaches CLOSED and stays in its event loop
# (see tests/test_cli_sigterm_closed.sh). The kill switch must keep blocking
# until the process exits; the rules are removed on clean exit (Test 4).
if kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "Client still running — checking kill switch blocks traffic"
    if ip netns exec vpn-client ping -c 1 -W 2 172.16.0.2 >/dev/null 2>&1; then
        echo "=== Test 3 (Traffic blocked): FAIL — ping succeeded (leak!) ==="
        exit 1
    else
        echo "OK: ping to external blocked by kill switch"
    fi
    # Also verify underlay to server is blocked (except VPN port)
    if ip netns exec vpn-client ping -c 1 -W 2 192.168.100.2 >/dev/null 2>&1; then
        echo "=== Test 3 (Traffic blocked): FAIL — underlay ping succeeded (leak!) ==="
        exit 1
    else
        echo "OK: underlay ping also blocked"
    fi
    echo ""
    echo "=== Test 3 (Traffic blocked): PASS ==="

    # Stop client for cleanup test
    kill "$CLIENT_PID" 2>/dev/null || true
    wait "$CLIENT_PID" 2>/dev/null || true
    CLIENT_PID=""
else
    check_closed_exit "$CLIENT_PID" "client" "${WORK_DIR}/client.log" || exit 1
    CLIENT_PID=""
    echo "Client exited (--no-reconnect mode, expected)"
    echo "=== Test 3 (Client exit): PASS ==="
fi

echo ""
echo "=== Test 4: Kill switch rules cleaned up after exit ==="
REMAINING=$(ip netns exec vpn-client iptables -L OUTPUT -v -n 2>/dev/null | grep "mqvpn-ks:" || true)
if [ -n "$REMAINING" ]; then
    echo "=== Test 4 (Rules cleaned up): FAIL — rules still present ==="
    echo "$REMAINING"
    exit 1
fi
if [ "$HAVE_IP6TABLES" -eq 1 ] &&
    ip netns exec vpn-client ip6tables -S 2>/dev/null | grep -qE "mqvpn-ks:|MQVPN-KS-"; then
    echo "=== Test 4 (Rules cleaned up): FAIL — ip6tables rules still present ==="
    ip netns exec vpn-client ip6tables -S 2>/dev/null
    exit 1
fi
if ip netns exec vpn-client iptables -S 2>/dev/null | grep -q "MQVPN-KS-"; then
    echo "=== Test 4 (Rules cleaned up): FAIL — kill switch chain still present ==="
    ip netns exec vpn-client iptables -S 2>/dev/null
    exit 1
fi
echo "OK: no kill switch rules remaining"
echo ""
echo "=== Test 4 (Rules cleaned up): PASS ==="

start_server() {
    ip netns exec vpn-server "$MQVPN" \
        --mode server \
        --listen 192.168.100.2:4433 \
        --subnet "$SUBNET" \
        --cert "${WORK_DIR}/server.crt" \
        --key "${WORK_DIR}/server.key" \
        --auth-key "$PSK" \
        --log-level debug >> "${WORK_DIR}/server.log" 2>&1 &
    SERVER_PID=$!
    sleep 2
    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "=== FAIL: Server process died ==="
        cat "${WORK_DIR}/server.log"
        exit 1
    fi
}

# start_client LOG [extra args...] — kill-switch client with reconnect on
start_client() {
    local log="$1"
    shift
    ip netns exec vpn-client "$MQVPN" \
        --mode client \
        --server 192.168.100.2:4433 \
        --auth-key "$PSK" \
        --insecure \
        --kill-switch \
        --log-level debug "$@" > "$log" 2>&1 &
    CLIENT_PID=$!
}

stop_client() {
    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    CLIENT_PID=""
}

# wait_tunnel SECONDS — ping through the tunnel until it answers
wait_tunnel() {
    local i
    for i in $(seq 1 "$1"); do
        ip netns exec vpn-client ping -c 1 -W 1 172.16.0.2 >/dev/null 2>&1 && return 0
        sleep 1
    done
    return 1
}

echo ""
echo "=== Test 5: Kill switch stays up while the client reconnects ==="
start_server
start_client "${WORK_DIR}/client5.log"
if ! wait_tunnel 15; then
    echo "=== Test 5 (Tunnel up): FAIL ==="
    cat "${WORK_DIR}/client5.log"
    exit 1
fi
kill "$SERVER_PID" 2>/dev/null || true
wait "$SERVER_PID" 2>/dev/null || true
SERVER_PID=""
# Wait for the client to notice (idle timeout) and enter RECONNECTING.
for i in $(seq 1 40); do
    grep -q "→ RECONNECTING" "${WORK_DIR}/client5.log" && break
    sleep 1
done
if ! grep -q "→ RECONNECTING" "${WORK_DIR}/client5.log"; then
    echo "=== Test 5 (Reconnecting): FAIL — client never entered RECONNECTING ==="
    tail -50 "${WORK_DIR}/client5.log"
    exit 1
fi
if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
    echo "=== Test 5: FAIL — client exited while reconnecting ==="
    tail -50 "${WORK_DIR}/client5.log"
    exit 1
fi
if ! ip netns exec vpn-client iptables -S OUTPUT 2>/dev/null | grep -q "mqvpn-ks:${CLIENT_PID}"; then
    echo "=== Test 5 (Rules kept): FAIL — kill switch rules removed while reconnecting ==="
    ip netns exec vpn-client iptables -S 2>/dev/null
    exit 1
fi
if ip netns exec vpn-client ping -c 1 -W 2 172.16.0.2 >/dev/null 2>&1 ||
    ip netns exec vpn-client ping -c 1 -W 2 192.168.100.2 >/dev/null 2>&1; then
    echo "=== Test 5 (Traffic blocked): FAIL — traffic leaked while reconnecting ==="
    exit 1
fi
echo "OK: traffic blocked while reconnecting"
start_server
if ! wait_tunnel 40; then
    echo "=== Test 5 (Reconnect): FAIL — tunnel did not come back ==="
    tail -50 "${WORK_DIR}/client5.log"
    exit 1
fi
echo "OK: tunnel back after reconnect, through the kill switch"
stop_client
echo "=== Test 5: PASS ==="

echo ""
echo "=== Test 6: Rules of a SIGKILLed client are removed by the next one ==="
start_client "${WORK_DIR}/client6a.log"
if ! wait_tunnel 15; then
    echo "=== Test 6 (Tunnel up): FAIL ==="
    cat "${WORK_DIR}/client6a.log"
    exit 1
fi
DEAD_PID=$CLIENT_PID
kill -KILL "$DEAD_PID" 2>/dev/null || true
wait "$DEAD_PID" 2>/dev/null || true
CLIENT_PID=""
if ! ip netns exec vpn-client iptables -S OUTPUT 2>/dev/null | grep -q "mqvpn-ks:${DEAD_PID}"; then
    echo "=== Test 6: FAIL — expected the killed client's rules to be left behind ==="
    exit 1
fi
start_client "${WORK_DIR}/client6b.log"
if ! wait_tunnel 15; then
    echo "=== Test 6 (Second client up): FAIL ==="
    cat "${WORK_DIR}/client6b.log"
    exit 1
fi
if ip netns exec vpn-client iptables -S 2>/dev/null | grep -qE "mqvpn-ks:${DEAD_PID}|MQVPN-KS-${DEAD_PID}"; then
    echo "=== Test 6 (Stale rules removed): FAIL ==="
    ip netns exec vpn-client iptables -S 2>/dev/null
    exit 1
fi
if ! grep -q "removing stale" "${WORK_DIR}/client6b.log"; then
    echo "=== Test 6: FAIL — no stale-rule removal logged ==="
    exit 1
fi
stop_client
echo "=== Test 6: PASS ==="

echo ""
echo "=== Test 7: Kill switch with --no-routes ==="
start_client "${WORK_DIR}/client7.log" --no-routes
for i in $(seq 1 15); do
    grep -q "TUN .* configured" "${WORK_DIR}/client7.log" && break
    sleep 1
done
if ! kill -0 "$CLIENT_PID" 2>/dev/null || ! grep -q "kill switch enabled" "${WORK_DIR}/client7.log" ||
    ! grep -q "TUN .* configured" "${WORK_DIR}/client7.log"; then
    echo "=== Test 7: FAIL — client with --kill-switch --no-routes did not come up ==="
    cat "${WORK_DIR}/client7.log"
    exit 1
fi
stop_client
if ip netns exec vpn-client iptables -S 2>/dev/null | grep -q "MQVPN-KS-"; then
    echo "=== Test 7: FAIL — rules left after exit ==="
    exit 1
fi
echo "=== Test 7: PASS ==="
