#!/bin/bash
# ci_bench_reinjection.sh — Reinjection control mode benchmark
#
# Measures the throughput impact of each reinjection control mode under
# asymmetric path conditions where reinjection is most beneficial.
#
# Reinjection modes tested:
#   no_reinjection, default, deadline, dgram
#
# Scenarios (asymmetric paths):
#   1. symmetric_baseline  — equal RTT/loss, no netem  (cost of reinjection)
#   2. asym_rtt            — A: 10ms 0%, B: 60ms 0%  (RTT asymmetry, default helps)
#   3. lossy_primary       — A: 10ms 2%, B: 30ms 0%  (primary loss, deadline helps)
#   4. lossy_both          — A: 10ms 1%, B: 40ms 1.5% (loss on both, all modes)
#   5. high_rtt_backup     — A: 10ms 0%, B: 120ms 0.5% (high-RTT backup, dgram shines)
#
# Both paths are always used; scheduler = wlb.
#
# Output: ci_bench_results/reinjection_<timestamp>.json
#
# Usage: sudo ./ci_bench_reinjection.sh [path-to-mqvpn-binary]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="${1:-${MQVPN}}"

DURATION=15
PARALLEL=4
SCHEDULER="wlb"
TUNNEL_TIMEOUT=25

REINJ_MODES=(no_reinjection default deadline dgram)

# ── Scenarios: name, description, netem_a, netem_b ──

SCENARIO_NAMES=(
    symmetric_baseline
    asym_rtt
    lossy_primary
    lossy_both
    high_rtt_backup
)

SCENARIO_DESCS=(
    "Symmetric, no loss (overhead cost)"
    "Asymmetric RTT: A=10ms B=60ms"
    "Lossy primary: A=10ms 2%, B=30ms 0%"
    "Lossy both: A=10ms 1%, B=40ms 1.5%"
    "High-RTT backup: A=10ms B=120ms 0.5%"
)

SCENARIO_NETEM_A=(
    ""
    "delay 10ms rate 100mbit"
    "delay 10ms rate 100mbit loss 2%"
    "delay 10ms rate 100mbit loss 1%"
    "delay 10ms rate 100mbit"
)

SCENARIO_NETEM_B=(
    ""
    "delay 60ms rate 50mbit"
    "delay 30ms rate 80mbit"
    "delay 40ms rate 60mbit loss 1.5%"
    "delay 120ms rate 30mbit loss 0.5%"
)

