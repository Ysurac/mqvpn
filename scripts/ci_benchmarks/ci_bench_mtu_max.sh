#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# ci_bench_mtu_max.sh — Max usable MTU / peak throughput finder
#
# Single-path, no netem (native veth speed) benchmark that finds the real
# ceiling on tunnel MTU and the throughput it delivers.
#
# --mtu on the client is only a *cap*: mqvpn negotiates the largest MTU the
# path actually supports and, if --mtu requests more than that, silently
# downgrades to the negotiated value (logged as a WRN, not a failure). So
# this script doesn't just sweep the requested --mtu — for every run it
# reads back the client TUN device's real MTU ("effective_mtu") and uses
# that to find the true ceiling, plus one baseline run with no --mtu at all
# (auto-negotiated) which is the most representative "max MTU" answer.
#
# Output: ci_bench_results/mtu_max_<timestamp>.json
# Usage:  sudo ./ci_bench_mtu_max.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"

DURATION=8
PARALLEL=1
SCHEDULER="wlb"
CC="bbr2"
TUN_NAME="mqvpn0"
# "auto" = no --mtu flag (let mqvpn negotiate); the rest probe requested
# values from below the expected ceiling up through well above it.
MTU_REQUESTS=(auto 1280 1320 1360 1380 1400 1420 1460 1500)

trap ci_bench_cleanup EXIT

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn Max MTU / Peak Throughput Benchmark (CI)"
echo "  Binary:      $MQVPN"
echo "  MTU requests: ${MTU_REQUESTS[*]}"
echo "  Scheduler:   $SCHEDULER   CC: $CC"
echo "  Commit:      $CI_BENCH_COMMIT"
echo "  Date:        $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

ci_bench_setup_netns

# Raise veth MTU to 9000 so wire-level QUIC datagrams (TUN MTU + ~100 B
# QUIC/MASQUE overhead) never get truncated by the link itself — any
# throughput collapse must come from mqvpn/xquic's own MTU negotiation,
# not from the test harness's own veth links.
ip netns exec "$NS_CLIENT" ip link set "$VETH_A0" mtu 9000
ip netns exec "$NS_SERVER" ip link set "$VETH_A1" mtu 9000

# Cert/PSK are reused across all iterations.
_CB_WORK_DIR="$(mktemp -d)"
_CB_PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${_CB_WORK_DIR}/server.key" -out "${_CB_WORK_DIR}/server.crt" \
    -days 365 -nodes -subj "/CN=ci-bench-mtu" 2>/dev/null

RESULTS_TMP="$(mktemp)"

for MTU in "${MTU_REQUESTS[@]}"; do
    echo ""
    echo "==> Requested MTU: $MTU"

    MTU_ARGS=()
    [ "$MTU" != "auto" ] && MTU_ARGS=(--mtu "$MTU")

    # ci_bench_start_server/client don't take --mtu; start mqvpn directly.
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_CB_WORK_DIR}/server.crt" \
        --key  "${_CB_WORK_DIR}/server.key" \
        --auth-key "$_CB_PSK" \
        --scheduler "$SCHEDULER" \
        --cc "$CC" \
        "${MTU_ARGS[@]}" \
        --log-level "$CI_BENCH_LOG_LEVEL" &
    _CB_SERVER_PID=$!
    sleep 2

    if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
        echo "    ERROR: VPN server died at MTU=$MTU"
        echo "$MTU 0 0.0 SERVER_DIED" >> "$RESULTS_TMP"
        continue
    fi

    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        --path "$VETH_A0" \
        --auth-key "$_CB_PSK" \
        --scheduler "$SCHEDULER" \
        --cc "$CC" \
        "${MTU_ARGS[@]}" \
        --insecure \
        --log-level "$CI_BENCH_LOG_LEVEL" &
    _CB_CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null || ! ci_bench_wait_tunnel 10; then
        echo "    MTU=$MTU: tunnel did not come up"
        echo "$MTU 0 0.0 NO_TUNNEL" >> "$RESULTS_TMP"
        ci_bench_stop_vpn
        continue
    fi

    EFFECTIVE_MTU="$(ip netns exec "$NS_CLIENT" cat "/sys/class/net/${TUN_NAME}/mtu")"
    echo "    effective client TUN MTU: $EFFECTIVE_MTU"

    JSON_FILE="$(ci_bench_run_iperf TCP DL "$DURATION" "$PARALLEL")"
    MBPS="$(ci_bench_parse_throughput "$JSON_FILE")"
    rm -f "$JSON_FILE"
    echo "    throughput: ${MBPS} Mbps"
    echo "$MTU $EFFECTIVE_MTU $MBPS OK" >> "$RESULTS_TMP"

    ci_bench_stop_vpn
    sleep 1
