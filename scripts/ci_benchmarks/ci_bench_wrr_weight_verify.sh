#!/bin/bash
# ci_bench_wrr_weight_verify.sh — WRR per-interface proportional-share check
#
# Loops through all 4 interfaces. On each iteration one interface is assigned
# weight=100 (HIGH) and the remaining three get weight=50 (LOW) — total
# weight=250, so the HIGH interface's expected share of client TX bytes is
# 100/250 = 40%, and each LOW interface's expected share is 50/250 = 20%.
# Traffic is sent at 20 Mbps UDP UL — far below every path's bandwidth cap —
# so no cwnd-block aggregation occurs and the test isolates weight-based
# proportional selection only.
#
# This is the WRR counterpart to ci_bench_wrtt_weight_verify.sh, but the pass
# criterion is different by design: WRTT is winner-take-all (the high-weight
# path should carry ~90%+ of bytes), while WRR interleaves continuously in
# proportion to weight, so the high-weight path should carry ~40%, not
# dominate outright. The pass criterion is: the HIGH-weight interface's share
# lands within +/- 15pp of the expected 40%, AND it is the largest share of
# the four (proving weight ordering is respected even though RTT differs
# across paths).
#
# Round-robin:
#   Pass 1: A=100  B=C=D=50   (A expected ~40% despite being best RTT)
#   Pass 2: B=100  A=C=D=50   (B expected ~40% despite being WORST RTT — key test)
#   Pass 3: C=100  A=B=D=50
#   Pass 4: D=100  A=B=C=50
#
# All four must pass for the suite to report PASS.
#
# Output: ci_bench_results/wrr_weight_verify_<timestamp>.json
# Usage:  sudo ./ci_bench_wrr_weight_verify.sh [path-to-mqvpn-binary] [--log-level LEVEL]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"
LOG_LEVEL="${CI_BENCH_LOG_LEVEL:-error}"

for arg in "$@"; do
    case "$arg" in
        --log-level) shift; LOG_LEVEL="$1"; shift ;;
        *) ;;
    esac
done

CTRL_PORT="9295"          # distinct port so this script can run alongside others

HIGH_WEIGHT=100
LOW_WEIGHT=50
TOTAL_WEIGHT=$((HIGH_WEIGHT + LOW_WEIGHT * 3))
EXPECTED_HIGH_SHARE=$(python3 -c "print(f'{${HIGH_WEIGHT} / ${TOTAL_WEIGHT} * 100:.1f}')")
UDP_BW="20M"              # well below all path caps (A:300 B:80 C:200 D:150)
IPERF_DURATION=15         # seconds per round
SHARE_TOLERANCE_PP=15     # HIGH-weight path share must be within +/- this of expected

# ── Extra paths C and D (A and B come from ci_bench_env.sh) ──

VETH_C0="ci-c0" ; VETH_C1="ci-c1"
VETH_D0="ci-d0" ; VETH_D1="ci-d1"
IP_C_CLIENT="10.201.0.2/24" ; IP_C_SERVER="10.201.0.1/24"
IP_D_CLIENT="10.202.0.2/24" ; IP_D_SERVER="10.202.0.1/24"

_WRV_WORK_DIR=""
_WRV_PSK=""
_WRV_SERVER_PID=""
_WRV_CLIENT_PID=""

# ── Cleanup ──

_wrv_cleanup() {
    [ -n "$_WRV_CLIENT_PID" ] && { kill "$_WRV_CLIENT_PID" 2>/dev/null || true; wait "$_WRV_CLIENT_PID" 2>/dev/null || true; }
    [ -n "$_WRV_SERVER_PID" ] && { kill "$_WRV_SERVER_PID" 2>/dev/null || true; wait "$_WRV_SERVER_PID" 2>/dev/null || true; }
    _WRV_CLIENT_PID="" ; _WRV_SERVER_PID=""

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

    [ -n "$_WRV_WORK_DIR" ] && rm -rf "$_WRV_WORK_DIR" || true
}
trap _wrv_cleanup EXIT

