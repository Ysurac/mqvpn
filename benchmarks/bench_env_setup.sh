#!/bin/bash
# bench_env_setup.sh — Netns environment setup for mqvpn benchmarks
#
# Source this file from other benchmark scripts:
#   source "$(dirname "$0")/bench_env_setup.sh"
#
# Provides functions:
#   bench_setup_netns      - Create veth pairs and network namespaces
#   bench_apply_netem      - Apply tc netem shaping (Path A/B defaults)
#   bench_start_vpn_server - Start mqvpn server in server netns
#   bench_start_vpn_client - Start mqvpn client in client netns
#   bench_wait_tunnel      - Wait for TUN device ping reachability
#   bench_cleanup          - Remove all netns/veth/processes

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MQVPN="${MQVPN:-${BENCH_DIR}/../build/mqvpn}"
RESULTS_DIR="${RESULTS_DIR:-${BENCH_DIR}/../bench_results}"

# Namespace and veth names
NS_SERVER="bench-server"
NS_CLIENT="bench-client"
VETH_A0="veth-a0"
VETH_A1="veth-a1"
VETH_B0="veth-b0"
VETH_B1="veth-b1"

# IP addressing (plan topology)
IP_A_CLIENT="10.100.0.2/24"
IP_A_SERVER="10.100.0.1/24"
IP_B_CLIENT="10.200.0.2/24"
IP_B_SERVER="10.200.0.1/24"
IP_A_SERVER_ADDR="10.100.0.1"
TUNNEL_SERVER_IP="10.0.0.1"
VPN_LISTEN_PORT="4433"
BENCH_SCHEDULER="${BENCH_SCHEDULER:-wlb}"
BENCH_LOG_LEVEL="${BENCH_LOG_LEVEL:-info}"

# Process PIDs (managed by start/cleanup functions)
_BENCH_SERVER_PID=""
_BENCH_CLIENT_PID=""
_BENCH_WORK_DIR=""

bench_check_deps() {
    if [ ! -f "$MQVPN" ]; then
        echo "error: mqvpn binary not found at $MQVPN"
        echo "Build first: mkdir build && cd build && cmake .. && make -j"
        exit 1
    fi
    MQVPN="$(realpath "$MQVPN")"

    if ! command -v iperf3 &>/dev/null; then
        echo "error: iperf3 not found. Install: sudo apt install iperf3"
        exit 1
    fi

    if ! command -v openssl &>/dev/null; then
        echo "error: openssl not found"
        exit 1
    fi

    mkdir -p "$RESULTS_DIR"
}

bench_setup_netns() {
    echo "Setting up network namespaces..."

    # Clean any leftovers
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    ip link del "$VETH_A0" 2>/dev/null || true
    ip link del "$VETH_B0" 2>/dev/null || true

    # Create namespaces
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

    # IP forwarding in server namespace
    ip netns exec "$NS_SERVER" sysctl -w net.ipv4.ip_forward=1 >/dev/null

    # Verify connectivity
    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "${IP_A_SERVER_ADDR}" >/dev/null
    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.200.0.1 >/dev/null

    echo "OK: netns created (${VETH_A0}/${VETH_B0})"
}

bench_apply_netem() {
    # Default: Path A = delay 10ms rate 300mbit, Path B = delay 30ms rate 80mbit
    local netem_a="${1:-delay 10ms rate 300mbit}"
    local netem_b="${2:-delay 30ms rate 80mbit}"

    echo "Applying tc netem: Path A = ${netem_a}, Path B = ${netem_b}"

    # Clear existing rules
    ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_A0" root 2>/dev/null || true
    ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_A1" root 2>/dev/null || true
    ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_B0" root 2>/dev/null || true
    ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_B1" root 2>/dev/null || true

    # Apply on both ends for realistic behavior
    ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_A0" root netem ${netem_a}
    ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_A1" root netem ${netem_a}
    ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_B0" root netem ${netem_b}
    ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_B1" root netem ${netem_b}

    echo "OK: tc netem applied"
}

