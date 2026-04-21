#!/bin/bash
# ci_bench_standby_backup.sh — Benchmark xquic-standby backup path failover
#
# Measures Time-To-Failover (TTF) and Time-To-Restore (TTR) for a primary +
# backup path configuration where the backup path is pre-validated and held in
# xquic STANDBY mode (app_path_status = STANDBY) rather than created on demand.
#
# Because the standby path is already probed and has fresh RTT measurements,
# failover should be faster than the old create-on-demand approach.
#
# Topology
#   Path A (primary) : 10.100.0.0/24  — 10ms RTT, 150 Mbit/s
#   Path B (backup)  : 10.200.0.0/24  — 30ms RTT, 150 Mbit/s
#
# Timeline (per scheduler)
#   t=0-20  : pre-fault   (Path A primary, Path B standby)
#   t=20-40 : fault       (Path A down → Path B promoted)
#   t=40-60 : recovery    (Path A restored → Path B demoted back to standby)
#   t=60-80 : second-fault (Path A down again → fast failover from warm standby)
#   t=80-90 : post        (Path A restored)
#
# Second-fault cycle validates that standby probing keeps the path warm for
# repeated failover events.
#
# Schedulers tested: backup, backup_fec
# (These are the two schedulers that natively consume app_path_status.)
#
# Output: ci_bench_results/standby_backup_<timestamp>.json
#
# Usage: sudo ./ci_bench_standby_backup.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"

DURATION=90
INTERVAL=0.5
IPERF_PARALLEL=4
SCHEDULERS=(backup backup_fec)

# Fault timing
FAULT1_INJECT_SEC=20
FAULT1_RECOVER_SEC=40
FAULT2_INJECT_SEC=60
FAULT2_RECOVER_SEC=80

# Thresholds
SURVIVING_MBPS=150   # Path B capacity
TTF_THRESHOLD_PCT=50 # 50% of surviving path = failover detected
TTR_THRESHOLD_PCT=90 # 90% of pre-fault average = fully recovered

TTF_DEFINITION="seconds from fault injection until throughput >= ${TTF_THRESHOLD_PCT}% of surviving path (${SURVIVING_MBPS} Mbit/s)"
TTR_DEFINITION="seconds from fault recovery until throughput >= ${TTR_THRESHOLD_PCT}% of pre-fault average"

trap ci_bench_cleanup EXIT

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn xquic-Standby Backup Path Benchmark"
echo "  Binary    : $MQVPN"
echo "  Schedulers: ${SCHEDULERS[*]}"
echo "  Duration  : ${DURATION}s"
echo "  Fault 1   : t=${FAULT1_INJECT_SEC}-${FAULT1_RECOVER_SEC}s"
echo "  Fault 2   : t=${FAULT2_INJECT_SEC}-${FAULT2_RECOVER_SEC}s (warm-standby reuse)"
echo "  Commit    : ${CI_BENCH_COMMIT:0:12}"
echo "  Date      : $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

RESULTS_TMP="$(mktemp)"

declare -A RES_PRE_FAULT1
declare -A RES_DEGRADED1
declare -A RES_TTF1
declare -A RES_TTR1
declare -A RES_RECOVERY1
declare -A RES_PRE_FAULT2
declare -A RES_DEGRADED2
declare -A RES_TTF2
declare -A RES_TTR2
declare -A RES_POST

