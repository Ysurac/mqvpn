#!/bin/bash
# run_cipher_test.sh — E2E test: verify tunnel connectivity for each TLS cipher suite
#
# For each cipher suite the script:
#   1. Starts server with --cipher <suite>
#   2. Starts client with --cipher <suite> --insecure
#   3. Pings through the tunnel (10.0.0.1) to confirm connectivity
#   4. Tears down cleanly before the next iteration
#
# Usage: sudo ./run_cipher_test.sh [path-to-mqvpn-binary]

set -e

source "$(dirname "$0")/sanitizer_check.sh"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MQVPN=""

while [ $# -gt 0 ]; do
    [ -z "$MQVPN" ] && MQVPN="$1"
    shift
done

MQVPN="${MQVPN:-${SCRIPT_DIR}/../../build/mqvpn}"

if [ ! -f "$MQVPN" ]; then
    echo "error: mqvpn binary not found at $MQVPN"
    echo "Build first: mkdir build && cd build && cmake .. && make"
    exit 1
fi

MQVPN="$(realpath "$MQVPN")"
WORK_DIR="$(mktemp -d)"

PSK=$("$MQVPN" --genkey 2>/dev/null)

echo "Generating self-signed certificate..."
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 365 -nodes -subj "/CN=mqvpn-test" 2>/dev/null

CIPHERS=(
    "TLS_AES_128_GCM_SHA256"
    "TLS_AES_256_GCM_SHA384"
    "TLS_CHACHA20_POLY1305_SHA256"
)

OVERALL_FAIL=0

teardown_netns() {
    ip netns del vpn-server 2>/dev/null || true
    ip netns del vpn-client 2>/dev/null || true
    ip link del veth-c 2>/dev/null || true
}

cleanup() {
    teardown_netns
    rm -rf "$WORK_DIR"
}
trap cleanup EXIT

for CIPHER in "${CIPHERS[@]}"; do
    echo ""
    echo "══════════════════════════════════════════════"
    echo "  Testing cipher: ${CIPHER}"
    echo "══════════════════════════════════════════════"

    teardown_netns

    ip netns add vpn-server
    ip netns add vpn-client

    ip link add veth-c type veth peer name veth-s
    ip link set veth-c netns vpn-client
    ip link set veth-s netns vpn-server

    ip netns exec vpn-client ip addr add 192.168.100.1/24 dev veth-c
    ip netns exec vpn-server ip addr add 192.168.100.2/24 dev veth-s
    ip netns exec vpn-client ip link set veth-c up
    ip netns exec vpn-server ip link set veth-s up
    ip netns exec vpn-client ip link set lo up
    ip netns exec vpn-server ip link set lo up
    ip netns exec vpn-server sysctl -w net.ipv4.ip_forward=1 >/dev/null

    ip netns exec vpn-client ping -c 1 -W 1 192.168.100.2 >/dev/null

    SERVER_LOG="${WORK_DIR}/server_${CIPHER}.log"
    CLIENT_LOG="${WORK_DIR}/client_${CIPHER}.log"

    ip netns exec vpn-server "$MQVPN" \
        --mode server \
        --listen 192.168.100.2:4433 \
        --subnet 10.0.0.0/24 \
        --cert "${WORK_DIR}/server.crt" \
        --key "${WORK_DIR}/server.key" \
        --auth-key "$PSK" \
        --cipher "$CIPHER" \
        --log-level debug > "$SERVER_LOG" 2>&1 &
    SERVER_PID=$!
    sleep 2

    if ! kill -0 "$SERVER_PID" 2>/dev/null; then
        echo "FAIL [${CIPHER}]: server process died"
        cat "$SERVER_LOG"
        OVERALL_FAIL=1
        teardown_netns
        continue
    fi

    ip netns exec vpn-client "$MQVPN" \
        --mode client \
        --server 192.168.100.2:4433 \
        --auth-key "$PSK" \
        --cipher "$CIPHER" \
        --insecure \
        --log-level debug > "$CLIENT_LOG" 2>&1 &
    CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
        echo "FAIL [${CIPHER}]: client process died"
        echo "--- Server log ---"
        cat "$SERVER_LOG"
        echo "--- Client log ---"
        cat "$CLIENT_LOG"
        OVERALL_FAIL=1
        teardown_netns
        continue
    fi

    if ip netns exec vpn-client ping -c 3 -W 2 10.0.0.1 >/dev/null 2>&1; then
        echo "PASS [${CIPHER}]: tunnel connectivity verified"
    else
        echo "FAIL [${CIPHER}]: ping through VPN tunnel failed"
        echo "--- Server log ---"
        cat "$SERVER_LOG"
        echo "--- Client log ---"
        cat "$CLIENT_LOG"
        OVERALL_FAIL=1
    fi

    kill "$SERVER_PID" "$CLIENT_PID" 2>/dev/null || true
    wait "$SERVER_PID" "$CLIENT_PID" 2>/dev/null || true

    teardown_netns
done

echo ""
if [ "$OVERALL_FAIL" -eq 0 ]; then
    echo "=== PASS: all cipher suites passed ==="
else
    echo "=== FAIL: one or more cipher suites failed ==="
    exit 1
fi
