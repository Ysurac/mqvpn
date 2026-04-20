#!/bin/bash
# ci_bench_fec.sh — FEC scheme throughput benchmark under packet loss
#
# Measures the throughput impact and recovery gain of each FEC scheme
# compared to a no-FEC baseline across four loss levels.
#
# FEC schemes tested:
#   reed_solomon, xor, packet_mask, galois_calculation
#
# Loss scenarios (applied symmetrically on both paths):
#   1. baseline      — 0%   loss, no netem  (FEC overhead cost)
#   2. low_loss      — 0.5% loss
#   3. medium_loss   — 1.5% loss
#   4. high_loss     — 3%   loss
#
# For each scenario, one run per FEC scheme plus a no-FEC reference run.
# Multipath (2 paths), scheduler: wlb.
#
# Output: ci_bench_results/fec_<timestamp>.json
#
# Usage: sudo ./ci_bench_fec.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"

DURATION=15
PARALLEL=4
SCHEDULER="wlb"
TUNNEL_TIMEOUT=20

FEC_SCHEMES=(no_fec reed_solomon xor packet_mask galois_calculation)

# ── Loss scenarios: name, loss%, description ──

SCENARIO_NAMES=(baseline low_loss medium_loss high_loss)
SCENARIO_DESCS=(
    "No loss (overhead cost)"
    "Low loss (0.5%)"
    "Medium loss (1.5%)"
    "High loss (3%)"
)
SCENARIO_LOSS=(0.0 0.5 1.5 3.0)

NUM_SCENARIOS=${#SCENARIO_NAMES[@]}

# ── Preflight ──

ci_bench_check_deps
trap ci_bench_cleanup EXIT

echo "================================================================"
echo "  mqvpn FEC Scheme Benchmark (CI)"
echo "  Binary:    $MQVPN"
echo "  Scheduler: $SCHEDULER"
echo "  Schemes:   ${FEC_SCHEMES[*]}"
echo "  Scenarios: ${NUM_SCENARIOS}"
echo "  iperf3:    -P ${PARALLEL} -t ${DURATION}s"
echo "  Commit:    ${CI_BENCH_COMMIT:0:12}"
echo "  Date:      $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

# ── Custom VPN start helpers (add FEC flags) ──

# Start server with optional FEC flags.
# Usage: _fec_start_server <fec_scheme|no_fec>
_fec_start_server() {
    local scheme="$1"
    _CB_WORK_DIR="$(mktemp -d)"
    _CB_PSK="$("$MQVPN" --genkey 2>/dev/null)"

    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "${_CB_WORK_DIR}/server.key" -out "${_CB_WORK_DIR}/server.crt" \
        -days 365 -nodes -subj "/CN=ci-bench-fec" 2>/dev/null

    local fec_args=""
    if [ "$scheme" != "no_fec" ]; then
        fec_args="--fec-enable --fec-scheme ${scheme}"
    fi

    # shellcheck disable=SC2086
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_CB_WORK_DIR}/server.crt" \
        --key  "${_CB_WORK_DIR}/server.key" \
        --auth-key "$_CB_PSK" \
        --scheduler "$SCHEDULER" \
        --log-level "$CI_BENCH_LOG_LEVEL" \
        ${fec_args} &
    _CB_SERVER_PID=$!
    sleep 2

    if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
        echo "ERROR: VPN server died (scheme=${scheme})"
        return 1
    fi
    echo "  VPN server running (PID $_CB_SERVER_PID, scheme=${scheme})"
}

# Start client with optional FEC flags and 2 paths.
# Usage: _fec_start_client <fec_scheme|no_fec>
_fec_start_client() {
    local scheme="$1"

    local fec_args=""
    if [ "$scheme" != "no_fec" ]; then
        fec_args="--fec-enable --fec-scheme ${scheme}"
    fi

    # shellcheck disable=SC2086
    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        --path "$VETH_A0" --path "$VETH_B0" \
        --auth-key "$_CB_PSK" \
        --scheduler "$SCHEDULER" \
        --insecure \
        --log-level "$CI_BENCH_LOG_LEVEL" \
        ${fec_args} &
    _CB_CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: VPN client died (scheme=${scheme})"
        return 1
    fi
    echo "  VPN client running (PID $_CB_CLIENT_PID)"
}

# ── Per-scenario results ──
# Key: "<scenario_name>_<scheme>"

declare -A RESULTS