# ── 4-path netns setup ──

_wrv_setup_netns() {
    ci_bench_setup_netns   # creates NS, veth A+B, loopback, rp_filter

    ip link add "$VETH_C0" type veth peer name "$VETH_C1"
    ip link set "$VETH_C0" netns "$NS_CLIENT"
    ip link set "$VETH_C1" netns "$NS_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "$IP_C_CLIENT" dev "$VETH_C0"
    ip netns exec "$NS_SERVER" ip addr add "$IP_C_SERVER" dev "$VETH_C1"
    ip netns exec "$NS_CLIENT" ip link set "$VETH_C0" up
    ip netns exec "$NS_SERVER" ip link set "$VETH_C1" up

    ip link add "$VETH_D0" type veth peer name "$VETH_D1"
    ip link set "$VETH_D0" netns "$NS_CLIENT"
    ip link set "$VETH_D1" netns "$NS_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "$IP_D_CLIENT" dev "$VETH_D0"
    ip netns exec "$NS_SERVER" ip addr add "$IP_D_SERVER" dev "$VETH_D1"
    ip netns exec "$NS_CLIENT" ip link set "$VETH_D0" up
    ip netns exec "$NS_SERVER" ip link set "$VETH_D1" up

    for iface in "$VETH_C1" "$VETH_D1"; do
        ip netns exec "$NS_SERVER" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null
    done
    for iface in "$VETH_C0" "$VETH_D0"; do
        ip netns exec "$NS_CLIENT" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null
    done

    ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.201.0.1 dev "$VETH_C0" metric 201
    ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.202.0.1 dev "$VETH_D0" metric 202

    for dev in "$VETH_A0" "$VETH_B0" "$VETH_C0" "$VETH_D0"; do
        ip netns exec "$NS_CLIENT" ip link set "$dev" mtu 9000
    done
    for dev in "$VETH_A1" "$VETH_B1" "$VETH_C1" "$VETH_D1"; do
        ip netns exec "$NS_SERVER" ip link set "$dev" mtu 9000
    done

    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.201.0.1 >/dev/null
    ip netns exec "$NS_CLIENT" ping -c 1 -W 1 10.202.0.1 >/dev/null
    echo "OK: 4-path netns ready"
}

# Paths: A=10ms/300Mbps  B=30ms/80Mbps  C=15ms/200Mbps  D=20ms/150Mbps
# Delays are one-way; RTT = 2x delay.
_wrv_apply_netem() {
    ci_bench_apply_netem "delay 10ms rate 300mbit" "delay 30ms rate 80mbit"

    for pair in "${VETH_C0}:${NS_CLIENT}" "${VETH_C1}:${NS_SERVER}"; do
        local dev="${pair%%:*}" ns="${pair##*:}"
        ip netns exec "$ns" tc qdisc del dev "$dev" root 2>/dev/null || true
        ip netns exec "$ns" tc qdisc add dev "$dev" root netem delay 15ms rate 200mbit
    done
    for pair in "${VETH_D0}:${NS_CLIENT}" "${VETH_D1}:${NS_SERVER}"; do
        local dev="${pair%%:*}" ns="${pair##*:}"
        ip netns exec "$ns" tc qdisc del dev "$dev" root 2>/dev/null || true
        ip netns exec "$ns" tc qdisc add dev "$dev" root netem delay 20ms rate 150mbit
    done
    echo "OK: netem (A:300M/20ms  B:80M/60ms  C:200M/30ms  D:150M/40ms)"
}

# ── VPN processes ──

