#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# ci_bench_redundant_verify.sh — redundant (broadcast) scheduler verification
#
# The redundant scheduler (xqc_redundant_scheduler_cb, see
# third_party/xquic/src/transport/scheduler/xqc_scheduler_redundant.c) is
# supposed to do two things differently from every other multipath
# scheduler: (1) duplicate every packet onto every usable path instead of
# splitting traffic across them, and (2) keep delivering data even when one
# whole path goes dark, with no failover delay. This script checks both
# claims against a real two-path client/server, not just the scheduler's
# own unit-level fixture tests in third_party/xquic.
#
# Check 1 — duplication ratio:
#   Run the same UDP UL volume once under `redundant` and once under `wlb`
#   (a splitting scheduler, as a same-topology baseline) and compare, on the
#   client side, each physical path's TX bytes against the TUN device's TX
#   bytes (the actual pre-duplication application payload):
#     - wlb splits traffic, so each path should carry a MINORITY share of
#       the inner payload (each roughly ~50% on a 2-path, equal-weight,
#       equal-capacity setup).
#     - redundant should push each path's share close to the full inner
#       payload (both paths independently carry ~100%), and use roughly
#       2x the combined outer bytes wlb needed to move the same inner data.
#   Comparing against a live baseline instead of a hardcoded QUIC-overhead
#   percentage keeps the test meaningful even if wire overhead changes.
#
# Check 2 — resilience under total path loss:
#   Under `redundant`, black-hole path B (tc netem loss 100%) mid-transfer
#   and confirm the inner tunnel throughput barely drops — path A alone,
#   which still gets a full copy of every packet, should be carrying the
#   whole stream already, so losing B costs nothing. This is the actual
#   point of the scheduler (see its header comment); the byte-ratio check
#   alone doesn't prove delivery survives a path outage.
#
# Output: ci_bench_results/redundant_verify_<timestamp>.json
# Usage:  sudo ./ci_bench_redundant_verify.sh [path-to-mqvpn-binary] [--log-level LEVEL]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"
LOG_LEVEL="${CI_BENCH_LOG_LEVEL:-error}"

_next_is_log_level=false
for arg in "$@"; do
    if [ "$_next_is_log_level" = true ]; then
        LOG_LEVEL="$arg"
        _next_is_log_level=false
        continue
    fi
    case "$arg" in
        --log-level) _next_is_log_level=true ;;
        *) ;;
    esac
done

TUN_NAME="mqvpn0"
UDP_BW="20M"              # well below both paths' caps — no cwnd-block skew
IPERF_DURATION=10         # seconds for the duplication-ratio runs
RESIL_DURATION=8          # seconds for the loss-resilience run

# Pass criteria
DUP_MIN_PATH_RATIO=0.75   # under `redundant`, each path's TX >= 75% of inner TX
DUP_MIN_GAP=0.20          # ...and that must beat the wlb baseline by >= 20pp
RESIL_MIN_THROUGHPUT_RATIO=0.60  # inner TX during loss >= 60% of inner TX before loss

trap ci_bench_cleanup EXIT

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn Redundant Scheduler Verification"
echo "  Binary:  $MQVPN"
echo "  Commit:  $CI_BENCH_COMMIT"
echo "  Date:    $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

ci_bench_setup_netns
# Symmetric, generous capacity on both paths so neither run is skewed by
# cwnd-block spillover — this test isolates scheduler behavior, not
# aggregation under scarcity (see ci_bench_wrr_weight_verify.sh for that).
ci_bench_apply_netem "delay 10ms rate 300mbit" "delay 10ms rate 300mbit"

for dev in "$VETH_A0" "$VETH_B0"; do
    ip netns exec "$NS_CLIENT" ip link set "$dev" mtu 9000
done
for dev in "$VETH_A1" "$VETH_B1"; do
    ip netns exec "$NS_SERVER" ip link set "$dev" mtu 9000
done

