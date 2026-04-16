#!/bin/bash
# ci_bench_failover.sh — CI failover benchmark (TTF + TTR)
#
# Measures failover TTF and TTR for all schedulers (minrtt, wlb, backup, backup_fec, rap).
#
# For each scheduler:
#   1. Setup netns with netem: Path A = 300Mbps/10ms, Path B = 80Mbps/30ms
#   2. Start VPN server + multipath client
#   3. Run 75s iperf3 transfer (TCP, -P 4)
#   4. At t=20s: inject fault on Path A (ip link set down on both ends)
#   5. At t=40s: recover Path A (ip link set up on both ends)
#   6. Parse iperf3 JSON intervals to calculate TTF, TTR, and phase averages
#
# Metrics:
#   TTF (Time-To-Fallback): seconds from fault injection until throughput
#       reaches 50% of surviving path capacity
#   TTR (Time-To-Recovery): seconds from fault recovery until throughput
#       reaches 80% of pre-fault average (path revalidation ~10-15s)
#   Pre-fault avg:  t=0-20s  (both paths active)
#   Degraded avg:   t=20-40s (surviving path only)
#   Post-recover:   last 10s (t=65-75s, after path revalidation)
#
# Output: ci_bench_results/failover_<timestamp>.json
#
# Usage: sudo ./ci_bench_failover.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"

DURATION=75
INTERVAL=0.5
FAULT_INJECT_SEC=20
FAULT_RECOVER_SEC=40
IPERF_PARALLEL=4
SCHEDULERS=(minrtt wlb backup backup_fec rap)

TTF_DEFINITION="seconds from fault injection until throughput reaches 50% of surviving path capacity (fallback detection)"
TTR_DEFINITION="seconds from fault recovery until throughput reaches 80% of pre-fault average (full recovery)"

trap ci_bench_cleanup EXIT

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn Failover TTR Benchmark (CI)"
echo "  Binary:     $MQVPN"
echo "  Schedulers: ${SCHEDULERS[*]}"
echo "  Commit:     ${CI_BENCH_COMMIT:0:12}"
echo "  Date:       $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

# ── Collect results for each scheduler ──

declare -A RESULT_PRE_FAULT
declare -A RESULT_DEGRADED
declare -A RESULT_TTF
declare -A RESULT_TTR
declare -A RESULT_POST_RECOVER
RESULTS_TMP="$(mktemp)"