_wrv_start_server() {
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_WRV_WORK_DIR}/server.crt" \
        --key  "${_WRV_WORK_DIR}/server.key" \
        --auth-key "$_WRV_PSK" \
        --scheduler wrr \
        --mtu 1350 \
        --cc "$CI_BENCH_CC" \
        --log-level "$LOG_LEVEL" >"${_WRV_WORK_DIR}/server.log" 2>&1 &
    _WRV_SERVER_PID=$!
    sleep 2
    if ! kill -0 "$_WRV_SERVER_PID" 2>/dev/null; then
        echo "ERROR: server died"; cat "${_WRV_WORK_DIR}/server.log" >&2; return 1
    fi
}

_wrv_start_client() {
    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        --path "$VETH_A0" --path "$VETH_B0" --path "$VETH_C0" --path "$VETH_D0" \
        --auth-key "$_WRV_PSK" \
        --scheduler wrr \
        --mtu 1350 \
        --cc "$CI_BENCH_CC" \
        --control-port "$CTRL_PORT" \
        --insecure \
        --log-level "$LOG_LEVEL" >"${_WRV_WORK_DIR}/client.log" 2>&1 &
    _WRV_CLIENT_PID=$!
    sleep 3
    if ! kill -0 "$_WRV_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: client died"; cat "${_WRV_WORK_DIR}/client.log" >&2; return 1
    fi
}

# ── Control socket helpers ──

ctrl_send() {
    printf '%s\n' "$1" \
        | ip netns exec "$NS_CLIENT" timeout 5 nc 127.0.0.1 "$CTRL_PORT" 2>/dev/null \
        || true
}

ctrl_wait() {
    local elapsed=0
    while [ "$elapsed" -lt 20 ]; do
        if ip netns exec "$NS_CLIENT" nc -z 127.0.0.1 "$CTRL_PORT" 2>/dev/null; then
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    echo "ERROR: control socket not reachable after 20s"; return 1
}

ctrl_set_weight() {
    local iface="$1" weight="$2"
    local resp
    resp=$(ctrl_send "{\"cmd\":\"set_path_weight\",\"iface\":\"${iface}\",\"weight\":${weight}}")
    if ! echo "$resp" | grep -q '"ok":true'; then
        echo "  WARN: set_path_weight ${iface}=${weight} → ${resp}"
    fi
}

# Wait until every client veth has sent >= 1000 bytes, proving that
# xqc_conn_create_path succeeded (sets xquic_path_live=true) and PATH_CHALLENGE
# was sent on each path. After all veths cross the threshold, sleep 2 s to
# allow PATH_CHALLENGE/RESPONSE to complete → XQC_PATH_STATE_ACTIVE on every
# path. Only after that will WRR consider secondary paths and will
# xqc_conn_set_path_weight actually be applied in xquic.
wait_for_xquic_paths_active() {
    local timeout="${1:-30}" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        local all_live=true
        for dev in "$VETH_A0" "$VETH_B0" "$VETH_C0" "$VETH_D0"; do
            local txb
            txb=$(read_tx_bytes "$dev")
            if [ "$txb" -lt 1000 ]; then
                all_live=false
                break
            fi
        done
        if [ "$all_live" = "true" ]; then
            sleep 2   # PATH_CHALLENGE/RESPONSE round-trip (worst case: 60 ms on path B)
            echo "  OK: all 4 xquic paths active (TX bytes > 1000 confirmed on each veth)"
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    echo "  ERROR: not all xquic paths active after ${timeout}s"
    return 1
}

read_tx_bytes() {
    ip netns exec "$NS_CLIENT" cat "/sys/class/net/${1}/statistics/tx_bytes" 2>/dev/null || echo 0
}

# ── Single UDP UL run; discards iperf output — we measure via TX bytes ──

_run_udp_traffic() {
    local duration="$1" bw="$2"
    ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_SERVER_IP" -1 >/dev/null 2>&1 &
    local sp=$!
    sleep 1
    ip netns exec "$NS_CLIENT" iperf3 \
        -c "$TUNNEL_SERVER_IP" -t "$duration" -P 1 -u -b "$bw" \
        >/dev/null 2>&1 || true
    wait "$sp" 2>/dev/null || true
}