for SCHED in "${SCHEDULERS[@]}"; do
    echo ""
    echo "────────────────────────────────────────"
    echo "  Scheduler: $SCHED"
    echo "────────────────────────────────────────"

    ci_bench_setup_netns
    ci_bench_apply_netem "delay 10ms rate 150mbit" "delay 30ms rate 150mbit"

    # Start server + client: Path A is primary, Path B is backup (--backup-path)
    ci_bench_start_server "$SCHED"

    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        --path "$VETH_A0" \
        --backup-path "$VETH_B0" \
        --auth-key "$_CB_PSK" \
        --scheduler "$SCHED" \
        --insecure \
        --log-level "$CI_BENCH_LOG_LEVEL" &
    _CB_CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: VPN client died"
        ci_bench_cleanup
        exit 1
    fi

    ci_bench_wait_tunnel 15

    # iperf3 server
    ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_SERVER_IP" -1 &>/dev/null &
    IPERF_SERVER_PID=$!
    sleep 1

    # iperf3 client (JSON, background)
    IPERF_JSON="$(mktemp)"
    echo "Starting iperf3 for ${DURATION}s ..."
    ip netns exec "$NS_CLIENT" iperf3 \
        -c "$TUNNEL_SERVER_IP" -t "$DURATION" \
        -P "$IPERF_PARALLEL" --interval "$INTERVAL" --json \
        > "$IPERF_JSON" 2>&1 &
    IPERF_CLIENT_PID=$!

    # ── Fault 1: primary down at t=20 ──
    (
        sleep "$FAULT1_INJECT_SEC"
        echo "[$(date +%T)] FAULT 1 INJECT ($SCHED): Path A down"
        ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" down
        ip netns exec "$NS_SERVER" ip link set "$VETH_A1" down
    ) &
    F1I_PID=$!

    # ── Fault 1: primary restored at t=40 ──
    (
        sleep "$FAULT1_RECOVER_SEC"
        echo "[$(date +%T)] FAULT 1 RECOVER ($SCHED): Path A up"
        ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" up
        ip netns exec "$NS_SERVER" ip link set "$VETH_A1" up
        ip netns exec "$NS_CLIENT" ip addr add "$IP_A_CLIENT" dev "$VETH_A0" 2>/dev/null || true
        ip netns exec "$NS_SERVER" ip addr add "$IP_A_SERVER" dev "$VETH_A1" 2>/dev/null || true
        ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_A0" root netem delay 10ms rate 150mbit 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_A1" root netem delay 10ms rate 150mbit 2>/dev/null || true
    ) &
    F1R_PID=$!

    # ── Fault 2: primary down again at t=60 (warm standby reuse) ──
    (
        sleep "$FAULT2_INJECT_SEC"
        echo "[$(date +%T)] FAULT 2 INJECT ($SCHED): Path A down (warm-standby test)"
        ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" down
        ip netns exec "$NS_SERVER" ip link set "$VETH_A1" down
    ) &
    F2I_PID=$!

    # ── Fault 2: primary restored at t=80 ──
    (
        sleep "$FAULT2_RECOVER_SEC"
        echo "[$(date +%T)] FAULT 2 RECOVER ($SCHED): Path A up"
        ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" up
        ip netns exec "$NS_SERVER" ip link set "$VETH_A1" up
        ip netns exec "$NS_CLIENT" ip addr add "$IP_A_CLIENT" dev "$VETH_A0" 2>/dev/null || true
        ip netns exec "$NS_SERVER" ip addr add "$IP_A_SERVER" dev "$VETH_A1" 2>/dev/null || true
        ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_A0" root netem delay 10ms rate 150mbit 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_A1" root netem delay 10ms rate 150mbit 2>/dev/null || true
    ) &
    F2R_PID=$!

    echo "Waiting for iperf3 to complete..."
    wait "$IPERF_CLIENT_PID" || true
    kill "$IPERF_SERVER_PID" 2>/dev/null || true
    wait "$IPERF_SERVER_PID" 2>/dev/null || true
    wait "$F1I_PID" 2>/dev/null || true
    wait "$F1R_PID" 2>/dev/null || true
    wait "$F2I_PID" 2>/dev/null || true
    wait "$F2R_PID" 2>/dev/null || true

    # ── Parse results ──
    PARSE_RESULT=$(python3 -c "
import json, sys

with open('${IPERF_JSON}') as f:
    raw = json.load(f)

intervals = []
for iv in raw.get('intervals', []):
    s = iv['sum']
    intervals.append({'t': round(s['end'], 2), 'mbps': round(s['bits_per_second'] / 1e6, 1)})

f1_inject  = ${FAULT1_INJECT_SEC}
f1_recover = ${FAULT1_RECOVER_SEC}
f2_inject  = ${FAULT2_INJECT_SEC}
f2_recover = ${FAULT2_RECOVER_SEC}
duration   = ${DURATION}
surviving  = ${SURVIVING_MBPS}
ttf_pct    = ${TTF_THRESHOLD_PCT} / 100.0
ttr_pct    = ${TTR_THRESHOLD_PCT} / 100.0

def avg(ivs): return sum(i['mbps'] for i in ivs) / len(ivs) if ivs else 0.0
def first_above(ivs, lo, hi, threshold):
    for i in ivs:
        if i['t'] > lo and i['t'] <= hi and i['mbps'] >= threshold:
            return round(i['t'] - lo, 2)
    return None

# Fault 1
pre1  = [i for i in intervals if i['t'] <= f1_inject]
deg1  = [i for i in intervals if f1_inject  < i['t'] <= f1_recover]
rec1  = [i for i in intervals if f1_recover < i['t'] <= f2_inject]

pre1_avg  = avg(pre1)
deg1_avg  = avg(deg1)
rec1_avg  = avg(rec1)
ttf1      = first_above(intervals, f1_inject,  f1_recover, surviving * ttf_pct)
ttr1      = first_above(intervals, f1_recover, f2_inject,  pre1_avg  * ttr_pct)

# Fault 2 (warm-standby reuse)
pre2  = [i for i in intervals if f1_recover < i['t'] <= f2_inject]
deg2  = [i for i in intervals if f2_inject  < i['t'] <= f2_recover]
post  = [i for i in intervals if i['t'] > f2_recover]

pre2_avg  = avg(pre2)
deg2_avg  = avg(deg2)
post_avg  = avg(post)
ttf2      = first_above(intervals, f2_inject,  f2_recover, surviving * ttf_pct)
ttr2      = first_above(intervals, f2_recover, duration,   pre2_avg  * ttr_pct)

print(f'{pre1_avg:.1f}')
print(f'{deg1_avg:.1f}')
print(f'{ttf1}')
print(f'{ttr1}')
print(f'{rec1_avg:.1f}')
print(f'{pre2_avg:.1f}')
print(f'{deg2_avg:.1f}')
print(f'{ttf2}')
print(f'{ttr2}')
print(f'{post_avg:.1f}')
")

    PRE1=$(echo  "$PARSE_RESULT" | sed -n '1p')
    DEG1=$(echo  "$PARSE_RESULT" | sed -n '2p')
    TTF1=$(echo  "$PARSE_RESULT" | sed -n '3p')
    TTR1=$(echo  "$PARSE_RESULT" | sed -n '4p')
    REC1=$(echo  "$PARSE_RESULT" | sed -n '5p')
    PRE2=$(echo  "$PARSE_RESULT" | sed -n '6p')
    DEG2=$(echo  "$PARSE_RESULT" | sed -n '7p')
    TTF2=$(echo  "$PARSE_RESULT" | sed -n '8p')
    TTR2=$(echo  "$PARSE_RESULT" | sed -n '9p')
    POST=$(echo  "$PARSE_RESULT" | sed -n '10p')

    echo "  ── Fault 1 (t=${FAULT1_INJECT_SEC}-${FAULT1_RECOVER_SEC}s, cold standby) ──"
    echo "  Pre-fault avg : ${PRE1} Mbps"
    echo "  Degraded avg  : ${DEG1} Mbps"
    echo "  TTF           : ${TTF1} sec"
    echo "  TTR           : ${TTR1} sec"
    echo "  Recovery avg  : ${REC1} Mbps"
    echo "  ── Fault 2 (t=${FAULT2_INJECT_SEC}-${FAULT2_RECOVER_SEC}s, warm standby) ──"
    echo "  Pre-fault avg : ${PRE2} Mbps"
    echo "  Degraded avg  : ${DEG2} Mbps"
    echo "  TTF           : ${TTF2} sec  (expect <= Fault-1 TTF)"
    echo "  TTR           : ${TTR2} sec"
    echo "  Post avg      : ${POST} Mbps"

    RES_PRE_FAULT1[$SCHED]="$PRE1"
    RES_DEGRADED1[$SCHED]="$DEG1"
    RES_TTF1[$SCHED]="$TTF1"
    RES_TTR1[$SCHED]="$TTR1"
    RES_RECOVERY1[$SCHED]="$REC1"
    RES_PRE_FAULT2[$SCHED]="$PRE2"
    RES_DEGRADED2[$SCHED]="$DEG2"
    RES_TTF2[$SCHED]="$TTF2"
    RES_TTR2[$SCHED]="$TTR2"
    RES_POST[$SCHED]="$POST"

    echo "$SCHED $PRE1 $DEG1 $TTF1 $TTR1 $REC1 $PRE2 $DEG2 $TTF2 $TTR2 $POST" >> "$RESULTS_TMP"

    rm -f "$IPERF_JSON"
    ci_bench_stop_vpn
    ci_bench_cleanup_stale
done

# ── Generate JSON ──

TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${CI_BENCH_RESULTS}/standby_backup_$(date +%Y%m%d_%H%M%S).json"

python3 <<PYEOF
import json

schedulers = "${SCHEDULERS[*]}".split()
sched_results = {}

with open('${RESULTS_TMP}') as f:
    for line in f:
        parts = line.strip().split()
        if len(parts) != 11:
            continue
        sched, pre1, deg1, ttf1, ttr1, rec1, pre2, deg2, ttf2, ttr2, post = parts
        sched_results[sched] = {
            'fault1_cold_standby': {
                'pre_fault_avg_mbps'  : float(pre1),
                'degraded_avg_mbps'   : float(deg1),
                'ttf_sec'             : None if ttf1 == 'None' else float(ttf1),
                'ttr_sec'             : None if ttr1 == 'None' else float(ttr1),
                'recovery_avg_mbps'   : float(rec1),
            },
            'fault2_warm_standby': {
                'pre_fault_avg_mbps'  : float(pre2),
                'degraded_avg_mbps'   : float(deg2),
                'ttf_sec'             : None if ttf2 == 'None' else float(ttf2),
                'ttr_sec'             : None if ttr2 == 'None' else float(ttr2),
                'post_avg_mbps'       : float(post),
            },
        }

result = {
    'test'      : 'standby_backup',
    'commit'    : '${CI_BENCH_COMMIT}',
    'timestamp' : '${TIMESTAMP}',
    'netem'     : {
        'path_a': {'role': 'primary',   'one_way_delay_ms': 10, 'rate_mbit': 150},
        'path_b': {'role': 'backup',    'one_way_delay_ms': 30, 'rate_mbit': 150},
    },
    'schedulers'   : schedulers,
    'results'      : {s: sched_results.get(s, {}) for s in schedulers},
    'ttf_definition': '${TTF_DEFINITION}',
    'ttr_definition': '${TTR_DEFINITION}',
    'note': 'fault1 = cold standby (first failover); fault2 = warm standby (path already probed)',
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(result, f, indent=2)

print(json.dumps(result, indent=2))
PYEOF

rm -f "$RESULTS_TMP"

ci_bench_sanity_check "$OUTPUT_FILE" "standby_backup benchmark"

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"