bench_start_vpn_server() {
    _BENCH_WORK_DIR="$(mktemp -d)"

    # Generate PSK
    _BENCH_PSK=$("$MQVPN" --genkey 2>/dev/null)
    echo "Generated PSK: ${_BENCH_PSK:0:8}..."

    # Generate self-signed cert
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "${_BENCH_WORK_DIR}/server.key" -out "${_BENCH_WORK_DIR}/server.crt" \
        -days 365 -nodes -subj "/CN=mqvpn-bench" 2>/dev/null

    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_BENCH_WORK_DIR}/server.crt" \
        --key "${_BENCH_WORK_DIR}/server.key" \
        --auth-key "$_BENCH_PSK" \
        --scheduler "$BENCH_SCHEDULER" \
        --log-level "$BENCH_LOG_LEVEL" &
    _BENCH_SERVER_PID=$!
    sleep 2

    if ! kill -0 "$_BENCH_SERVER_PID" 2>/dev/null; then
        echo "ERROR: VPN server process died"
        return 1
    fi
    echo "VPN server running (PID $_BENCH_SERVER_PID)"
}

bench_start_vpn_client() {
    local paths="$1"  # e.g. "--path veth-a0 --path veth-b0"

    # Kill previous client if running
    if [ -n "$_BENCH_CLIENT_PID" ] && kill -0 "$_BENCH_CLIENT_PID" 2>/dev/null; then
        kill "$_BENCH_CLIENT_PID" 2>/dev/null || true
        wait "$_BENCH_CLIENT_PID" 2>/dev/null || true
        _BENCH_CLIENT_PID=""
        sleep 1
    fi

    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        ${paths} \
        --auth-key "$_BENCH_PSK" \
        --scheduler "$BENCH_SCHEDULER" \
        --insecure \
        --log-level "$BENCH_LOG_LEVEL" &
    _BENCH_CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$_BENCH_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: VPN client process died"
        return 1
    fi
    echo "VPN client running (PID $_BENCH_CLIENT_PID)"
}

bench_wait_tunnel() {
    local timeout="${1:-15}"
    local elapsed=0

    echo "Waiting for tunnel (max ${timeout}s)..."
    while [ "$elapsed" -lt "$timeout" ]; do
        if ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$TUNNEL_SERVER_IP" >/dev/null 2>&1; then
            echo "OK: tunnel up (${elapsed}s)"
            return 0
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done

    echo "ERROR: tunnel not reachable after ${timeout}s"
    return 1
}

bench_stop_vpn_client() {
    if [ -n "$_BENCH_CLIENT_PID" ]; then
        kill "$_BENCH_CLIENT_PID" 2>/dev/null || true
        wait "$_BENCH_CLIENT_PID" 2>/dev/null || true
        _BENCH_CLIENT_PID=""
        sleep 1
    fi
}

bench_stop_vpn() {
    bench_stop_vpn_client
    if [ -n "$_BENCH_SERVER_PID" ]; then
        kill "$_BENCH_SERVER_PID" 2>/dev/null || true
        wait "$_BENCH_SERVER_PID" 2>/dev/null || true
        _BENCH_SERVER_PID=""
        sleep 1
    fi
}

bench_cleanup() {
    echo ""
    echo "Cleaning up..."

    # Kill VPN processes
    [ -n "$_BENCH_CLIENT_PID" ] && kill "$_BENCH_CLIENT_PID" 2>/dev/null || true
    [ -n "$_BENCH_SERVER_PID" ] && kill "$_BENCH_SERVER_PID" 2>/dev/null || true
    _BENCH_CLIENT_PID=""
    _BENCH_SERVER_PID=""

    # Kill stale iperf3 inside benchmark netns only (avoid killing unrelated iperf3)
    ip netns exec "$NS_SERVER" pkill -f "iperf3" 2>/dev/null || true
    ip netns exec "$NS_CLIENT" pkill -f "iperf3" 2>/dev/null || true
    sleep 1

    # Remove tc rules
    ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_A0" root 2>/dev/null || true
    ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_A1" root 2>/dev/null || true
    ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_B0" root 2>/dev/null || true
    ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_B1" root 2>/dev/null || true

    # Remove namespaces and veth pairs
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    ip link del "$VETH_A0" 2>/dev/null || true
    ip link del "$VETH_B0" 2>/dev/null || true

    # Remove temp dir
    [ -n "$_BENCH_WORK_DIR" ] && rm -rf "$_BENCH_WORK_DIR"
}