for SCHED in "${SCHEDULERS[@]}"; do
    echo ""
    echo "────────────────────────────────────────"
    echo "  Scheduler: $SCHED"
    echo "────────────────────────────────────────"

    # --- Setup netns + netem ---
    ci_bench_setup_netns
    ci_bench_apply_netem

    # --- Start VPN ---
    ci_bench_start_server "$SCHED"
    ci_bench_start_client "--path $VETH_A0 --path $VETH_B0" "$SCHED"
    ci_bench_wait_tunnel 15

    # --- iperf3 server ---
    ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_SERVER_IP" -1 &>/dev/null &
    IPERF_SERVER_PID=$!
    sleep 1

    # --- iperf3 client (background, JSON output) ---
    IPERF_JSON="$(mktemp)"
    echo "Starting iperf3 for ${DURATION}s (interval=${INTERVAL}s, -P ${IPERF_PARALLEL}, JSON)..."
    ip netns exec "$NS_CLIENT" iperf3 \
        -c "$TUNNEL_SERVER_IP" -t "$DURATION" \
        -P "$IPERF_PARALLEL" \
        --interval "$INTERVAL" --json \
        > "$IPERF_JSON" 2>&1 &
    IPERF_CLIENT_PID=$!

    # --- Fault injection at t=20s (background) ---
    (
        sleep "$FAULT_INJECT_SEC"
        echo "[$(date +%T)] FAULT INJECT ($SCHED): bringing down Path A"
        ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" down
        ip netns exec "$NS_SERVER" ip link set "$VETH_A1" down
    ) &
    FAULT_INJECT_PID=$!

    # --- Fault recovery at t=40s (background) ---
    (
        sleep "$FAULT_RECOVER_SEC"
        echo "[$(date +%T)] FAULT RECOVER ($SCHED): bringing up Path A"
        ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" up
        ip netns exec "$NS_SERVER" ip link set "$VETH_A1" up
        # Re-add IPs lost when link went down
        ip netns exec "$NS_CLIENT" ip addr add "$IP_A_CLIENT" dev "$VETH_A0" 2>/dev/null || true
        ip netns exec "$NS_SERVER" ip addr add "$IP_A_SERVER" dev "$VETH_A1" 2>/dev/null || true
        # Re-apply netem on restored interfaces
        ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_A0" root netem delay 10ms rate 300mbit 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_A1" root netem delay 10ms rate 300mbit 2>/dev/null || true
    ) &
    FAULT_RECOVER_PID=$!

    # --- Wait for iperf3 to finish ---
    echo "Waiting for iperf3 to complete..."
    wait "$IPERF_CLIENT_PID" || true
    wait "$IPERF_SERVER_PID" 2>/dev/null || true
    wait "$FAULT_INJECT_PID" 2>/dev/null || true
    wait "$FAULT_RECOVER_PID" 2>/dev/null || true

    # --- Parse iperf3 JSON ---
    PARSE_RESULT=$(python3 -c "
import json, sys

with open('${IPERF_JSON}') as f:
    raw = json.load(f)

intervals = []
for iv in raw.get('intervals', []):
    s = iv['sum']
    intervals.append({
        'time_sec': round(s['end'], 2),
        'mbps': round(s['bits_per_second'] / 1e6, 1)
    })

fault_inject = ${FAULT_INJECT_SEC}
fault_recover = ${FAULT_RECOVER_SEC}

# Pre-fault average (intervals before fault injection, both paths active)
pre_fault = [iv['mbps'] for iv in intervals if iv['time_sec'] <= fault_inject]
pre_fault_avg = sum(pre_fault) / len(pre_fault) if pre_fault else 0

# Degraded average (during fault, surviving path only)
degraded = [iv['mbps'] for iv in intervals
            if iv['time_sec'] > fault_inject and iv['time_sec'] <= fault_recover]
degraded_avg = sum(degraded) / len(degraded) if degraded else 0

# TTF (Time-To-Fallback): time from fault injection until throughput reaches
# 50% of surviving path capacity. Measures how fast traffic shifts to Path B.
surviving_path_mbps = 80  # Path B rate
ttf_threshold = surviving_path_mbps * 0.5
ttf = None
for iv in intervals:
    if iv['time_sec'] > fault_inject and iv['mbps'] >= ttf_threshold:
        ttf = round(iv['time_sec'] - fault_inject, 2)
        break

# TTR (Time-To-Recovery): time from fault recovery until throughput reaches
# 80% of pre-fault average. Measures how fast full multipath resumes.
ttr_threshold = pre_fault_avg * 0.8
ttr = None
for iv in intervals:
    if iv['time_sec'] > fault_recover and iv['mbps'] >= ttr_threshold:
        ttr = round(iv['time_sec'] - fault_recover, 2)
        break

# Post-recover average (last 10s of test — path revalidation takes ~10-15s)
duration = ${DURATION}
post_recover = [iv['mbps'] for iv in intervals if iv['time_sec'] > duration - 10]
post_recover_avg = sum(post_recover) / len(post_recover) if post_recover else 0

print(f'{pre_fault_avg:.1f}')
print(f'{degraded_avg:.1f}')
print(f'{ttf}')
print(f'{ttr}')
print(f'{post_recover_avg:.1f}')
")

    PRE_FAULT=$(echo "$PARSE_RESULT" | sed -n '1p')
    DEGRADED=$(echo "$PARSE_RESULT" | sed -n '2p')
    TTF=$(echo "$PARSE_RESULT" | sed -n '3p')
    TTR=$(echo "$PARSE_RESULT" | sed -n '4p')
    POST_RECOVER=$(echo "$PARSE_RESULT" | sed -n '5p')

    echo "  Pre-fault avg:     ${PRE_FAULT} Mbps"
    echo "  Degraded avg:      ${DEGRADED} Mbps"
    echo "  TTF:               ${TTF} sec"
    echo "  TTR:               ${TTR} sec"
    echo "  Post-recover avg:  ${POST_RECOVER} Mbps"

    RESULT_PRE_FAULT[$SCHED]="$PRE_FAULT"
    RESULT_DEGRADED[$SCHED]="$DEGRADED"
    RESULT_TTF[$SCHED]="$TTF"
    RESULT_TTR[$SCHED]="$TTR"
    RESULT_POST_RECOVER[$SCHED]="$POST_RECOVER"
    echo "$SCHED $PRE_FAULT $DEGRADED $TTF $TTR $POST_RECOVER" >> "$RESULTS_TMP"

    rm -f "$IPERF_JSON"

    # --- Stop VPN before next scheduler ---
    ci_bench_stop_vpn

    # --- Tear down netns (will be recreated for next scheduler) ---
    ci_bench_cleanup_stale
done

# ── Generate combined JSON output ──

TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${CI_BENCH_RESULTS}/failover_$(date +%Y%m%d_%H%M%S).json"

python3 <<PYEOF
import json

schedulers = "${SCHEDULERS[*]}".split()
sched_results = {}

with open('${RESULTS_TMP}') as f:
    for line in f:
        parts = line.strip().split()
        if len(parts) != 6:
            continue
        sched, pre_fault, degraded, ttf, ttr, post_recover = parts
        sched_results[sched] = {
            'pre_fault_avg_mbps': float(pre_fault),
            'degraded_avg_mbps': float(degraded),
            'ttf_sec': None if ttf == 'None' else float(ttf),
            'ttr_sec': None if ttr == 'None' else float(ttr),
            'post_recover_avg_mbps': float(post_recover)
        }

result = {
    'test': 'failover',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'netem': {
        'path_a': {'one_way_delay_ms': 10, 'rtt_ms': 20, 'rate_mbit': 300},
        'path_b': {'one_way_delay_ms': 30, 'rtt_ms': 60, 'rate_mbit': 80}
    },
    'schedulers': schedulers,
    'results': {sched: sched_results.get(sched, {}) for sched in schedulers},
    'ttf_definition': '${TTF_DEFINITION}',
    'ttr_definition': '${TTR_DEFINITION}'
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(result, f, indent=2)

print(json.dumps(result, indent=2))
PYEOF

rm -f "$RESULTS_TMP"

ci_bench_sanity_check "$OUTPUT_FILE" "failover benchmark"

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"
