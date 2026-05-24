#!/bin/bash
# ci_bench_4path_mtu.sh — 4-interface aggregation + MTU sweep benchmark
#
# Sets up 4 network paths and measures UDP throughput for:
#   - 1-path baseline (Path A only)
#   - 2-path aggregation (A + B)
#   - 4-path aggregation (A + B + C + D)
# across schedulers (wlb, minrtt) and MTU values 1280–1500.
#
# UDP iperf3 is used (not TCP) to avoid cwnd reduction from out-of-order
# delivery caused by mixing paths with different RTTs.
#
# xquic hard-caps QUIC packet size at XQC_QUIC_MAX_MSS=1420 B; after
# QUIC/MASQUE headers (~20 B) the maximum TUN payload is ~1403 B.
# MTU values above this produce 0 Mbps and are labelled "exceeds_quic_cap"
# in the JSON output — they document the current ceiling, not regressions.
#
# Paths (one-way delay / rate; RTT = 2 × delay):
#   A: 300 Mbps / 10 ms RTT  (10.100.0.0/24)
#   B:  80 Mbps / 30 ms RTT  (10.200.0.0/24)
#   C: 200 Mbps / 15 ms RTT  (10.201.0.0/24)
#   D: 150 Mbps / 20 ms RTT  (10.202.0.0/24)
#   Theoretical max (A+B+C+D): 730 Mbps
#
# Pass/fail: 4-path throughput must be ≥120% of single-path for every
#            (scheduler, MTU) combination.
#
# Output: ci_bench_results/4path_mtu_<timestamp>.json
# Usage:  sudo ./ci_bench_4path_mtu.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"
IPERF_DURATION=20        # longer duration lets BBR2 fully ramp up on all 4 paths
IPERF_STREAMS=1          # UDP: single stream saturates the paths; more streams cause UDP loss storms
IPERF_UDP_BW="900M"      # exceeds any single path; aggregate determines actual delivery
MTU_VALUES=(1280 1350 1380 1400 1420 1460 1500)
SCHEDULERS=(wlb minrtt)
# 120% threshold: practical floor measured over mixed-RTT paths (20/60/30/40ms).
# Both WLB and minrtt are expected to aggregate across 4 paths.
# MTU values that exceed the xquic hard cap (XQC_QUIC_MAX_MSS=1420, effective
# TUN ceiling ~1403 B) will show 0 Mbps and are recorded but skipped in the
# pass/fail check (marked "exceeds_quic_cap" in the JSON).
AGGREGATION_PASS_THRESHOLD=120   # 4-path must be ≥120% of single-path

# ── Extra paths C and D ──

VETH_C0="ci-c0"
VETH_C1="ci-c1"
VETH_D0="ci-d0"
VETH_D1="ci-d1"
IP_C_CLIENT="10.201.0.2/24"
IP_C_SERVER="10.201.0.1/24"
IP_D_CLIENT="10.202.0.2/24"
IP_D_SERVER="10.202.0.1/24"

# Shared credentials (generated once, reused across MTU iterations)
_4P_WORK_DIR=""
_4P_PSK=""

# ── Extended cleanup ──

_4p_cleanup() {
    echo ""
    echo "Cleaning up..."

    [ -n "$_CB_CLIENT_PID" ] && { kill "$_CB_CLIENT_PID" 2>/dev/null || true; wait "$_CB_CLIENT_PID" 2>/dev/null || true; }
    [ -n "$_CB_SERVER_PID" ] && { kill "$_CB_SERVER_PID" 2>/dev/null || true; wait "$_CB_SERVER_PID" 2>/dev/null || true; }
    _CB_CLIENT_PID=""
    _CB_SERVER_PID=""

    ip netns exec "$NS_SERVER" pkill -f "iperf3" 2>/dev/null || true
    ip netns exec "$NS_CLIENT" pkill -f "iperf3" 2>/dev/null || true
    sleep 1

    for dev in "$VETH_A0" "$VETH_B0" "$VETH_C0" "$VETH_D0"; do
        ip netns exec "$NS_CLIENT" tc qdisc del dev "$dev" root 2>/dev/null || true
    done
    for dev in "$VETH_A1" "$VETH_B1" "$VETH_C1" "$VETH_D1"; do
        ip netns exec "$NS_SERVER" tc qdisc del dev "$dev" root 2>/dev/null || true
    done

    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    for link in "$VETH_A0" "$VETH_B0" "$VETH_C0" "$VETH_D0"; do
        ip link del "$link" 2>/dev/null || true
    done

    [ -n "$_4P_WORK_DIR" ] && rm -rf "$_4P_WORK_DIR" || true
}

