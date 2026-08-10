#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# ci_bench_dscp_verify.sh — DSCP policy-routing scheduler verification
#
# The dscp scheduler (xqc_dscp_scheduler_cb, see
# third_party/xquic/src/transport/scheduler/xqc_scheduler_dscp.c) routes
# packets tagged with a DSCP class to whichever path's mask
# (xqc_conn_set_path_dscp_mask / mqvpn's `set_path_dscp_mask` control
# command) includes that class, and falls back to plain MinRTT across
# every usable path when no assigned path is currently usable. This script
# checks both properties against a real two-path client/server, not just
# the scheduler's own unit-level fixture tests in third_party/xquic.
#
# Check 1 — class-to-path routing:
#   Assign the SAME DSCP class (EF/46) to path A only, send EF-tagged UDP
#   traffic (iperf3 --tos), and confirm path A's client TX share of the
#   total (A+B) is high. Then flip the assignment to path B only and
#   confirm the share flips too. Reassigning one class between rounds
#   (rather than testing two different classes on fixed paths) isolates
#   mask-driven placement from any RTT/capacity bias between the two
#   symmetric paths — if routing were actually just a fixed tie-break, one
#   of the two rounds would fail.
#
# Check 2 — resilience when the assigned path goes dark:
#   With EF assigned to path A only, black-hole path A (tc netem loss 100%)
#   mid-transfer of EF-tagged traffic and confirm inner tunnel throughput
#   survives — the scheduler's fallback-to-MinRTT path should redirect
#   onto path B instead of stalling that traffic class. Same technique as
#   ci_bench_redundant_verify.sh's resilience check.
#
# Output: ci_bench_results/dscp_verify_<timestamp>.json
# Usage:  sudo ./ci_bench_dscp_verify.sh [path-to-mqvpn-binary] [--log-level LEVEL]

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
CTRL_PORT="9296"          # distinct port so this script can run alongside others
UDP_BW="20M"              # well below both paths' caps — no cwnd-block skew
IPERF_DURATION=10         # seconds for each class-routing round
RESIL_DURATION=8          # seconds for the loss-resilience run

EF_DSCP=46                # EF (Expedited Forwarding) — arbitrary but recognizable
EF_MASK=$((1 << EF_DSCP))
EF_TOS=$((EF_DSCP << 2))  # iperf3 --tos wants the full TOS byte, ECN=0

# Pass criteria
CLASS_MIN_PATH_RATIO=0.70        # assigned path's share of (A+B) TX >= 70%
RESIL_MIN_THROUGHPUT_RATIO=0.50  # inner TX during loss >= 50% of inner TX before loss

trap ci_bench_cleanup EXIT

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn DSCP Scheduler Verification"
echo "  Binary:  $MQVPN"
echo "  Commit:  $CI_BENCH_COMMIT"
echo "  Date:    $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

ci_bench_setup_netns
# Symmetric, generous capacity on both paths — this test isolates
# mask-driven placement, not aggregation under scarcity or RTT tie-breaks
# (see ci_bench_wrr_weight_verify.sh for a proportional-share test).
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
    -days 1 -nodes -subj "/CN=ci-dscp-verify" 2>/dev/null
echo "OK: credentials generated"

read_tx_bytes() {
    ip netns exec "$NS_CLIENT" cat "/sys/class/net/${1}/statistics/tx_bytes" 2>/dev/null || echo 0
}

read_tun_tx_bytes() {
    ip netns exec "$NS_CLIENT" cat "/sys/class/net/${TUN_NAME}/statistics/tx_bytes" 2>/dev/null || echo 0
}