# ── Main ──

ci_bench_check_deps

IFACES=("$VETH_A0" "$VETH_B0" "$VETH_C0" "$VETH_D0")
LABELS=("A" "B" "C" "D")
RTTS=("20ms" "60ms" "30ms" "40ms")
RATES=("300Mbps" "80Mbps" "200Mbps" "150Mbps")

echo "================================================================"
echo "  mqvpn WRR Weight Verification — per-interface round-robin"
echo "  Binary:  $MQVPN"
echo "  Commit:  $CI_BENCH_COMMIT"
echo "  Date:    $(date '+%Y-%m-%d %H:%M')"
echo "  Traffic: UDP UL ${UDP_BW} × ${IPERF_DURATION}s  (no aggregation)"
echo "  Weights: high=${HIGH_WEIGHT}  low=${LOW_WEIGHT}  total=${TOTAL_WEIGHT}"
echo "  Expected HIGH-weight share: ${EXPECTED_HIGH_SHARE}% (+/- ${SHARE_TOLERANCE_PP}pp)"
echo "  Paths:"
echo "    A  ${VETH_A0}  300 Mbps  20 ms RTT  (best RTT)"
echo "    B  ${VETH_B0}   80 Mbps  60 ms RTT  (worst RTT)"
echo "    C  ${VETH_C0}  200 Mbps  30 ms RTT"
echo "    D  ${VETH_D0}  150 Mbps  40 ms RTT"
echo "================================================================"

_wrv_setup_netns
_wrv_apply_netem

