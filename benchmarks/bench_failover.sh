#!/bin/bash
# bench_failover.sh — Netns failover TTR (Time-To-Recovery) measurement
#
# Runs a 60-second iperf3 transfer over multipath VPN, injects a path
# failure at t=20s, recovers at t=40s, and measures how long throughput
# takes to recover to 90% of pre-fault average.
#
# Output: bench_results/failover_netns_<timestamp>.json
#
# Usage: sudo ./bench_failover.sh [-s scheduler] [-p a|b] [-P streams] [path-to-mqvpn-binary]
#   -s  Scheduler: wlb or minrtt (default: wlb)
#   -p  Fault path to inject/recover: a or b (default: a)
#   -P  iperf3 parallel streams (default: 4)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/bench_env_setup.sh"

FAULT_PATH="${FAULT_PATH:-a}"
IPERF_PARALLEL="${IPERF_PARALLEL:-4}"

while getopts "s:p:P:" opt; do
    case "$opt" in
        s) BENCH_SCHEDULER="$OPTARG" ;;
        p) FAULT_PATH="$OPTARG" ;;
        P) IPERF_PARALLEL="$OPTARG" ;;
        *) echo "Usage: $0 [-s scheduler] [-p a|b] [-P streams] [mqvpn-binary]"; exit 1 ;;
    esac
done
shift $((OPTIND - 1))

case "$FAULT_PATH" in
    a|A)
        FAULT_PATH_LABEL="A"
        FAULT_IF_CLIENT="$VETH_A0"
        FAULT_IF_SERVER="$VETH_A1"
        FAULT_SERVER_IP_CIDR="$IP_A_SERVER"
        ;;
    b|B)
        FAULT_PATH="b"
        FAULT_PATH_LABEL="B"
        FAULT_IF_CLIENT="$VETH_B0"
        FAULT_IF_SERVER="$VETH_B1"
        FAULT_SERVER_IP_CIDR="$IP_B_SERVER"
        ;;
    *)
        echo "error: invalid fault path '$FAULT_PATH' (expected a or b)"
        exit 1
        ;;
esac

case "$IPERF_PARALLEL" in
    ''|*[!0-9]*)
        echo "error: invalid -P '$IPERF_PARALLEL' (expected positive integer)"
        exit 1
        ;;
esac
if [ "$IPERF_PARALLEL" -lt 1 ]; then
    echo "error: invalid -P '$IPERF_PARALLEL' (expected >= 1)"
    exit 1
fi

MQVPN="${1:-${MQVPN}}"
DURATION=60
INTERVAL=0.5
FAULT_INJECT_SEC=20
FAULT_RECOVER_SEC=40
IPERF_SERVER_PID=""

trap bench_cleanup EXIT

bench_check_deps

echo "================================================================"
echo "  mqvpn Failover TTR Benchmark (netns)"
echo "  Binary:    $MQVPN"
echo "  Scheduler: $BENCH_SCHEDULER"
echo "  FaultPath: ${FAULT_PATH_LABEL}"
echo "  Streams:   ${IPERF_PARALLEL}"
echo "  Date:      $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

# --- Setup ---
bench_setup_netns
bench_apply_netem
bench_start_vpn_server
bench_start_vpn_client "--path $VETH_A0 --path $VETH_B0"
bench_wait_tunnel 15

# --- iperf3 server ---
ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_SERVER_IP" -1 &>/dev/null &
IPERF_SERVER_PID=$!
sleep 1

# --- iperf3 client (background, JSON output) ---
IPERF_JSON="$(mktemp)"
echo "Starting iperf3 for ${DURATION}s (interval=${INTERVAL}s, JSON)..."
ip netns exec "$NS_CLIENT" iperf3 \
    -c "$TUNNEL_SERVER_IP" -t "$DURATION" \
    -P "$IPERF_PARALLEL" \
    --interval "$INTERVAL" --json \
    > "$IPERF_JSON" 2>&1 &