for ((i = 0; i < NUM_SCENARIOS; i++)); do
    sname="${SCENARIO_NAMES[$i]}"
    sdesc="${SCENARIO_DESCS[$i]}"
    sloss="${SCENARIO_LOSS[$i]}"

    echo ""
    echo "──────────────────────────────────────────────────────────────"
    echo "  [$((i + 1))/${NUM_SCENARIOS}] ${sdesc}"
    echo "──────────────────────────────────────────────────────────────"

    for scheme in "${FEC_SCHEMES[@]}"; do
        echo ""
        echo "  => Scheme: ${scheme}"

        ci_bench_setup_netns

        # Apply loss netem (symmetric on both paths, 50mbit each)
        ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_A0" root 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_A1" root 2>/dev/null || true
        ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_B0" root 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_B1" root 2>/dev/null || true

        if [ "$(echo "$sloss > 0" | bc -l)" = "1" ]; then
            local_netem="delay 10ms rate 100mbit loss ${sloss}%"
            ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_A0" root netem ${local_netem}
            ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_A1" root netem ${local_netem}
            ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_B0" root netem ${local_netem}
            ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_B1" root netem ${local_netem}
        fi

        key="${sname}_${scheme}"

        if ! _fec_start_server "$scheme"; then
            echo "    SKIP: server failed"
            RESULTS[$key]="0.0"
            ci_bench_stop_vpn; ci_bench_cleanup_stale; continue
        fi

        if ! _fec_start_client "$scheme"; then
            echo "    SKIP: client failed"
            RESULTS[$key]="0.0"
            ci_bench_stop_vpn; ci_bench_cleanup_stale; continue
        fi

        if ! ci_bench_wait_tunnel "$TUNNEL_TIMEOUT"; then
            echo "    SKIP: tunnel not established"
            RESULTS[$key]="0.0"
            ci_bench_stop_vpn; ci_bench_cleanup_stale; continue
        fi

        iperf_json=$(ci_bench_run_iperf TCP DL "$DURATION" "$PARALLEL")
        mbps=$(ci_bench_parse_throughput "$iperf_json")
        rm -f "$iperf_json"
        echo "    Throughput: ${mbps} Mbps"
        RESULTS[$key]="$mbps"

        ci_bench_stop_vpn
        ci_bench_cleanup_stale
    done
done

# ── Generate JSON output ──

echo ""
echo "Generating JSON output..."

TIMESTAMP="$(date -u '+%Y-%m-%dT%H:%M:%SZ')"
OUTPUT_FILE="${CI_BENCH_RESULTS}/fec_$(date -u '+%Y%m%d_%H%M%S').json"

python3 <<PYEOF
import json

schemes = "${FEC_SCHEMES[*]}".split()
scenario_names = "${SCENARIO_NAMES[*]}".split()
scenario_descs = $(printf '%s\n' "${SCENARIO_DESCS[@]}" | python3 -c "import sys,json; print(json.dumps([l.strip() for l in sys.stdin]))")
scenario_losses = [float(x) for x in "${SCENARIO_LOSS[*]}".split()]

raw = {
$(for ((i = 0; i < NUM_SCENARIOS; i++)); do
    sname="${SCENARIO_NAMES[$i]}"
    for scheme in "${FEC_SCHEMES[@]}"; do
        echo "    \"${sname}_${scheme}\": ${RESULTS[${sname}_${scheme}]:-0.0},"
    done
done)
}

def gain_pct(base, measured):
    if base > 0 and measured > 0:
        return round((measured / base - 1) * 100, 1)
    return None

scenarios = []
for i, name in enumerate(scenario_names):
    entry = {
        "name":        name,
        "description": scenario_descs[i],
        "loss_pct":    scenario_losses[i],
    }
    no_fec_mbps = raw.get(f"{name}_no_fec", 0.0)
    entry["no_fec_mbps"] = no_fec_mbps
    for scheme in schemes:
        if scheme == "no_fec":
            continue
        key = f"{name}_{scheme}"
        mbps = raw.get(key, 0.0)
        entry[f"{scheme}_mbps"] = mbps
        entry[f"{scheme}_gain_pct"] = gain_pct(no_fec_mbps, mbps)
    scenarios.append(entry)

output = {
    "test":        "fec",
    "commit":      "${CI_BENCH_COMMIT}",
    "timestamp":   "${TIMESTAMP}",
    "scheduler":   "${SCHEDULER}",
    "protocol":    "tcp",
    "direction":   "DL",
    "duration_sec":     ${DURATION},
    "parallel_streams": ${PARALLEL},
    "schemes":     [s for s in schemes if s != "no_fec"],
    "scenarios":   scenarios,
}

with open("${OUTPUT_FILE}", "w") as f:
    json.dump(output, f, indent=2)

print(json.dumps(output, indent=2))
PYEOF

# ── Sanity check ──

ci_bench_sanity_check "$OUTPUT_FILE" "FEC benchmark"

# ── Summary ──

echo ""
echo "================================================================"
echo "  FEC Benchmark Results"
echo "================================================================"
printf "  %-18s  %-9s" "scenario" "no_fec"
for scheme in reed_solomon xor packet_mask galois_calculation; do
    printf "  %-16s" "$scheme"
done
echo ""
echo "  ──────────────────────────────────────────────────────────────"
for ((i = 0; i < NUM_SCENARIOS; i++)); do
    sname="${SCENARIO_NAMES[$i]}"
    printf "  %-18s  %-9s" "$sname" "${RESULTS[${sname}_no_fec]:-N/A} Mbps"
    for scheme in reed_solomon xor packet_mask galois_calculation; do
        printf "  %-16s" "${RESULTS[${sname}_${scheme}]:-N/A} Mbps"
    done
    echo ""
done
echo ""
echo "Result: ${OUTPUT_FILE}"
echo ""
echo "================================================================"
echo "  FEC Benchmark DONE"
echo "================================================================"