_WRV_WORK_DIR="$(mktemp -d)"
_WRV_PSK="$("$MQVPN" --genkey 2>/dev/null)"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${_WRV_WORK_DIR}/server.key" -out "${_WRV_WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=ci-wrr-verify" 2>/dev/null
echo "OK: credentials generated"

_wrv_start_server
echo "OK: server started (PID $_WRV_SERVER_PID)"

_wrv_start_client
echo "OK: client started (PID $_WRV_CLIENT_PID)"

ctrl_wait
wait_for_xquic_paths_active

TIMESTAMP="$(date -Iseconds)"

# Collect per-round results for JSON output
_ROUND_RESULTS=""   # newline-separated JSON objects
_OVERALL_PASSED=true

for idx in 0 1 2 3; do
    dominant="${IFACES[$idx]}"
    dom_label="${LABELS[$idx]}"
    dom_rtt="${RTTS[$idx]}"
    dom_rate="${RATES[$idx]}"

    echo ""
    echo "================================================================"
    echo "  Round $((idx+1))/4: ${dom_label} (${dominant}) = ${HIGH_WEIGHT}"
    echo "  Remaining: ${LOW_WEIGHT} each  |  RTT ${dom_rtt}  |  Cap ${dom_rate}"
    echo "================================================================"

    # Apply weights for this round
    for j in 0 1 2 3; do
        iface="${IFACES[$j]}"
        if [ "$j" -eq "$idx" ]; then
            ctrl_set_weight "$iface" "$HIGH_WEIGHT"
            echo "  set ${iface} (${LABELS[$j]}) = ${HIGH_WEIGHT}  ← HIGH"
        else
            ctrl_set_weight "$iface" "$LOW_WEIGHT"
            echo "  set ${iface} (${LABELS[$j]}) = ${LOW_WEIGHT}"
        fi
    done

    sleep 3   # let xquic propagate new weights

    # Snapshot TX bytes on all 4 client veths
    b0=$(read_tx_bytes "${IFACES[0]}")
    b1=$(read_tx_bytes "${IFACES[1]}")
    b2=$(read_tx_bytes "${IFACES[2]}")
    b3=$(read_tx_bytes "${IFACES[3]}")

    echo "  sending UDP UL ${UDP_BW} for ${IPERF_DURATION}s ..."
    _run_udp_traffic "$IPERF_DURATION" "$UDP_BW"

    a0=$(read_tx_bytes "${IFACES[0]}")
    a1=$(read_tx_bytes "${IFACES[1]}")
    a2=$(read_tx_bytes "${IFACES[2]}")
    a3=$(read_tx_bytes "${IFACES[3]}")

    py_out=$(python3 -c "
ifaces  = ['${IFACES[0]}', '${IFACES[1]}', '${IFACES[2]}', '${IFACES[3]}']
labels  = ['${LABELS[0]}',  '${LABELS[1]}',  '${LABELS[2]}',  '${LABELS[3]}']
before  = [int('$b0'), int('$b1'), int('$b2'), int('$b3')]
after   = [int('$a0'), int('$a1'), int('$a2'), int('$a3')]
delta   = [max(0, a - b) for a, b in zip(after, before)]
total   = sum(delta) or 1
high_sh = delta[$idx] / total * 100
expected = ${EXPECTED_HIGH_SHARE}
tol      = $SHARE_TOLERANCE_PP
is_max   = delta[$idx] == max(delta)
within_tol = abs(high_sh - expected) <= tol
result_pass = within_tol and is_max

for lbl, ifc, d in zip(labels, ifaces, delta):
    print(f'  {lbl} ({ifc}): {d:>10} B  {d/total*100:5.1f}%')
print(f'  HIGH-weight share: {high_sh:.1f}%  (expected {expected:.1f}% +/- {tol}pp, must also be largest share)')
print(f'RESULT:{high_sh:.1f}:{str(result_pass).lower()}')
")

    # Print human-readable breakdown (all lines except the RESULT sentinel)
    echo "$py_out" | grep -v '^RESULT:'

    dom_share=$(echo "$py_out" | grep '^RESULT:' | cut -d: -f2)
    passed=$(echo    "$py_out" | grep '^RESULT:' | cut -d: -f3)

    if [ "$passed" = "true" ]; then
        echo "  => PASS  (${dom_label}=${HIGH_WEIGHT} carried ${dom_share}%, within tolerance of ${EXPECTED_HIGH_SHARE}%)"
    else
        echo "  => FAIL  (${dom_label}=${HIGH_WEIGHT} carried ${dom_share}%, expected ~${EXPECTED_HIGH_SHARE}%)"
        _OVERALL_PASSED=false
    fi

    # Build a JSON fragment for this round (collected below)
    _round_json=$(python3 -c "
import json
ifaces  = ['${IFACES[0]}', '${IFACES[1]}', '${IFACES[2]}', '${IFACES[3]}']
labels  = ['${LABELS[0]}',  '${LABELS[1]}',  '${LABELS[2]}',  '${LABELS[3]}']
before  = [int('$b0'), int('$b1'), int('$b2'), int('$b3')]
after   = [int('$a0'), int('$a1'), int('$a2'), int('$a3')]
delta   = [max(0, a - b) for a, b in zip(after, before)]
total   = sum(delta) or 1
high_sh = delta[$idx] / total * 100
expected = ${EXPECTED_HIGH_SHARE}
tol      = $SHARE_TOLERANCE_PP
is_max   = delta[$idx] == max(delta)

d = {
    'high_iface':          '${dominant}',
    'high_label':          '${dom_label}',
    'high_weight':         $HIGH_WEIGHT,
    'other_weight':        $LOW_WEIGHT,
    'high_share_pct':      round(high_sh, 1),
    'expected_share_pct':  expected,
    'tolerance_pp':        tol,
    'is_max_share':        is_max,
    'passed':              bool(abs(high_sh - expected) <= tol and is_max),
    'per_path_bytes':      dict(zip(labels, delta)),
    'per_path_share_pct':  {l: round(d/total*100,1) for l,d in zip(labels, delta)},
}
print(json.dumps(d))
")
    if [ -n "$_ROUND_RESULTS" ]; then
        _ROUND_RESULTS="${_ROUND_RESULTS},"
    fi
    _ROUND_RESULTS="${_ROUND_RESULTS}${_round_json}"
done

# ── Summary ──

echo ""
echo "================================================================"
echo "  Summary"
echo "================================================================"

# Recompute pass/fail from collected JSON for display
python3 -c "
import json
rounds = json.loads('[${_ROUND_RESULTS}]')
print(f\"  {'Round':<8}  {'HIGH':<10}  {'Weight':<10}  {'Share':>16}  {'Result':>6}\")
print(f\"  {'-'*8}  {'-'*10}  {'-'*10}  {'-'*16}  {'-'*6}\")
all_pass = True
for i, r in enumerate(rounds):
    status = 'PASS' if r['passed'] else 'FAIL'
    if not r['passed']: all_pass = False
    share_s = f\"{r['high_share_pct']:.1f}% (exp {r['expected_share_pct']:.0f}%)\"
    print(f\"  {i+1:<8}  {r['high_label']} ({r['high_iface']})  {r['high_weight']:>3} vs {r['other_weight']:<3}  {share_s:>16}  {status:>6}\")
print(f\"  {'-'*8}  {'-'*10}  {'-'*10}  {'-'*16}  {'-'*6}\")
print(f\"  {'OVERALL':<8}  {'':10}  {'':10}  {'':>16}  {'PASS' if all_pass else 'FAIL':>6}\")
"

# ── JSON output ──

OUTPUT_FILE="${CI_BENCH_RESULTS}/wrr_weight_verify_$(date +%Y%m%d_%H%M%S).json"
mkdir -p "$CI_BENCH_RESULTS"

python3 -c "
import json, sys

rounds = json.loads('[${_ROUND_RESULTS}]')
all_pass = all(r['passed'] for r in rounds)

out = {
    'test':        'wrr_weight_verify',
    'commit':      '${CI_BENCH_COMMIT}',
    'timestamp':   '${TIMESTAMP}',
    'scheduler':   'wrr',
    'mtu':         1350,
    'traffic': {
        'direction':   'udp_ul',
        'target_bw':   '${UDP_BW}',
        'duration_s':  ${IPERF_DURATION},
    },
    'weights': {
        'high':  ${HIGH_WEIGHT},
        'low':   ${LOW_WEIGHT},
        'total': ${TOTAL_WEIGHT},
    },
    'netem': {
        'path_a': {'iface': '${VETH_A0}', 'rtt_ms': 20,  'rate_mbit': 300},
        'path_b': {'iface': '${VETH_B0}', 'rtt_ms': 60,  'rate_mbit':  80},
        'path_c': {'iface': '${VETH_C0}', 'rtt_ms': 30,  'rate_mbit': 200},
        'path_d': {'iface': '${VETH_D0}', 'rtt_ms': 40,  'rate_mbit': 150},
    },
    'expected_high_share_pct': ${EXPECTED_HIGH_SHARE},
    'tolerance_pp': ${SHARE_TOLERANCE_PP},
    'rounds':  rounds,
    'summary': {
        'all_passed':          all_pass,
        'rounds_passed':       sum(1 for r in rounds if r['passed']),
        'rounds_total':        len(rounds),
        'per_round': {
            r['high_label']: r['passed'] for r in rounds
        },
    },
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(out, f, indent=2)

print()
print(f'  Result file: ${OUTPUT_FILE}')
if not all_pass:
    sys.exit(1)
"

echo "================================================================"
echo "  $([ "$_OVERALL_PASSED" = "true" ] && echo 'ALL ROUNDS PASSED' || echo 'ONE OR MORE ROUNDS FAILED')"
echo "================================================================"

[ "$_OVERALL_PASSED" = "true" ] || exit 1
