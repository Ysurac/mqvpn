#!/bin/bash
# run_8link_aggregation_test.sh — 8-link multipath aggregation E2E test
#
# Verifies that mqvpn correctly aggregates traffic across 8 simultaneous
# network links using the WLB scheduler.
#
# Assertions:
#   1. All 8 paths become registered (control-API list_paths check)
#   2. 8-path throughput >= 2x single-path throughput (real aggregation)
#   3. Absolute 8-path floor >= 150 Mbps (gross tunnel-breakage check)
#
# Topology (8 independent veth pairs, each 10 Mbps / 5 ms one-way):
#   vpn-client-ag8                    vpn-server-ag8
#     veth-ag0 ─────────────────── veth-ag0s   192.168.0.0/24  (primary)
#     veth-ag1 ─────────────────── veth-ag1s   192.168.1.0/24
#     veth-ag2 ─────────────────── veth-ag2s   192.168.2.0/24
#     veth-ag3 ─────────────────── veth-ag3s   192.168.3.0/24
#     veth-ag4 ─────────────────── veth-ag4s   192.168.4.0/24
#     veth-ag5 ─────────────────── veth-ag5s   192.168.5.0/24
#     veth-ag6 ─────────────────── veth-ag6s   192.168.6.0/24
#     veth-ag7 ─────────────────── veth-ag7s   192.168.7.0/24
#
# Usage: sudo ./scripts/ci_e2e/run_8link_aggregation_test.sh [path-to-mqvpn-binary] [--log-level LEVEL]
# Requires: root, iproute2, iperf3, openssl, python3, tc, nc

set -e

source "$(dirname "$0")/sanitizer_check.sh"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MQVPN=""
LOG_LEVEL="error"

while [ $# -gt 0 ]; do
    case "$1" in
        --log-level) LOG_LEVEL="$2"; shift 2 ;;
        *) [ -z "$MQVPN" ] && MQVPN="$1"; shift ;;
    esac
done

MQVPN="${MQVPN:-${SCRIPT_DIR}/../../build/mqvpn}"

if [ ! -f "$MQVPN" ]; then
    echo "error: mqvpn binary not found at $MQVPN"
    echo "Build first: mkdir build && cd build && cmake .. && make"
    exit 1
fi

for cmd in iperf3 openssl python3 tc nc; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo "error: required dependency '$cmd' not found on PATH"
        exit 2
    fi
done

MQVPN="$(realpath "$MQVPN")"
WORK_DIR="$(mktemp -d)"

NS_SERVER="vpn-server-ag8"
NS_CLIENT="vpn-client-ag8"
N_LINKS=8
SERVER_ADDR="192.168.0.1"
TUNNEL_IP="10.0.0.1"
VPN_PORT="4433"
CTRL_PORT="9183"

# Each link: 10 Mbps cap, 5 ms one-way (10 ms RTT).
# Keeping per-link rate well below the QUIC CPU ceiling (~120 Mbps on this
# machine) so netem — not the kernel — is the bottleneck on every path.
# 32 parallel TCP streams (>> 8 paths) mitigates birthday-problem clustering
# so the hash distributes across all 8 paths.
#
# Observed healthy behaviour: single ~7-9 Mbps, 8-path ~18-22 Mbps (~2.4×).
# xquic WLB does not achieve linear scaling on equal-latency paths (the
# stability timer and pacing are the limiting factor, analogous to the
# 2-path floor test which sees only ~1.08× healthy gain).
# Regression signature: 8-path collapses to ~1× single (WLB stuck on 1 path).
NETEM_RATE="10mbit"
NETEM_DELAY="5ms"

IPERF_DURATION=12
IPERF_STREAMS=32

FLOOR_SINGLE_MBPS=6
FLOOR_MULTI_MBPS=14
MIN_RATIO=1.5

SERVER_PID=""
CLIENT_PID=""
SANITIZER_FAIL=0

# ── Helpers ───────────────────────────────────────────────────────────────────

stop_client() {
    stop_and_check_sanitizer "$CLIENT_PID" "client" \
        "${WORK_DIR}/client.log" || SANITIZER_FAIL=1
    CLIENT_PID=""
    sleep 1
}

stop_all() {
    stop_client
    stop_and_check_sanitizer "$SERVER_PID" "server" \
        "${WORK_DIR}/server.log" || SANITIZER_FAIL=1
    SERVER_PID=""
    sleep 1
}

