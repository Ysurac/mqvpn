#!/bin/bash
# ci_bench_aggregate_cc.sh — CI aggregation benchmark across all congestion controls
#
# Measures single-path vs multipath throughput for every combination of
# congestion control algorithm and scheduler.
#
# Netem: Path A = 300 Mbps / 10 ms, Path B = 80 Mbps / 30 ms
# Theoretical max = 380 Mbps
#
# A fixed stream count is used (4 parallel streams) to keep CI runtime
# manageable while still exercising the full CC × scheduler matrix.
#
# Output: ci_bench_results/aggregate_cc_<timestamp>.json
#
# Usage: sudo ./ci_bench_aggregate_cc.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"
IPERF_DURATION=10
STREAMS=4
CC_ALGOS=(bbr2 bbr cubic new_reno copa unlimited)
SCHEDULERS=(minrtt wlb backup backup_fec rap)

trap ci_bench_cleanup EXIT

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn Aggregation Benchmark — All Congestion Controls (CI)"
echo "  Binary:     $MQVPN"
echo "  CC algos:   ${CC_ALGOS[*]}"
echo "  Schedulers: ${SCHEDULERS[*]}"
echo "  Streams:    $STREAMS (fixed)"
echo "  Duration:   ${IPERF_DURATION}s per iperf3 run"
echo "  Commit:     $CI_BENCH_COMMIT"
echo "  Date:       $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

# --- Setup netns and netem ---
ci_bench_setup_netns
ci_bench_apply_netem "delay 10ms rate 300mbit" "delay 30ms rate 80mbit"

# --- Collect results ---
# Format: cc scheduler single_mbps multi_mbps
RESULTS_TMP="$(mktemp)"

for CC in "${CC_ALGOS[@]}"; do
    echo ""
    echo "================================================================"
    echo "  Congestion Control: $CC"
    echo "================================================================"

    for SCHED in "${SCHEDULERS[@]}"; do
        echo ""
        echo "--------------------------------------------"
        echo "  [$CC / $SCHED]"
        echo "--------------------------------------------"

        ci_bench_start_server "$SCHED" "$CC"

        # --- Single-path measurement (Path A only) ---
        echo "  [1/2] single-path (Path A only)..."
        ci_bench_start_client "--path $VETH_A0" "$SCHED" "$CC"
        ci_bench_wait_tunnel 15

        SINGLE_JSON="$(ci_bench_run_iperf TCP DL "$IPERF_DURATION" "$STREAMS")"
        SINGLE_MBPS="$(ci_bench_parse_throughput "$SINGLE_JSON")"
        rm -f "$SINGLE_JSON"
        echo "        single-path: ${SINGLE_MBPS} Mbps"

        ci_bench_stop_client
        sleep 2

        # --- Multipath measurement (Path A + Path B) ---
        echo "  [2/2] multipath (Path A + Path B)..."
        ci_bench_start_client "--path $VETH_A0 --path $VETH_B0" "$SCHED" "$CC"
        ci_bench_wait_tunnel 15

        MULTI_JSON="$(ci_bench_run_iperf TCP DL "$IPERF_DURATION" "$STREAMS")"
        MULTI_MBPS="$(ci_bench_parse_throughput "$MULTI_JSON")"
        rm -f "$MULTI_JSON"
        echo "        multipath:   ${MULTI_MBPS} Mbps"

        echo "$CC $SCHED $SINGLE_MBPS $MULTI_MBPS" >> "$RESULTS_TMP"

        ci_bench_stop_vpn
        sleep 2
    done
done

# --- Generate output JSON ---
TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${CI_BENCH_RESULTS}/aggregate_cc_$(date +%Y%m%d_%H%M%S).json"

python3 <<PYEOF
import json
from collections import defaultdict

cc_algos = "${CC_ALGOS[*]}".split()
schedulers = "${SCHEDULERS[*]}".split()

# results[cc][sched] = {single_path_mbps, multipath_mbps, gain_pct}
results = {cc: {} for cc in cc_algos}

with open('${RESULTS_TMP}') as f:
    for line in f:
        parts = line.strip().split()
        if len(parts) != 4:
            continue
        cc, sched, single, multi = parts
        single_f = float(single)
        multi_f = float(multi)
        gain = ((multi_f / single_f) - 1.0) * 100.0 if single_f > 0 else 0.0
        results[cc][sched] = {
            'single_path_mbps': single_f,
            'multipath_mbps': multi_f,
            'gain_pct': round(gain, 1)
        }

output = {
    'test': 'aggregate_cc',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'parallel_streams': ${STREAMS},
    'netem': {
        'path_a': {'one_way_delay_ms': 10, 'rtt_ms': 20, 'rate_mbit': 300},
        'path_b': {'one_way_delay_ms': 30, 'rtt_ms': 60, 'rate_mbit': 80}
    },
    'theoretical_max_mbps': 380,
    'cc_algos': cc_algos,
    'schedulers': schedulers,
    'results': results
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(output, f, indent=2)

print()
print('Results summary:')
hdr = f"  {'CC':<12}" + ''.join(f"  {s:>12}" for s in schedulers)
print(hdr)
print('  ' + '-' * (12 + 14 * len(schedulers)))
for cc in cc_algos:
    row = f"  {cc:<12}"
    for sched in schedulers:
        r = results[cc].get(sched)
        if r:
            row += f"  {r['multipath_mbps']:>9.1f} M"
        else:
            row += f"  {'N/A':>11}"
    print(row)
print()
print('  (multipath Mbps — gain% in JSON)')
PYEOF

rm -f "$RESULTS_TMP"

# --- Sanity check ---
ci_bench_sanity_check "$OUTPUT_FILE" "aggregate_cc benchmark"

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"