IPERF_CLIENT_PID=$!

# --- Fault injection at t=20s ---
sleep "$FAULT_INJECT_SEC"
echo "[$(date +%T)] FAULT INJECT: bringing down $FAULT_IF_SERVER (Path ${FAULT_PATH_LABEL} server-side)"
ip netns exec "$NS_SERVER" ip link set "$FAULT_IF_SERVER" down

# --- Fault recovery at t=40s ---
WAIT_RECOVER=$((FAULT_RECOVER_SEC - FAULT_INJECT_SEC))
sleep "$WAIT_RECOVER"
echo "[$(date +%T)] FAULT RECOVER: bringing up $FAULT_IF_SERVER (Path ${FAULT_PATH_LABEL} server-side)"
ip netns exec "$NS_SERVER" ip link set "$FAULT_IF_SERVER" up
ip netns exec "$NS_SERVER" ip addr add "$FAULT_SERVER_IP_CIDR" dev "$FAULT_IF_SERVER" 2>/dev/null || true

# --- Wait for iperf3 to finish ---
echo "Waiting for iperf3 to complete..."
wait "$IPERF_CLIENT_PID" || true
wait "$IPERF_SERVER_PID" 2>/dev/null || true
IPERF_SERVER_PID=""

# --- Parse iperf3 JSON and produce output ---
TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${RESULTS_DIR}/failover_netns_$(date +%Y%m%d_%H%M%S).json"

python3 -c "
import json, sys

with open('${IPERF_JSON}') as f:
    raw = json.load(f)

# Extract intervals
intervals = []
for iv in raw.get('intervals', []):
    s = iv['sum']
    intervals.append({
        'time_sec': round(s['end'], 2),
        'mbps': round(s['bits_per_second'] / 1e6, 1)
    })

fault_inject = ${FAULT_INJECT_SEC}
fault_recover = ${FAULT_RECOVER_SEC}

# Pre-fault average (intervals before fault injection)
pre_fault = [iv['mbps'] for iv in intervals if iv['time_sec'] <= fault_inject]
pre_fault_avg = sum(pre_fault) / len(pre_fault) if pre_fault else 0

# TTR: time from fault injection until goodput recovers to 90% of pre-fault avg
threshold = pre_fault_avg * 0.9
ttr = None
for iv in intervals:
    if iv['time_sec'] > fault_inject and iv['mbps'] >= threshold:
        ttr = round(iv['time_sec'] - fault_inject, 2)
        break

# Post-recover average (intervals after fault_recover + 2s settling)
post_recover = [iv['mbps'] for iv in intervals if iv['time_sec'] > fault_recover + 2]
post_recover_avg = sum(post_recover) / len(post_recover) if post_recover else 0

result = {
    'test': 'failover',
    'env': 'netns',
    'scheduler': '${BENCH_SCHEDULER}',
    'fault_path': '${FAULT_PATH_LABEL}',
    'iperf_parallel_streams': ${IPERF_PARALLEL},
    'timestamp': '${TIMESTAMP}',
    'netem': {
        'path_a': {'delay_ms': 10, 'rate_mbit': 300},
        'path_b': {'delay_ms': 30, 'rate_mbit': 80}
    },
    'duration_sec': ${DURATION},
    'fault_inject_sec': fault_inject,
    'fault_recover_sec': fault_recover,
    'intervals': intervals,
    'pre_fault_avg_mbps': round(pre_fault_avg, 1),
    'ttr_sec': ttr,
    'post_recover_avg_mbps': round(post_recover_avg, 1)
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(result, f, indent=2)

print(f'Pre-fault avg:     {pre_fault_avg:.1f} Mbps')
print(f'TTR:               {ttr} sec')
print(f'Post-recover avg:  {post_recover_avg:.1f} Mbps')
"

rm -f "$IPERF_JSON"

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"