# Both client veths must show real TX before xquic considers path B usable
# (PATH_CHALLENGE/RESPONSE must complete first, and set_path_dscp_mask only
# takes effect once the target path is active) — same reasoning as
# ci_bench_wrr_weight_verify.sh's wait_for_xquic_paths_active.
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
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_CB_WORK_DIR}/server.crt" \
        --key  "${_CB_WORK_DIR}/server.key" \
        --auth-key "$_CB_PSK" \
        --scheduler dscp \
        --cc "$CI_BENCH_CC" \
        --log-level "$LOG_LEVEL" >"${_CB_WORK_DIR}/server.log" 2>&1 &
    _CB_SERVER_PID=$!
    sleep 2
    if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
        echo "ERROR: server died"; cat "${_CB_WORK_DIR}/server.log" >&2; return 1
    fi

    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        --path "$VETH_A0" --path "$VETH_B0" \
        --auth-key "$_CB_PSK" \
        --scheduler dscp \
        --cc "$CI_BENCH_CC" \
        --control-port "$CTRL_PORT" \
        --insecure \
        --log-level "$LOG_LEVEL" >"${_CB_WORK_DIR}/client.log" 2>&1 &
    _CB_CLIENT_PID=$!
    sleep 3
    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: client died"; cat "${_CB_WORK_DIR}/client.log" >&2; return 1
    fi

    ci_bench_wait_tunnel 15
    ctrl_wait
    wait_for_xquic_paths_active
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

ctrl_set_dscp_mask() {
    local iface="$1" mask="$2"
    local resp
    resp=$(ctrl_send "{\"cmd\":\"set_path_dscp_mask\",\"iface\":\"${iface}\",\"dscp_mask\":${mask}}")
    if ! echo "$resp" | grep -q '"ok":true'; then
        echo "  WARN: set_path_dscp_mask ${iface}=${mask} -> ${resp}"
    fi
}

# UDP UL tagged with the given TOS byte (DSCP class << 2). Discards iperf
# output — path share is measured via veth TX byte counters.
_run_tagged_udp_traffic() {
    local duration="$1" bw="$2" tos="$3"
    ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_SERVER_IP" -1 >/dev/null 2>&1 &
    local sp=$!
    sleep 1
    ip netns exec "$NS_CLIENT" iperf3 \
        -c "$TUNNEL_SERVER_IP" -t "$duration" -P 1 -u -b "$bw" -S "$tos" \
        >/dev/null 2>&1 || true
    wait "$sp" 2>/dev/null || true
}

# ── Check 1: class-to-path routing, EF reassigned between path A and B ──

echo ""
echo "================================================================"
echo "  Class-routing runs: EF (DSCP ${EF_DSCP}) reassigned A <-> B"
echo "================================================================"

_start_vpn

declare -A A_DELTA B_DELTA

for round_target in A B; do
    echo ""
    echo "  Round: EF assigned to path ${round_target} only"

    if [ "$round_target" = "A" ]; then
        ctrl_set_dscp_mask "$VETH_A0" "$EF_MASK"
        ctrl_set_dscp_mask "$VETH_B0" 0
    else
        ctrl_set_dscp_mask "$VETH_A0" 0
        ctrl_set_dscp_mask "$VETH_B0" "$EF_MASK"
    fi
    sleep 2   # let xquic propagate the new masks

    a_before=$(read_tx_bytes "$VETH_A0")
    b_before=$(read_tx_bytes "$VETH_B0")

    echo "  sending EF-tagged UDP UL ${UDP_BW} for ${IPERF_DURATION}s ..."
    _run_tagged_udp_traffic "$IPERF_DURATION" "$UDP_BW" "$EF_TOS"

    a_after=$(read_tx_bytes "$VETH_A0")
    b_after=$(read_tx_bytes "$VETH_B0")

    A_DELTA[$round_target]=$((a_after - a_before))
    B_DELTA[$round_target]=$((b_after - b_before))

    echo "  path A TX delta: ${A_DELTA[$round_target]} B"
    echo "  path B TX delta: ${B_DELTA[$round_target]} B"
done

ci_bench_stop_vpn
sleep 1