cleanup() {
    echo ""
    echo "Cleaning up..."
    stop_all
    ip netns exec "$NS_SERVER" pkill -f iperf3 2>/dev/null || true
    ip netns exec "$NS_CLIENT" pkill -f iperf3 2>/dev/null || true
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    for i in $(seq 0 $((N_LINKS - 1))); do
        ip link del "veth-ag${i}" 2>/dev/null || true
    done
    rm -rf "$WORK_DIR"
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: sanitizer errors detected"
        exit 1
    fi
}
trap cleanup EXIT

dump_logs() {
    echo ""
    echo "--- client log ---"
    cat "${WORK_DIR}/client.log" 2>/dev/null || true
    echo ""
    echo "--- server log ---"
    cat "${WORK_DIR}/server.log" 2>/dev/null || true
}

ctrl_send() {
    printf '%s\n' "$1" \
        | ip netns exec "$NS_CLIENT" timeout 5 nc 127.0.0.1 "$CTRL_PORT" 2>/dev/null \
        || true
}

wait_tunnel() {
    local timeout="${1:-25}" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        if ip netns exec "$NS_CLIENT" ping -c 1 -W 1 "$TUNNEL_IP" >/dev/null 2>&1; then
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    return 1
}

wait_ctrl_socket() {
    local elapsed=0
    while [ "$elapsed" -lt 15 ]; do
        if ip netns exec "$NS_CLIENT" nc -z 127.0.0.1 "$CTRL_PORT" 2>/dev/null; then
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    return 1
}

# Poll list_paths until all N_LINKS interfaces appear (or timeout).
wait_for_all_paths() {
    local timeout="${1:-40}" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        local resp all_found=1
        resp="$(ctrl_send '{"cmd":"list_paths"}')"
        for i in $(seq 0 $((N_LINKS - 1))); do
            if ! echo "$resp" | grep -q "veth-ag${i}"; then
                all_found=0
                break
            fi
        done
        if [ "$all_found" -eq 1 ]; then
            echo "$resp"
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    return 1
}