done

rm -rf "$_CB_WORK_DIR"
_CB_WORK_DIR=""

# ── Generate output JSON ──

TIMESTAMP="$(date -Iseconds)"
OUTPUT_FILE="${CI_BENCH_RESULTS}/mtu_max_$(date +%Y%m%d_%H%M%S).json"

python3 <<PYEOF
import json

points = []
with open('${RESULTS_TMP}') as f:
    for line in f:
        parts = line.strip().split()
        if len(parts) != 4:
            continue
        requested, effective, mbps, status = parts
        points.append({
            'requested_mtu': requested,
            'effective_mtu': int(effective),
            'throughput_mbps': float(mbps),
            'status': status,
        })

working = [p for p in points if p['status'] == 'OK' and p['throughput_mbps'] > 1.0]
auto_point = next((p for p in points if p['requested_mtu'] == 'auto'), None)
max_effective_mtu = max((p['effective_mtu'] for p in working), default=None)
best_point = max(working, key=lambda p: p['throughput_mbps'], default=None)

output = {
    'test': 'mtu_max',
    'commit': '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'scheduler': '${SCHEDULER}',
    'cc': '${CC}',
    'duration_s': ${DURATION},
    'note': (
        "--mtu only caps the client TUN MTU; mqvpn negotiates the largest "
        "MTU the path actually supports and silently downgrades any --mtu "
        "request above that (logged as a WRN). effective_mtu is what was "
        "actually configured on the client TUN device for each run."
    ),
    'points': points,
    'auto_negotiated_mtu': auto_point['effective_mtu'] if auto_point else None,
    'auto_negotiated_throughput_mbps': auto_point['throughput_mbps'] if auto_point else None,
    'max_effective_mtu': max_effective_mtu,
    'best_effective_mtu': best_point['effective_mtu'] if best_point else None,
    'best_throughput_mbps': best_point['throughput_mbps'] if best_point else None,
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(output, f, indent=2)

print()
print(f"  {'requested':>10}  {'effective':>10}  {'Mbps':>10}  {'status':>12}")
print(f"  {'-'*10}  {'-'*10}  {'-'*10}  {'-'*12}")
for p in points:
    print(f"  {p['requested_mtu']:>10}  {p['effective_mtu']:>10}  {p['throughput_mbps']:>8.1f} M  {p['status']:>12}")
print()
print(f"  auto-negotiated MTU  = {output['auto_negotiated_mtu']} ({output['auto_negotiated_throughput_mbps']} Mbps)")
print(f"  max effective MTU    = {max_effective_mtu}")
print(f"  best MTU/throughput  = {output['best_effective_mtu']} ({output['best_throughput_mbps']} Mbps)")
PYEOF

rm -f "$RESULTS_TMP"

ci_bench_sanity_check "$OUTPUT_FILE" "mtu_max benchmark"

echo ""
echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"

# CI fails only if nothing worked at all (i.e. mqvpn is broken).
python3 -c "
import json, sys
with open('${OUTPUT_FILE}') as f:
    data = json.load(f)
if data['max_effective_mtu'] is None:
    print('CI FAIL: no MTU value produced working throughput')
    sys.exit(1)
"