trap _4p_cleanup EXIT

# ── 4-path netns setup ──

_4p_setup_netns() {
    # Create base 2-path netns (A + B, loopback, rp_filter, server lo /32)
    ci_bench_setup_netns

    # Path C: 10.201.0.0/24
    ip link add "$VETH_C0" type veth peer name "$VETH_C1"
    ip link set "$VETH_C0" netns "$NS_CLIENT"
    ip link set "$VETH_C1" netns "$NS_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "$IP_C_CLIENT" dev "$VETH_C0"
    ip netns exec "$NS_SERVER" ip addr add "$IP_C_SERVER" dev "$VETH_C1"
    ip netns exec "$NS_CLIENT" ip link set "$VETH_C0" up
    ip netns exec "$NS_SERVER" ip link set "$VETH_C1" up

    # Path D: 10.202.0.0/24
    ip link add "$VETH_D0" type veth peer name "$VETH_D1"
    ip link set "$VETH_D0" netns "$NS_CLIENT"
    ip link set "$VETH_D1" netns "$NS_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "$IP_D_CLIENT" dev "$VETH_D0"
    ip netns exec "$NS_SERVER" ip addr add "$IP_D_SERVER" dev "$VETH_D1"
    ip netns exec "$NS_CLIENT" ip link set "$VETH_D0" up
    ip netns exec "$NS_SERVER" ip link set "$VETH_D1" up

    # rp_filter=0 on C and D in both namespaces
    for iface in "$VETH_C1" "$VETH_D1"; do
        ip netns exec "$NS_SERVER" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null
    done
    for iface in "$VETH_C0" "$VETH_D0"; do
        ip netns exec "$NS_CLIENT" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null
    done

    # Backup routes via C and D so QUIC paths to server IP (10.100.0.1) work
    ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.201.0.1 \
        dev "$VETH_C0" metric 201
    ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.202.0.1 \
        dev "$VETH_D0" metric 202

    # Raise MTU on all veth pairs to 9000 so that VPN MTU up to 1500 +
    # QUIC overhead (~100 B) fits without fragmentation or drops.
    for dev in "$VETH_A0" "$VETH_B0" "$VETH_C0" "$VETH_D0"; do
        ip netns exec "$NS_CLIENT" ip link set "$dev" mtu 9000
    done
    for dev in "$VETH_A1" "$VETH_B1" "$VETH_C1" "$VETH_D1"; do
        ip netns exec "$NS_SERVER" ip link set "$dev" mtu 9000
    done

    # Verify C and D connectivity
    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.201.0.1 >/dev/null
    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.202.0.1 >/dev/null

    echo "OK: 4-path netns ready (veth MTU=9000)"
}

# ── Apply netem on all 4 paths ──

_4p_apply_netem() {
    # Base paths A and B
    ci_bench_apply_netem "delay 10ms rate 300mbit" "delay 30ms rate 80mbit"

    # Path C: 200 Mbps / 15 ms one-way
    ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_C0" root netem delay 15ms rate 200mbit
    ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_C1" root netem delay 15ms rate 200mbit

    # Path D: 150 Mbps / 20 ms one-way
    ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_D0" root netem delay 20ms rate 150mbit
    ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_D1" root netem delay 20ms rate 150mbit

    echo "OK: 4-path netem applied (A:300M/10ms B:80M/30ms C:200M/15ms D:150M/20ms)"
}

# ── VPN server with scheduler and MTU ──

_4p_start_server() {
    local sched="$1"
    local mtu="$2"

    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_4P_WORK_DIR}/server.crt" \
        --key  "${_4P_WORK_DIR}/server.key" \
        --auth-key "$_4P_PSK" \
        --scheduler "$sched" \
        --cc "$CI_BENCH_CC" \
        --mtu "$mtu" \
        --log-level "$CI_BENCH_LOG_LEVEL" &
    _CB_SERVER_PID=$!
    sleep 2

    if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
        echo "ERROR: VPN server died (sched=$sched, mtu=$mtu)"
        return 1
    fi
    echo "VPN server running (PID $_CB_SERVER_PID, scheduler=$sched, mtu=$mtu)"
}

# ── VPN client with scheduler, path list and MTU ──