CLASS_RESULT_JSON=$(python3 -c "
a_to_a = ${A_DELTA[A]}
b_to_a = ${B_DELTA[A]}
a_to_b = ${A_DELTA[B]}
b_to_b = ${B_DELTA[B]}

def share(target, other):
    total = target + other
    return (target / total) if total > 0 else 0.0

share_a = share(a_to_a, b_to_a)   # EF -> A: expect path A dominant
share_b = share(b_to_b, a_to_b)   # EF -> B: expect path B dominant

passed = share_a >= ${CLASS_MIN_PATH_RATIO} and share_b >= ${CLASS_MIN_PATH_RATIO}

import json
print(json.dumps({
    'ef_to_path_a': {'path_a_bytes': a_to_a, 'path_b_bytes': b_to_a, 'assigned_path_share': round(share_a, 3)},
    'ef_to_path_b': {'path_a_bytes': a_to_b, 'path_b_bytes': b_to_b, 'assigned_path_share': round(share_b, 3)},
    'passed': passed,
}))
")

echo ""
CLASS_PASSED=$(python3 -c "import json; print(str(json.loads('''$CLASS_RESULT_JSON''')['passed']).lower())")
if [ "$CLASS_PASSED" = "true" ]; then
    echo "  => PASS  (EF traffic followed its assigned path in both rounds)"
else
    echo "  => FAIL  (EF traffic did not consistently follow its assigned path)"
fi

# ── Check 2: resilience when the assigned path goes dark ──

echo ""
echo "================================================================"
echo "  Resilience run: EF assigned to path A only, then path A black-holed"
echo "================================================================"

_start_vpn
ctrl_set_dscp_mask "$VETH_A0" "$EF_MASK"
ctrl_set_dscp_mask "$VETH_B0" 0
sleep 2

echo "  baseline: EF-tagged UDP UL ${UDP_BW} for $((RESIL_DURATION / 2))s (path A healthy) ..."
inner_before=$(read_tun_tx_bytes)
_run_tagged_udp_traffic "$((RESIL_DURATION / 2))" "$UDP_BW" "$EF_TOS"
inner_mid=$(read_tun_tx_bytes)
BASELINE_DELTA=$((inner_mid - inner_before))

echo "  black-holing path A (100% loss) ..."
ip netns exec "$NS_CLIENT" tc qdisc change dev "$VETH_A0" root netem loss 100% 2>/dev/null || true
ip netns exec "$NS_SERVER" tc qdisc change dev "$VETH_A1" root netem loss 100% 2>/dev/null || true

echo "  under loss: EF-tagged UDP UL ${UDP_BW} for $((RESIL_DURATION / 2))s (path A dead) ..."
_run_tagged_udp_traffic "$((RESIL_DURATION / 2))" "$UDP_BW" "$EF_TOS"
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

echo "  baseline inner TX:   ${BASELINE_DELTA} B"
echo "  under-loss inner TX: ${LOSS_DELTA} B"
RESIL_PASSED=$(python3 -c "import json; print(str(json.loads('''$RESIL_RESULT_JSON''')['passed']).lower())")
RESIL_RATIO=$(python3 -c "import json; print(json.loads('''$RESIL_RESULT_JSON''')['throughput_ratio'])")
if [ "$RESIL_PASSED" = "true" ]; then
    echo "  => PASS  (throughput ratio ${RESIL_RATIO} >= ${RESIL_MIN_THROUGHPUT_RATIO} — EF traffic fell back to path B)"
else
    echo "  => FAIL  (throughput ratio ${RESIL_RATIO} < ${RESIL_MIN_THROUGHPUT_RATIO} — EF traffic stalled instead of falling back)"
fi

# ── JSON output ──

mkdir -p "$CI_BENCH_RESULTS"
OUTPUT_FILE="${CI_BENCH_RESULTS}/dscp_verify_$(date +%Y%m%d_%H%M%S).json"
TIMESTAMP="$(date -Iseconds)"

ALL_PASSED="false"
[ "$CLASS_PASSED" = "true" ] && [ "$RESIL_PASSED" = "true" ] && ALL_PASSED="true"

python3 -c "
import json, sys

out = {
    'test': 'dscp_verify',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'traffic': {'direction': 'udp_ul', 'target_bw': '${UDP_BW}', 'dscp_class': ${EF_DSCP}},
    'class_routing_check': json.loads('''$CLASS_RESULT_JSON'''),
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