run_iperf3_mbps() {
    local json_file
    json_file="$(mktemp)"

    ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_IP" -1 >/dev/null 2>&1 &
    local srv_pid=$!
    sleep 1

    ip netns exec "$NS_CLIENT" iperf3 \
        -c "$TUNNEL_IP" \
        -t "$IPERF_DURATION" \
        -P "$IPERF_STREAMS" \
        -R \
        --json > "$json_file" 2>&1 || true

    wait "$srv_pid" 2>/dev/null || true

    python3 -c "
import json
try:
    with open('${json_file}') as f:
        data = json.load(f)
    end = data.get('end', {})
    if 'sum_received' in end:
        print(f\"{end['sum_received']['bits_per_second'] / 1e6:.1f}\")
    elif 'sum' in end:
        print(f\"{end['sum']['bits_per_second'] / 1e6:.1f}\")
    else:
        print('0.0')
except Exception:
    print('0.0')
"
    rm -f "$json_file"
}

start_client() {
    local paths_args=""
    for iface in $1; do
        paths_args="$paths_args --path $iface"
    done
    # shellcheck disable=SC2086
    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${SERVER_ADDR}:${VPN_PORT}" \
        $paths_args \
        --auth-key "$PSK" \
        --insecure \
        --scheduler wlb \
        --control-port "$CTRL_PORT" \
        --log-level "$LOG_LEVEL" > "${WORK_DIR}/client.log" 2>&1 &
    CLIENT_PID=$!
    sleep 3
    if ! kill -0 "$CLIENT_PID" 2>/dev/null; then
        echo "FAIL: client process died immediately"
        dump_logs
        return 1
    fi
}

# ── Pre-clean leftovers from prior runs ───────────────────────────────────────

ip netns del "$NS_SERVER" 2>/dev/null || true
ip netns del "$NS_CLIENT" 2>/dev/null || true
for i in $(seq 0 $((N_LINKS - 1))); do
    ip link del "veth-ag${i}" 2>/dev/null || true
done

# ── Credentials ───────────────────────────────────────────────────────────────

PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${WORK_DIR}/server.key" -out "${WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=mqvpn-8link-agg-test" 2>/dev/null

echo "================================================================"
echo "  mqvpn 8-link aggregation test"
echo "  Binary:   $MQVPN"
echo "  Links:    $N_LINKS × ${NETEM_RATE} / ${NETEM_DELAY} (RTT ~10 ms)"
echo "  Probe:    WLB, $IPERF_STREAMS iperf3 streams, ${IPERF_DURATION}s TCP DL"
echo "  Goal:     ratio >= ${MIN_RATIO}x single, multi >= ${FLOOR_MULTI_MBPS} Mbps (healthy ~2.4x)"
echo "================================================================"

# ── Network topology ──────────────────────────────────────────────────────────

echo ""
echo "=== Setting up namespaces and $N_LINKS veth pairs ==="

ip netns add "$NS_SERVER"
ip netns add "$NS_CLIENT"
ip netns exec "$NS_CLIENT" ip link set lo up
ip netns exec "$NS_SERVER" ip link set lo up
ip netns exec "$NS_SERVER" sysctl -w net.ipv4.ip_forward=1 >/dev/null

for iface in all default; do
    ip netns exec "$NS_SERVER" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null
    ip netns exec "$NS_CLIENT" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null
done

for i in $(seq 0 $((N_LINKS - 1))); do
    cli_if="veth-ag${i}"
    srv_if="veth-ag${i}s"
    cli_ip="192.168.${i}.2/24"
    srv_ip="192.168.${i}.1/24"

    ip link add "$cli_if" type veth peer name "$srv_if"
    ip link set "$cli_if" netns "$NS_CLIENT"
    ip link set "$srv_if" netns "$NS_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "$cli_ip" dev "$cli_if"
    ip netns exec "$NS_SERVER" ip addr add "$srv_ip" dev "$srv_if"
    ip netns exec "$NS_CLIENT" ip link set "$cli_if" up
    ip netns exec "$NS_SERVER" ip link set "$srv_if" up

    ip netns exec "$NS_SERVER" sysctl -w "net.ipv4.conf.${srv_if}.rp_filter=0" >/dev/null
    ip netns exec "$NS_CLIENT" sysctl -w "net.ipv4.conf.${cli_if}.rp_filter=0" >/dev/null

    ip netns exec "$NS_CLIENT" tc qdisc add dev "$cli_if" root netem \
        rate "$NETEM_RATE" delay "$NETEM_DELAY"
    ip netns exec "$NS_SERVER" tc qdisc add dev "$srv_if" root netem \
        rate "$NETEM_RATE" delay "$NETEM_DELAY"

    # Secondary links need a route to the server's primary subnet
    if [ "$i" -gt 0 ]; then
        ip netns exec "$NS_CLIENT" ip route add 192.168.0.0/24 \
            via "192.168.${i}.1" dev "$cli_if" metric $((200 + i * 10)) 2>/dev/null || true
    fi

    echo "  link $i: $cli_if <-> $srv_if  (192.168.${i}.0/24)"
done

# Server's primary address on lo so all secondary paths can reach it
ip netns exec "$NS_SERVER" ip addr add "${SERVER_ADDR}/32" dev lo

# Sanity-check underlay connectivity
ip netns exec "$NS_CLIENT" ping -c 1 -W 2 "$SERVER_ADDR" >/dev/null
for i in $(seq 1 $((N_LINKS - 1))); do
    ip netns exec "$NS_CLIENT" ping -c 1 -W 2 "192.168.${i}.1" >/dev/null
done
echo "OK: all $N_LINKS underlay paths reachable"

# ── VPN server ────────────────────────────────────────────────────────────────

echo ""
echo "=== Starting VPN server ==="
ip netns exec "$NS_SERVER" "$MQVPN" \
    --mode server \
    --listen "0.0.0.0:${VPN_PORT}" \
    --subnet 10.0.0.0/24 \
    --cert "${WORK_DIR}/server.crt" \
    --key  "${WORK_DIR}/server.key" \
    --auth-key "$PSK" \
    --scheduler wlb \
    --log-level "$LOG_LEVEL" > "${WORK_DIR}/server.log" 2>&1 &
SERVER_PID=$!
sleep 2

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FAIL: server process died"
    cat "${WORK_DIR}/server.log"
    exit 1
fi
echo "Server running (PID $SERVER_PID)"

# ── Phase 1: single-path baseline ────────────────────────────────────────────

echo ""
echo "[1/3] Single-path baseline (veth-ag0 only)..."
start_client "veth-ag0"

if ! wait_tunnel 20; then
    echo "FAIL: single-path tunnel did not come up within 20s"
    dump_logs; exit 1
fi
echo "OK: tunnel up"

SINGLE_MBPS="$(run_iperf3_mbps)"
echo "    throughput: ${SINGLE_MBPS} Mbps"

stop_client
sleep 2

# ── Phase 2: 8-path aggregation ───────────────────────────────────────────────

echo ""
echo "[2/3] 8-path WLB aggregation (veth-ag0 through veth-ag7)..."

ALL_IFACES="veth-ag0 veth-ag1 veth-ag2 veth-ag3 veth-ag4 veth-ag5 veth-ag6 veth-ag7"
start_client "$ALL_IFACES"

if ! wait_tunnel 25; then
    echo "FAIL: 8-path tunnel did not come up within 25s"
    dump_logs; exit 1
fi
echo "OK: tunnel up"

if ! wait_ctrl_socket; then
    echo "FAIL: control socket not ready within 15s"
    dump_logs; exit 1
fi

# ── Phase 3: verify all 8 paths via control API ───────────────────────────────

echo ""
echo "[3/3] Verifying all $N_LINKS paths registered via list_paths..."

LIST_RESP=""
if ! LIST_RESP="$(wait_for_all_paths 40)"; then
    # Show what was returned for debugging
    LIST_RESP="$(ctrl_send '{"cmd":"list_paths"}')"
    echo "FAIL: not all $N_LINKS paths appeared within 40s"
    echo "  list_paths response: $LIST_RESP"
    dump_logs; exit 1
fi

MISSING=0
for i in $(seq 0 $((N_LINKS - 1))); do
    if echo "$LIST_RESP" | grep -q "veth-ag${i}"; then
        echo "  OK: veth-ag${i} present"
    else
        echo "  FAIL: veth-ag${i} missing from list_paths"
        MISSING=1
    fi
done

if [ "$MISSING" -ne 0 ]; then
    echo "FAIL: some paths missing from list_paths: $LIST_RESP"
    dump_logs; exit 1
fi
echo "OK: all $N_LINKS paths confirmed via list_paths"

# Allow a moment for xquic to have all paths fully paced
sleep 3

MULTI_MBPS="$(run_iperf3_mbps)"
echo "    8-path throughput: ${MULTI_MBPS} Mbps"

# ── Assert aggregation ────────────────────────────────────────────────────────

python3 - <<EOF
import sys

single = float("$SINGLE_MBPS")
multi  = float("$MULTI_MBPS")
floor_single = float("$FLOOR_SINGLE_MBPS")
floor_multi  = float("$FLOOR_MULTI_MBPS")
min_ratio    = float("$MIN_RATIO")

ratio = (multi / single) if single > 0 else 0.0

print()
print(f"Aggregation check ({$N_LINKS} links × $NETEM_RATE / $NETEM_DELAY each):")
print(f"  single = {single:.1f} Mbps  (floor = {floor_single:.0f})")
print(f"  multi  = {multi:.1f} Mbps  (floor = {floor_multi:.0f})")
print(f"  multi / single = {ratio:.2f}  (min = {min_ratio:.1f})")

failures = []
if single < floor_single:
    failures.append(
        f"single-path {single:.1f} < {floor_single:.0f} Mbps floor "
        "(tunnel or netem broken?)"
    )
if multi < floor_multi:
    failures.append(
        f"8-path {multi:.1f} < {floor_multi:.0f} Mbps floor "
        "(aggregation not working?)"
    )
if single > 0 and ratio < min_ratio:
    failures.append(
        f"multi/single ratio {ratio:.2f} < {min_ratio:.1f} "
        "(WLB not spreading load across links)"
    )

if failures:
    print()
    print("FAIL:")
    for f in failures:
        print(f"  - {f}")
    sys.exit(1)

print()
print("PASS: 8-link aggregation verified")
EOF

echo ""
echo "================================================================"
echo "  All checks PASSED"
echo "  Phase 1: single-path baseline     ${SINGLE_MBPS} Mbps"
echo "  Phase 2: 8-path WLB aggregation   ${MULTI_MBPS} Mbps"
echo "  Phase 3: all $N_LINKS paths confirmed active via list_paths"
echo "================================================================"