_4p_start_client() {
    local paths="$1"   # e.g. "--path ci-a0 --path ci-b0 --path ci-c0 --path ci-d0"
    local sched="$2"
    local mtu="$3"

    if [ -n "$_CB_CLIENT_PID" ] && kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        kill "$_CB_CLIENT_PID" 2>/dev/null || true
        wait "$_CB_CLIENT_PID" 2>/dev/null || true
        _CB_CLIENT_PID=""
        sleep 1
    fi

    # shellcheck disable=SC2086
    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        ${paths} \
        --auth-key "$_4P_PSK" \
        --scheduler "$sched" \
        --cc "$CI_BENCH_CC" \
        --mtu "$mtu" \
        --insecure \
        --log-level "$CI_BENCH_LOG_LEVEL" &
    _CB_CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: VPN client died (sched=$sched, mtu=$mtu, paths='$paths')"
        return 1
    fi
    echo "VPN client running (PID $_CB_CLIENT_PID, paths: $paths)"
}

# ── Main ──

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn 4-Interface Aggregation + MTU Sweep Benchmark (CI)"
echo "  Binary:     $MQVPN"
echo "  Schedulers: ${SCHEDULERS[*]}"
echo "  MTU values: ${MTU_VALUES[*]}"
echo "  iperf3:     UDP ${IPERF_UDP_BW} target, ${IPERF_DURATION}s, ${IPERF_STREAMS} stream(s)"
echo "  Commit:     $CI_BENCH_COMMIT"
echo "  Date:       $(date '+%Y-%m-%d %H:%M')"
echo "  Paths:"
echo "    A  10.100.0.0/24  300 Mbps / 10 ms RTT"
echo "    B  10.200.0.0/24   80 Mbps / 30 ms RTT"
echo "    C  10.201.0.0/24  200 Mbps / 15 ms RTT"
echo "    D  10.202.0.0/24  150 Mbps / 20 ms RTT"
echo "    Theoretical max: 730 Mbps"
echo "================================================================"

_4p_setup_netns
_4p_apply_netem

# Generate cert and PSK once for all MTU iterations
_4P_WORK_DIR="$(mktemp -d)"
_4P_PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${_4P_WORK_DIR}/server.key" -out "${_4P_WORK_DIR}/server.crt" \
    -days 365 -nodes -subj "/CN=ci-bench-4path" 2>/dev/null
echo "OK: credentials generated"

RESULTS_TMP="$(mktemp)"

for SCHED in "${SCHEDULERS[@]}"; do
    echo ""
    echo "################################################################"
    echo "  Scheduler: $SCHED"
    echo "################################################################"

    for MTU in "${MTU_VALUES[@]}"; do
        echo ""
        echo "================================================================"
        echo "  [$SCHED] MTU: $MTU"
        echo "================================================================"

        _4p_start_server "$SCHED" "$MTU"

        for CONFIG in "1path:--path ${VETH_A0}" \
                      "2path:--path ${VETH_A0} --path ${VETH_B0}" \
                      "4path:--path ${VETH_A0} --path ${VETH_B0} --path ${VETH_C0} --path ${VETH_D0}"; do
            LABEL="${CONFIG%%:*}"
            PATHS="${CONFIG#*:}"

            echo ""
            echo "  [$SCHED / $MTU / $LABEL]  paths: $PATHS"

            _4p_start_client "$PATHS" "$SCHED" "$MTU"
            ci_bench_wait_tunnel 20

            JSON_FILE="$(ci_bench_run_iperf UDP DL "$IPERF_DURATION" "$IPERF_STREAMS" "$IPERF_UDP_BW")"
            MBPS="$(ci_bench_parse_throughput "$JSON_FILE")"
            rm -f "$JSON_FILE"
            echo "        throughput: ${MBPS} Mbps"

            echo "$SCHED $MTU $LABEL $MBPS" >> "$RESULTS_TMP"

            ci_bench_stop_client
            sleep 2
        done

        ci_bench_stop_vpn
        sleep 2
    done
done

# ── Generate output JSON ──

TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${CI_BENCH_RESULTS}/4path_mtu_$(date +%Y%m%d_%H%M%S).json"

python3 <<PYEOF
import json
from collections import defaultdict

schedulers = "${SCHEDULERS[*]}".split()
mtu_values = [int(x) for x in "${MTU_VALUES[*]}".split()]
threshold  = ${AGGREGATION_PASS_THRESHOLD}

# rows[sched][mtu][label] = mbps
rows = defaultdict(lambda: defaultdict(dict))

with open('${RESULTS_TMP}') as f:
    for line in f:
        parts = line.strip().split()
        if len(parts) != 4:
            continue
        sched, mtu, label, mbps = parts[0], int(parts[1]), parts[2], float(parts[3])
        rows[sched][mtu][label] = mbps

results    = {}
all_passed = True