NUM_SCENARIOS=${#SCENARIO_NAMES[@]}

# ── Preflight ──

ci_bench_check_deps
trap ci_bench_cleanup EXIT

echo "================================================================"
echo "  mqvpn Reinjection Control Benchmark (CI)"
echo "  Binary:    $MQVPN"
echo "  Scheduler: $SCHEDULER"
echo "  Modes:     ${REINJ_MODES[*]}"
echo "  Scenarios: ${NUM_SCENARIOS}"
echo "  iperf3:    -P ${PARALLEL} -t ${DURATION}s"
echo "  Commit:    ${CI_BENCH_COMMIT:0:12}"
echo "  Date:      $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

# ── Custom VPN start helpers (add reinjection flags) ──

# Start server.  Reinjection is negotiated client-side in xquic, but we pass
# the flag to the server as well so both endpoints agree on the capability.
# Usage: _reinj_start_server <mode|no_reinjection>
_reinj_start_server() {
    local mode="$1"
    _CB_WORK_DIR="$(mktemp -d)"
    _CB_PSK="$("$MQVPN" --genkey 2>/dev/null)"

    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "${_CB_WORK_DIR}/server.key" -out "${_CB_WORK_DIR}/server.crt" \
        -days 365 -nodes -subj "/CN=ci-bench-reinj" 2>/dev/null

    local reinj_args=""
    if [ "$mode" != "no_reinjection" ]; then
        reinj_args="--reinjection-control --reinjection-mode ${mode}"
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
        ${reinj_args} &
    _CB_SERVER_PID=$!
    sleep 2

    if ! kill -0 "$_CB_SERVER_PID" 2>/dev/null; then
        echo "ERROR: VPN server died (mode=${mode})"
        return 1
    fi
    echo "  VPN server running (PID $_CB_SERVER_PID, mode=${mode})"
}

# Start client with optional reinjection flags and 2 paths.
# Usage: _reinj_start_client <mode|no_reinjection>
_reinj_start_client() {
    local mode="$1"

    local reinj_args=""
    if [ "$mode" != "no_reinjection" ]; then
        reinj_args="--reinjection-control --reinjection-mode ${mode}"
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
        ${reinj_args} &
    _CB_CLIENT_PID=$!
    sleep 3

    if ! kill -0 "$_CB_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: VPN client died (mode=${mode})"
        return 1
    fi
    echo "  VPN client running (PID $_CB_CLIENT_PID)"
}

# ── Per-scenario results ──
# Key: "<scenario_name>_<mode>"

declare -A RESULTS

for ((i = 0; i < NUM_SCENARIOS; i++)); do
    sname="${SCENARIO_NAMES[$i]}"
    sdesc="${SCENARIO_DESCS[$i]}"
    netem_a="${SCENARIO_NETEM_A[$i]}"
    netem_b="${SCENARIO_NETEM_B[$i]}"

    echo ""
    echo "──────────────────────────────────────────────────────────────"
    echo "  [$((i + 1))/${NUM_SCENARIOS}] ${sdesc}"
    [ -n "$netem_a" ] && echo "  Path A: ${netem_a}"
    [ -n "$netem_b" ] && echo "  Path B: ${netem_b}"
    echo "──────────────────────────────────────────────────────────────"

    for mode in "${REINJ_MODES[@]}"; do
        echo ""
        echo "  => Mode: ${mode}"

        ci_bench_setup_netns

        # Apply netem if defined
        ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_A0" root 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_A1" root 2>/dev/null || true
        ip netns exec "$NS_CLIENT" tc qdisc del dev "$VETH_B0" root 2>/dev/null || true
        ip netns exec "$NS_SERVER" tc qdisc del dev "$VETH_B1" root 2>/dev/null || true

        if [ -n "$netem_a" ]; then
            ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_A0" root netem ${netem_a}
            ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_A1" root netem ${netem_a}
        fi
        if [ -n "$netem_b" ]; then
            ip netns exec "$NS_CLIENT" tc qdisc add dev "$VETH_B0" root netem ${netem_b}
            ip netns exec "$NS_SERVER" tc qdisc add dev "$VETH_B1" root netem ${netem_b}
        fi

        key="${sname}_${mode}"

        if ! _reinj_start_server "$mode"; then
            echo "    SKIP: server failed"
            RESULTS[$key]="0.0"
            ci_bench_stop_vpn; ci_bench_cleanup_stale; continue
        fi

        if ! _reinj_start_client "$mode"; then
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
OUTPUT_FILE="${CI_BENCH_RESULTS}/reinjection_$(date -u '+%Y%m%d_%H%M%S').json"

python3 <<PYEOF
import json

modes = "${REINJ_MODES[*]}".split()
scenario_names = "${SCENARIO_NAMES[*]}".split()
scenario_descs = $(printf '%s\n' "${SCENARIO_DESCS[@]}" | python3 -c "import sys,json; print(json.dumps([l.strip() for l in sys.stdin]))")
scenario_netem_a = $(printf '%s\n' "${SCENARIO_NETEM_A[@]}" | python3 -c "import sys,json; print(json.dumps([l.strip() for l in sys.stdin]))")
scenario_netem_b = $(printf '%s\n' "${SCENARIO_NETEM_B[@]}" | python3 -c "import sys,json; print(json.dumps([l.strip() for l in sys.stdin]))")

raw = {
$(for ((i = 0; i < NUM_SCENARIOS; i++)); do
    sname="${SCENARIO_NAMES[$i]}"
    for mode in "${REINJ_MODES[@]}"; do
        echo "    \"${sname}_${mode}\": ${RESULTS[${sname}_${mode}]:-0.0},"
    done
done)
}

def gain_pct(base, measured):
    if base > 0 and measured > 0:
        return round((measured / base - 1) * 100, 1)
    return None

scenarios = []
for i, name in enumerate(scenario_names):
    no_reinj = raw.get(f"{name}_no_reinjection", 0.0)
    entry = {
        "name":               name,
        "description":        scenario_descs[i],
        "netem_a":            scenario_netem_a[i],
        "netem_b":            scenario_netem_b[i],
        "no_reinjection_mbps": no_reinj,
    }
    for mode in modes:
        if mode == "no_reinjection":
            continue
        mbps = raw.get(f"{name}_{mode}", 0.0)
        entry[f"{mode}_mbps"] = mbps
        entry[f"{mode}_gain_pct"] = gain_pct(no_reinj, mbps)
    scenarios.append(entry)

output = {
    "test":             "reinjection",
    "commit":           "${CI_BENCH_COMMIT}",
    "timestamp":        "${TIMESTAMP}",
    "scheduler":        "${SCHEDULER}",
    "protocol":         "tcp",
    "direction":        "DL",
    "duration_sec":     ${DURATION},
    "parallel_streams": ${PARALLEL},
    "modes":            [m for m in modes if m != "no_reinjection"],
    "scenarios":        scenarios,
}

with open("${OUTPUT_FILE}", "w") as f:
    json.dump(output, f, indent=2)

print(json.dumps(output, indent=2))
PYEOF

# ── Sanity check ──

ci_bench_sanity_check "$OUTPUT_FILE" "reinjection benchmark"

# ── Summary ──

echo ""
echo "================================================================"
echo "  Reinjection Benchmark Results"
echo "================================================================"
printf "  %-22s  %-12s" "scenario" "no_reinj"
for mode in default deadline dgram; do
    printf "  %-14s" "$mode"
done
echo ""
echo "  ────────────────────────────────────────────────────────────"
for ((i = 0; i < NUM_SCENARIOS; i++)); do
    sname="${SCENARIO_NAMES[$i]}"
    printf "  %-22s  %-12s" "$sname" "${RESULTS[${sname}_no_reinjection]:-N/A} Mbps"
    for mode in default deadline dgram; do
        printf "  %-14s" "${RESULTS[${sname}_${mode}]:-N/A} Mbps"
    done
    echo ""
done
echo ""
echo "Result: ${OUTPUT_FILE}"
echo ""
echo "================================================================"
echo "  Reinjection Benchmark DONE"
echo "================================================================"