_CB_WORK_DIR="$(mktemp -d)"
_CB_PSK="$("$MQVPN" --genkey 2>/dev/null)"
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${_CB_WORK_DIR}/server.key" -out "${_CB_WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=ci-redundant-verify" 2>/dev/null
echo "OK: credentials generated"

read_tx_bytes() {
    ip netns exec "$NS_CLIENT" cat "/sys/class/net/${1}/statistics/tx_bytes" 2>/dev/null || echo 0
}

# Both client veths must show real TX before xquic considers path B for
# duplication (PATH_CHALLENGE/RESPONSE must complete first) — same
# reasoning as ci_bench_wrr_weight_verify.sh's wait_for_xquic_paths_active.
wait_for_xquic_paths_active() {
    local timeout="${1:-30}" elapsed=0
    while [ "$elapsed" -lt "$timeout" ]; do
        local a b
        a=$(read_tx_bytes "$VETH_A0")
        b=$(read_tx_bytes "$VETH_B0")
        if [ "$a" -ge 1000 ] && [ "$b" -ge 1000 ]; then
            sleep 2   # PATH_CHALLENGE/RESPONSE round-trip settle time
            echo "  OK: both xquic paths active (TX bytes > 1000 on each veth)"
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    echo "  ERROR: not both xquic paths active after ${timeout}s"
    return 1
}

_start_vpn() {
    local scheduler="$1"
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_CB_WORK_DIR}/server.crt" \
        --key  "${_CB_WORK_DIR}/server.key" \
        --auth-key "$_CB_PSK" \
        --scheduler "$scheduler" \
        --cc "$CI_BENCH_CC" \
        --log-level "$LOG_LEVEL" >"${_CB_WORK_DIR}/server.log" 2>&1 &
    _CB_SERVER_PID=$!
    sleep 2
    if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
        echo "ERROR: server died (scheduler=$scheduler)"; cat "${_CB_WORK_DIR}/server.log" >&2; return 1
    fi

    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        --path "$VETH_A0" --path "$VETH_B0" \
        --auth-key "$_CB_PSK" \
        --scheduler "$scheduler" \
        --cc "$CI_BENCH_CC" \
        --insecure \
        --log-level "$LOG_LEVEL" >"${_CB_WORK_DIR}/client.log" 2>&1 &
    _CB_CLIENT_PID=$!
    sleep 3
    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: client died (scheduler=$scheduler)"; cat "${_CB_WORK_DIR}/client.log" >&2; return 1
    fi

    ci_bench_wait_tunnel 15
    wait_for_xquic_paths_active
}

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

read_tun_tx_bytes() {
    ip netns exec "$NS_CLIENT" cat "/sys/class/net/${TUN_NAME}/statistics/tx_bytes" 2>/dev/null || echo 0
}

# ── Check 1: duplication ratio, `redundant` vs `wlb` baseline ──

declare -A INNER_DELTA OUTER_A_DELTA OUTER_B_DELTA

for scheduler in redundant wlb; do
    echo ""
    echo "================================================================"
    echo "  Duplication-ratio run: scheduler=${scheduler}"
    echo "================================================================"

    _start_vpn "$scheduler"

    inner_before=$(read_tun_tx_bytes)
    a_before=$(read_tx_bytes "$VETH_A0")
    b_before=$(read_tx_bytes "$VETH_B0")

    echo "  sending UDP UL ${UDP_BW} for ${IPERF_DURATION}s ..."
    _run_udp_traffic "$IPERF_DURATION" "$UDP_BW"

    inner_after=$(read_tun_tx_bytes)
    a_after=$(read_tx_bytes "$VETH_A0")
    b_after=$(read_tx_bytes "$VETH_B0")

    INNER_DELTA[$scheduler]=$((inner_after - inner_before))
    OUTER_A_DELTA[$scheduler]=$((a_after - a_before))
    OUTER_B_DELTA[$scheduler]=$((b_after - b_before))

    echo "  inner (TUN) TX delta: ${INNER_DELTA[$scheduler]} B"
    echo "  path A TX delta:      ${OUTER_A_DELTA[$scheduler]} B"
    echo "  path B TX delta:      ${OUTER_B_DELTA[$scheduler]} B"

    ci_bench_stop_vpn
    sleep 1
done