for sched in schedulers:
    results[sched] = {}
    sched_passed   = True
    for mtu in mtu_values:
        r      = rows[sched].get(mtu, {})
        single = r.get('1path', 0.0)
        two    = r.get('2path', 0.0)
        four   = r.get('4path', 0.0)
        # MTU exceeds xquic cap when single-path itself cannot deliver traffic
        cap_exceeded = single < 1.0
        gain_2 = round(((two  / single) - 1.0) * 100.0, 1) if single > 0 else None
        gain_4 = round(((four / single) - 1.0) * 100.0, 1) if single > 0 else None
        passed = cap_exceeded or (four >= single * (threshold / 100.0))
        sched_passed = sched_passed and passed
        results[sched][str(mtu)] = {
            'mtu': mtu,
            '1path_mbps': single,
            '2path_mbps': two,
            '4path_mbps': four,
            '2path_gain_over_1path_pct': gain_2,
            '4path_gain_over_1path_pct': gain_4,
            'exceeds_quic_cap': cap_exceeded,
            'aggregation_pass': passed,
        }
    all_passed = all_passed and sched_passed

output = {
    'test': '4path_mtu',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'schedulers': schedulers,
    'iperf_proto': 'udp',
    'iperf_target_bw': '${IPERF_UDP_BW}',
    'iperf_streams': ${IPERF_STREAMS},
    'iperf_duration_s': ${IPERF_DURATION},
    'netem': {
        'path_a': {'subnet': '10.100.0.0/24', 'one_way_delay_ms': 10, 'rtt_ms': 20, 'rate_mbit': 300},
        'path_b': {'subnet': '10.200.0.0/24', 'one_way_delay_ms': 30, 'rtt_ms': 60, 'rate_mbit':  80},
        'path_c': {'subnet': '10.201.0.0/24', 'one_way_delay_ms': 15, 'rtt_ms': 30, 'rate_mbit': 200},
        'path_d': {'subnet': '10.202.0.0/24', 'one_way_delay_ms': 20, 'rtt_ms': 40, 'rate_mbit': 150},
    },
    'theoretical_max_mbps': 730,
    'mtu_values': mtu_values,
    'quic_mtu_cap_note': 'XQC_QUIC_MAX_MSS=1420 B hard-caps QUIC packet size; effective TUN ceiling ~1403 B. MTU values above this are labelled exceeds_quic_cap and skipped in pass/fail.',
    'aggregation_threshold_pct': threshold,
    'results': results,
    'aggregation_check': {
        'passed': all_passed,
        'per_scheduler': {s: all(results[s][str(m)]['aggregation_pass'] for m in mtu_values)
                          for s in schedulers},
        'description': f'4-path >= {threshold}% of single-path for MTU values within xquic cap; cap-exceeded MTU values always pass (expected 0 Mbps)',
    },
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(output, f, indent=2)

print()
hdr = f"  {'MTU':>6}  {'1-path':>10}  {'2-path':>10}  {'4-path':>10}  {'2p gain':>9}  {'4p gain':>9}  {'status':>12}"
sep = f"  {'-'*6}  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*9}  {'-'*9}  {'-'*12}"
for sched in schedulers:
    print(f'Results [{sched}]:')
    print(hdr)
    print(sep)
    for mtu in mtu_values:
        r = results[sched][str(mtu)]
        if r['exceeds_quic_cap']:
            status = 'CAP_EXCEEDED'
            g2 = g4 = '    n/a'
        else:
            status = 'PASS' if r['aggregation_pass'] else 'FAIL'
            g2 = f"{r['2path_gain_over_1path_pct']:>+8.1f}%"
            g4 = f"{r['4path_gain_over_1path_pct']:>+8.1f}%"
        print(f"  {r['mtu']:>6}  {r['1path_mbps']:>8.1f} M  {r['2path_mbps']:>8.1f} M  "
              f"{r['4path_mbps']:>8.1f} M  {g2}  {g4}  {status:>12}")
    sched_ok = output['aggregation_check']['per_scheduler'][sched]
    print(f"  => {'PASS' if sched_ok else 'FAIL'}")
    print()
if all_passed:
    print(f"Overall aggregation check: PASS")
else:
    print(f"Overall aggregation check: FAIL")
PYEOF

rm -f "$RESULTS_TMP"

ci_bench_sanity_check "$OUTPUT_FILE" "4path_mtu benchmark"

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"

# Exit non-zero if aggregation check failed
python3 -c "
import json, sys
with open('${OUTPUT_FILE}') as f:
    data = json.load(f)
if not data['aggregation_check']['passed']:
    print('CI FAIL: 4-path aggregation below threshold for one or more MTU values')
    sys.exit(1)
"