DUP_RESULT_JSON=$(python3 -c "
inner_r = ${INNER_DELTA[redundant]}
a_r = ${OUTER_A_DELTA[redundant]}
b_r = ${OUTER_B_DELTA[redundant]}
inner_w = ${INNER_DELTA[wlb]}
a_w = ${OUTER_A_DELTA[wlb]}
b_w = ${OUTER_B_DELTA[wlb]}

def ratio(a, b, inner):
    return (min(a, b) / inner) if inner > 0 else 0.0

redundant_min_ratio = ratio(a_r, b_r, inner_r)
wlb_min_ratio = ratio(a_w, b_w, inner_w)
gap = redundant_min_ratio - wlb_min_ratio

meets_floor = redundant_min_ratio >= ${DUP_MIN_PATH_RATIO}
beats_baseline = gap >= ${DUP_MIN_GAP}
passed = meets_floor and beats_baseline

import json
print(json.dumps({
    'redundant': {'inner_bytes': inner_r, 'path_a_bytes': a_r, 'path_b_bytes': b_r,
                  'min_path_ratio': round(redundant_min_ratio, 3)},
    'wlb_baseline': {'inner_bytes': inner_w, 'path_a_bytes': a_w, 'path_b_bytes': b_w,
                      'min_path_ratio': round(wlb_min_ratio, 3)},
    'gap': round(gap, 3),
    'meets_floor': meets_floor,
    'beats_baseline': beats_baseline,
    'passed': passed,
}))
")

echo ""
echo "  redundant min-path-ratio: $(python3 -c "import json,sys; d=json.loads('''$DUP_RESULT_JSON'''); print(d['redundant']['min_path_ratio'])")"
echo "  wlb       min-path-ratio: $(python3 -c "import json,sys; d=json.loads('''$DUP_RESULT_JSON'''); print(d['wlb_baseline']['min_path_ratio'])")"
DUP_PASSED=$(python3 -c "import json; print(str(json.loads('''$DUP_RESULT_JSON''')['passed']).lower())")
if [ "$DUP_PASSED" = "true" ]; then
    echo "  => PASS  (redundant duplicates onto both paths; wlb splits as expected)"
else
    echo "  => FAIL  (redundant scheduler did not show the expected duplication ratio)"
fi

# ── Check 2: resilience under total loss on one path ──

echo ""
echo "================================================================"
echo "  Resilience run: scheduler=redundant, path B black-holed mid-transfer"
echo "================================================================"

_start_vpn redundant

echo "  baseline: UDP UL ${UDP_BW} for $((RESIL_DURATION / 2))s (both paths healthy) ..."
inner_before=$(read_tun_tx_bytes)
_run_udp_traffic "$((RESIL_DURATION / 2))" "$UDP_BW"
inner_mid=$(read_tun_tx_bytes)
BASELINE_DELTA=$((inner_mid - inner_before))

echo "  black-holing path B (100% loss) ..."
ip netns exec "$NS_CLIENT" tc qdisc change dev "$VETH_B0" root netem loss 100% 2>/dev/null || true
ip netns exec "$NS_SERVER" tc qdisc change dev "$VETH_B1" root netem loss 100% 2>/dev/null || true

echo "  under loss: UDP UL ${UDP_BW} for $((RESIL_DURATION / 2))s (path B dead) ..."
_run_udp_traffic "$((RESIL_DURATION / 2))" "$UDP_BW"
inner_after=$(read_tun_tx_bytes)
LOSS_DELTA=$((inner_after - inner_mid))

ci_bench_stop_vpn

RESIL_RESULT_JSON=$(python3 -c "
baseline = ${BASELINE_DELTA}
under_loss = ${LOSS_DELTA}
ratio = (under_loss / baseline) if baseline > 0 else 0.0
passed = ratio >= ${RESIL_MIN_THROUGHPUT_RATIO}
import json
print(json.dumps({
    'baseline_bytes': baseline,
    'under_loss_bytes': under_loss,
    'throughput_ratio': round(ratio, 3),
    'passed': passed,
}))
")

echo "  baseline inner TX:  ${BASELINE_DELTA} B"
echo "  under-loss inner TX: ${LOSS_DELTA} B"
RESIL_PASSED=$(python3 -c "import json; print(str(json.loads('''$RESIL_RESULT_JSON''')['passed']).lower())")
RESIL_RATIO=$(python3 -c "import json; print(json.loads('''$RESIL_RESULT_JSON''')['throughput_ratio'])")
if [ "$RESIL_PASSED" = "true" ]; then
    echo "  => PASS  (throughput ratio ${RESIL_RATIO} >= ${RESIL_MIN_THROUGHPUT_RATIO} despite path B total loss)"
else
    echo "  => FAIL  (throughput ratio ${RESIL_RATIO} < ${RESIL_MIN_THROUGHPUT_RATIO} — losing path B hurt more than it should)"
fi

# ── JSON output ──

mkdir -p "$CI_BENCH_RESULTS"
OUTPUT_FILE="${CI_BENCH_RESULTS}/redundant_verify_$(date +%Y%m%d_%H%M%S).json"
TIMESTAMP="$(date -Iseconds)"

ALL_PASSED="false"
[ "$DUP_PASSED" = "true" ] && [ "$RESIL_PASSED" = "true" ] && ALL_PASSED="true"

python3 -c "
import json, sys

out = {
    'test': 'redundant_verify',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'traffic': {'direction': 'udp_ul', 'target_bw': '${UDP_BW}'},
    'duplication_ratio_check': json.loads('''$DUP_RESULT_JSON'''),
    'resilience_check': json.loads('''$RESIL_RESULT_JSON'''),
    'summary': {'all_passed': json.loads('${ALL_PASSED}')},
}
with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(out, f, indent=2)
print(f'  Result file: ${OUTPUT_FILE}')
"

echo "================================================================"
echo "  $([ "$ALL_PASSED" = "true" ] && echo 'ALL CHECKS PASSED' || echo 'ONE OR MORE CHECKS FAILED')"
echo "================================================================"

[ "$ALL_PASSED" = "true" ] || exit 1
