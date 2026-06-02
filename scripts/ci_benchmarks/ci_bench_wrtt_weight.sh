#!/bin/bash
# ci_bench_wrtt_weight.sh — WRTT weight scheduler E2E benchmark
#
# Four tests on a 4-path veth topology.
# T1 uses UDP DL (server→client) to test bandwidth aggregation via cwnd-block
# spillover: a 600 Mbps target exceeds path A's 300 Mbps cap, forcing WRTT to
# distribute across B/C/D.  T2/T3/T4 use UDP UL (client→server) so the
# client's WRTT + weight settings drive path selection; per-interface TX-byte
# counters on the client veths make weight assertions directly observable.
#
#   T1  Aggregation       UDP DL at 600 Mbps target — exceeds path A's 300 Mbps
#                         cap so WRTT must spill over to B/C/D.
#                         4-path WRTT throughput must be >= 120% of 1-path.
#
#   T2  Weight priority   Weights: A=10 B=C=D=1.  Low target BW (100 Mbps,
#                         well below A's 300 Mbps cap).  Path A must carry
#                         >= 70% of total TX bytes on the client.
#
#   T3  Weight > RTT      Weights: B=10 A=C=D=1.  Low target BW (40 Mbps,
#                         below B's 80 Mbps cap).  Path B has the worst RTT
#                         (60 ms) but must still carry >= 70% of bytes —
#                         proving weight overrides RTT.
#
#   T4  RTT tiebreaker    All weights=1.  Target 100 Mbps (below path A's
#                         300 Mbps cap).  Path A (lowest RTT = 20 ms) must
#                         carry the largest share of TX bytes.
#
# Paths (one-way delay / rate; RTT = 2 × delay):
#   A: 300 Mbps / 10 ms one-way (20 ms RTT)   — fastest, lowest RTT
#   B:  80 Mbps / 30 ms one-way (60 ms RTT)   — slowest, highest RTT
#   C: 200 Mbps / 15 ms one-way (30 ms RTT)
#   D: 150 Mbps / 20 ms one-way (40 ms RTT)
#   Theoretical max (A+B+C+D): 730 Mbps
#
# Output:  ci_bench_results/wrtt_weight_<timestamp>.json
# Usage:   sudo ./ci_bench_wrtt_weight.sh [path-to-mqvpn-binary] [--log-level LEVEL]

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

CTRL_PORT="9287"

# T1: UDP DL at a target that exceeds path A's cap → WRTT must spill to B/C/D
IPERF_AGG_DURATION=20
IPERF_AGG_BW="600M"    # exceeds path A's 300 Mbps cap; forces cwnd-block spillover

# T2/T3/T4: UDP upload, single stream at low bandwidth
IPERF_WEIGHT_DURATION=20
IPERF_WEIGHT_A_BW="100M"   # T2: path A (300 Mbps cap) at 100 Mbps — not saturated
IPERF_WEIGHT_B_BW="40M"    # T3: path B (80 Mbps cap) at 40 Mbps — not saturated
IPERF_RTT_BW="100M"        # T4: equal weights, below path A cap

AGGREGATION_PASS_THRESHOLD=120   # 4-path >= 120% of single-path
WEIGHT_DOMINANT_THRESHOLD=70     # dominant path carries >= 70% of client TX bytes

# ── Extra paths C and D ──

VETH_C0="ci-c0" ; VETH_C1="ci-c1"
VETH_D0="ci-d0" ; VETH_D1="ci-d1"
IP_C_CLIENT="10.201.0.2/24" ; IP_C_SERVER="10.201.0.1/24"
IP_D_CLIENT="10.202.0.2/24" ; IP_D_SERVER="10.202.0.1/24"

_WW_WORK_DIR=""
_WW_PSK=""
_WW_SERVER_PID=""
_WW_CLIENT_PID=""

# ── Cleanup ──

_ww_cleanup() {
    [ -n "$_WW_CLIENT_PID" ] && { kill "$_WW_CLIENT_PID" 2>/dev/null || true; wait "$_WW_CLIENT_PID" 2>/dev/null || true; }
    [ -n "$_WW_SERVER_PID" ] && { kill "$_WW_SERVER_PID" 2>/dev/null || true; wait "$_WW_SERVER_PID" 2>/dev/null || true; }
    _WW_CLIENT_PID="" ; _WW_SERVER_PID=""

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

    [ -n "$_WW_WORK_DIR" ] && rm -rf "$_WW_WORK_DIR" || true
}
trap _ww_cleanup EXIT

# ── Network namespace setup (4 paths) ──

_ww_setup_netns() {
    ci_bench_setup_netns   # creates NS, veth A+B, loopback, rp_filter, lo /32

    # Path C
    ip link add "$VETH_C0" type veth peer name "$VETH_C1"
    ip link set "$VETH_C0" netns "$NS_CLIENT"
    ip link set "$VETH_C1" netns "$NS_SERVER"
    ip netns exec "$NS_CLIENT" ip addr add "$IP_C_CLIENT" dev "$VETH_C0"
    ip netns exec "$NS_SERVER" ip addr add "$IP_C_SERVER" dev "$VETH_C1"
    ip netns exec "$NS_CLIENT" ip link set "$VETH_C0" up
    ip netns exec "$NS_SERVER" ip link set "$VETH_C1" up

    # Path D
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

# ── Netem: A=10ms/300Mbps  B=30ms/80Mbps  C=15ms/200Mbps  D=20ms/150Mbps ──

_ww_apply_netem() {
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
    echo "OK: netem applied (A:300M/20ms B:80M/60ms C:200M/30ms D:150M/40ms)"
}

# ── VPN server ──

_ww_start_server() {
    ip netns exec "$NS_SERVER" "$MQVPN" \
        --mode server \
        --listen "0.0.0.0:${VPN_LISTEN_PORT}" \
        --subnet 10.0.0.0/24 \
        --cert "${_WW_WORK_DIR}/server.crt" \
        --key  "${_WW_WORK_DIR}/server.key" \
        --auth-key "$_WW_PSK" \
        --scheduler wrtt \
        --mtu 1350 \
        --cc "$CI_BENCH_CC" \
        --log-level "$LOG_LEVEL" >"${_WW_WORK_DIR}/server.log" 2>&1 &
    _WW_SERVER_PID=$!
    sleep 2
    if ! kill -0 "$_WW_SERVER_PID" 2>/dev/null; then
        echo "ERROR: server died"; cat "${_WW_WORK_DIR}/server.log" >&2; return 1
    fi
}

# ── VPN client ──

_ww_start_client() {
    local paths_args="$1"

    if [ -n "$_WW_CLIENT_PID" ] && kill -0 "$_WW_CLIENT_PID" 2>/dev/null; then
        kill "$_WW_CLIENT_PID" 2>/dev/null || true
        wait "$_WW_CLIENT_PID" 2>/dev/null || true
        _WW_CLIENT_PID=""
        sleep 1
    fi

    # shellcheck disable=SC2086
    ip netns exec "$NS_CLIENT" "$MQVPN" \
        --mode client \
        --server "${IP_A_SERVER_ADDR}:${VPN_LISTEN_PORT}" \
        $paths_args \
        --auth-key "$_WW_PSK" \
        --scheduler wrtt \
        --mtu 1350 \
        --cc "$CI_BENCH_CC" \
        --control-port "$CTRL_PORT" \
        --insecure \
        --log-level "$LOG_LEVEL" >"${_WW_WORK_DIR}/client.log" 2>&1 &
    _WW_CLIENT_PID=$!
    sleep 3
    if ! kill -0 "$_WW_CLIENT_PID" 2>/dev/null; then
        echo "ERROR: client died"; cat "${_WW_WORK_DIR}/client.log" >&2; return 1
    fi
}

_ww_stop_client() {
    if [ -n "$_WW_CLIENT_PID" ] && kill -0 "$_WW_CLIENT_PID" 2>/dev/null; then
        kill "$_WW_CLIENT_PID" 2>/dev/null || true
        wait "$_WW_CLIENT_PID" 2>/dev/null || true
        _WW_CLIENT_PID=""
        sleep 1
    fi
}

# ── Control socket ──

ctrl_send() {
    printf '%s\n' "$1" \
        | ip netns exec "$NS_CLIENT" timeout 5 nc 127.0.0.1 "$CTRL_PORT" 2>/dev/null \
        || true
}

ctrl_wait() {
    local elapsed=0
    while [ "$elapsed" -lt 15 ]; do
        if ip netns exec "$NS_CLIENT" nc -z 127.0.0.1 "$CTRL_PORT" 2>/dev/null; then
            return 0
        fi
        sleep 1; elapsed=$((elapsed + 1))
    done
    echo "ERROR: control socket not reachable after 15s"; return 1
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
# was sent on each path.  Then sleep 2 s for PATH_CHALLENGE/RESPONSE to
# complete → XQC_PATH_STATE_ACTIVE, required by WRTT and xqc_conn_set_path_weight.
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

# ── Per-interface TX byte counter (client sends in UL direction) ──

read_tx_bytes() {
    ip netns exec "$NS_CLIENT" cat "/sys/class/net/${1}/statistics/tx_bytes" 2>/dev/null || echo 0
}

# ── UDP upload iperf3 (client→server, no -R) ──

run_udp_ul_mbps() {
    local duration="$1" bw_target="$2"
    local jf; jf="$(mktemp)"
    ip netns exec "$NS_SERVER" iperf3 -s -B "$TUNNEL_SERVER_IP" -1 >/dev/null 2>&1 &
    local sp=$!
    sleep 1
    ip netns exec "$NS_CLIENT" iperf3 \
        -c "$TUNNEL_SERVER_IP" -t "$duration" -P 1 -u -b "$bw_target" --json \
        >"$jf" 2>&1 || true
    wait "$sp" 2>/dev/null || true
    python3 -c "
import json
try:
    d = json.load(open('${jf}'))
    e = d.get('end', {})
    s = e.get('sum_sent') or e.get('sum', {})
    print(f\"{s.get('bits_per_second', 0) / 1e6:.1f}\")
except: print('0.0')
"
    rm -f "$jf"
}

# ── T1: UDP DL aggregation (target exceeds single-path cap → WRTT must spill) ──

run_t1_aggregation() {
    echo ""
    echo "================================================================"
    echo "  T1: WRTT 4-path aggregation (UDP DL ${IPERF_AGG_BW}, server→client)"
    echo "================================================================"

    local paths4="--path $VETH_A0 --path $VETH_B0 --path $VETH_C0 --path $VETH_D0"
    local jf

    # 1-path baseline (path A only, capped at 300 Mbps by netem)
    _ww_start_client "--path $VETH_A0"
    ci_bench_wait_tunnel 20
    echo "  1-path (A only) ..."
    jf="$(ci_bench_run_iperf UDP DL "$IPERF_AGG_DURATION" 1 "$IPERF_AGG_BW")"
    MBPS_1P="$(ci_bench_parse_throughput "$jf")"
    rm -f "$jf"
    echo "    => ${MBPS_1P} Mbps"
    _ww_stop_client
    sleep 2

    # 4-path — 600 Mbps target exceeds path A's 300 Mbps cap; WRTT cwnd-block
    # fallback distributes to C (200 Mbps), D (150 Mbps), B (80 Mbps).
    _ww_start_client "$paths4"
    ci_bench_wait_tunnel 20
    echo "  4-path (A+B+C+D, all weights=1) ..."
    jf="$(ci_bench_run_iperf UDP DL "$IPERF_AGG_DURATION" 1 "$IPERF_AGG_BW")"
    MBPS_4P="$(ci_bench_parse_throughput "$jf")"
    rm -f "$jf"
    echo "    => ${MBPS_4P} Mbps"
    _ww_stop_client
    sleep 2

    T1_PASSED=$(python3 -c "
s, f = float('${MBPS_1P}'), float('${MBPS_4P}')
print('true' if s > 0 and f >= s * ${AGGREGATION_PASS_THRESHOLD} / 100.0 else 'false')
")
    T1_GAIN=$(python3 -c "
s, f = float('${MBPS_1P}'), float('${MBPS_4P}')
print(f'{((f/s)-1)*100:.1f}' if s > 0 else 'n/a')
")
    echo "  T1: 1p=${MBPS_1P} Mbps  4p=${MBPS_4P} Mbps  gain=+${T1_GAIN}%  threshold=${AGGREGATION_PASS_THRESHOLD}%  => $([ "$T1_PASSED" = "true" ] && echo PASS || echo FAIL)"
}

# ── Weight distribution test (T2/T3/T4) ──
#
# Args: label  description  dominant_iface  udp_bw_target  [iface=weight ...]
# Measures client veth TX bytes before/after a UDP upload test.
# Returns per-path shares and pass/fail via global _${label}_* vars.

run_weight_dist_test() {
    local label="$1" desc="$2" dominant="$3" bw_target="$4"
    shift 4

    echo ""
    echo "================================================================"
    echo "  ${label}: ${desc}"
    echo "================================================================"

    local paths4="--path $VETH_A0 --path $VETH_B0 --path $VETH_C0 --path $VETH_D0"
    _ww_start_client "$paths4"
    ci_bench_wait_tunnel 20
    ctrl_wait
    wait_for_xquic_paths_active   # ensure all 4 paths are live in xquic before setting weights

    # Apply requested weights
    for kv in "$@"; do
        local iface="${kv%%=*}" weight="${kv##*=}"
        ctrl_set_weight "$iface" "$weight"
        echo "  set ${iface} weight=${weight}"
    done
    sleep 3   # give xquic time to propagate new weights before traffic starts

    # Snapshot client TX bytes before iperf
    local b_a b_b b_c b_d
    b_a=$(read_tx_bytes "$VETH_A0")
    b_b=$(read_tx_bytes "$VETH_B0")
    b_c=$(read_tx_bytes "$VETH_C0")
    b_d=$(read_tx_bytes "$VETH_D0")

    echo "  UDP UL ${bw_target} for ${IPERF_WEIGHT_DURATION}s ..."
    local mbps
    mbps=$(run_udp_ul_mbps "$IPERF_WEIGHT_DURATION" "$bw_target")
    echo "  Throughput: ${mbps} Mbps"

    # Snapshot after
    local a_a a_b a_c a_d
    a_a=$(read_tx_bytes "$VETH_A0")
    a_b=$(read_tx_bytes "$VETH_B0")
    a_c=$(read_tx_bytes "$VETH_C0")
    a_d=$(read_tx_bytes "$VETH_D0")

    _ww_stop_client
    sleep 2

    # Compute byte deltas and dominant share
    local py_result
    py_result=$(python3 -c "
ifaces  = ['$VETH_A0', '$VETH_B0', '$VETH_C0', '$VETH_D0']
before  = [int('$b_a'), int('$b_b'), int('$b_c'), int('$b_d')]
after   = [int('$a_a'), int('$a_b'), int('$a_c'), int('$a_d')]
delta   = [max(0, a - b) for a, b in zip(after, before)]
total   = sum(delta) or 1
dom_idx = ifaces.index('$dominant')
dom_sh  = delta[dom_idx] / total * 100
thresh  = $WEIGHT_DOMINANT_THRESHOLD

for ifc, d in zip(ifaces, delta):
    print(f'tx_{ifc}={d}')
    print(f'share_{ifc}={d/total*100:.1f}')
print(f'dominant_share={dom_sh:.1f}')
print(f'passed={str(dom_sh >= thresh).lower()}')
")

    echo "$py_result" | while IFS='=' read -r k v; do printf '  %s = %s\n' "$k" "$v"; done

    local dom_share passed
    dom_share=$(echo "$py_result" | grep '^dominant_share=' | cut -d= -f2)
    passed=$(echo    "$py_result" | grep '^passed='         | cut -d= -f2)

    echo "  ${label}: dominant=${dominant} share=${dom_share}%  threshold=${WEIGHT_DOMINANT_THRESHOLD}%  => $([ "$passed" = "true" ] && echo PASS || echo FAIL)"

    # Export to globals
    eval "_${label}_PASSED=${passed}"
    eval "_${label}_DOM_SHARE=${dom_share}"
    eval "_${label}_MBPS=${mbps}"

    # Store full result block for JSON generation
    eval "_${label}_RESULT=\$(cat <<'PYBLOCK'
${py_result}
PYBLOCK
)"
}

# ── Main ──

ci_bench_check_deps

echo "================================================================"
echo "  mqvpn WRTT Weight Scheduler E2E Benchmark"
echo "  Binary:  $MQVPN"
echo "  Commit:  $CI_BENCH_COMMIT"
echo "  Date:    $(date '+%Y-%m-%d %H:%M')"
echo "  Paths:"
echo "    A  ci-a0  300 Mbps / 20 ms RTT  (fastest, best RTT)"
echo "    B  ci-b0   80 Mbps / 60 ms RTT  (slowest, worst RTT)"
echo "    C  ci-c0  200 Mbps / 30 ms RTT"
echo "    D  ci-d0  150 Mbps / 40 ms RTT"
echo "  Tests:"
echo "    T1  Aggregation:     UDP DL ${IPERF_AGG_BW} (>path A cap), 4-path >= ${AGGREGATION_PASS_THRESHOLD}% of 1-path"
echo "    T2  Weight priority: UDP UL ${IPERF_WEIGHT_A_BW}, A=10 beats B/C/D (all=1)"
echo "    T3  Weight > RTT:    UDP UL ${IPERF_WEIGHT_B_BW}, B=10 beats A despite worse RTT"
echo "    T4  RTT tiebreaker:  UDP UL ${IPERF_RTT_BW}, all weights=1, A (best RTT) leads"
echo "================================================================"

_ww_setup_netns
_ww_apply_netem

_WW_WORK_DIR="$(mktemp -d)"
_WW_PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "${_WW_WORK_DIR}/server.key" -out "${_WW_WORK_DIR}/server.crt" \
    -days 1 -nodes -subj "/CN=ci-wrtt-weight" 2>/dev/null
echo "OK: credentials generated"

_ww_start_server
echo "OK: server started"

TIMESTAMP="$(date -Iseconds)"

# ── T1: Aggregation ──
MBPS_1P="0.0"; MBPS_4P="0.0"; T1_PASSED="false"; T1_GAIN="n/a"
run_t1_aggregation

# ── T2: Path A (weight=10) dominates ──
_T2_PASSED="false"; _T2_DOM_SHARE="0.0"; _T2_MBPS="0.0"; _T2_RESULT=""
run_weight_dist_test \
    "T2" "Weight priority: A=10 B=C=D=1, target ${IPERF_WEIGHT_A_BW} UDP UL" \
    "$VETH_A0" "$IPERF_WEIGHT_A_BW" \
    "${VETH_A0}=10" "${VETH_B0}=1" "${VETH_C0}=1" "${VETH_D0}=1"

# ── T3: Path B (weight=10, worst RTT) dominates ──
_T3_PASSED="false"; _T3_DOM_SHARE="0.0"; _T3_MBPS="0.0"; _T3_RESULT=""
run_weight_dist_test \
    "T3" "Weight > RTT: B=10 A=C=D=1, target ${IPERF_WEIGHT_B_BW} UDP UL" \
    "$VETH_B0" "$IPERF_WEIGHT_B_BW" \
    "${VETH_A0}=1" "${VETH_B0}=10" "${VETH_C0}=1" "${VETH_D0}=1"

# ── T4: RTT tiebreaker (equal weights, A leads) ──
_T4_PASSED="false"; _T4_DOM_SHARE="0.0"; _T4_MBPS="0.0"; _T4_RESULT=""
run_weight_dist_test \
    "T4" "RTT tiebreaker: all weights=1, A (lowest RTT) leads, target ${IPERF_RTT_BW} UDP UL" \
    "$VETH_A0" "$IPERF_RTT_BW" \
    "${VETH_A0}=1" "${VETH_B0}=1" "${VETH_C0}=1" "${VETH_D0}=1"

# ── Generate JSON ──

OUTPUT_FILE="${CI_BENCH_RESULTS}/wrtt_weight_$(date +%Y%m%d_%H%M%S).json"

_parse_result() {
    # Parse 'key=value' lines from a result block string into a Python dict literal
    local block="$1"
    echo "$block" | python3 -c "
import sys, json
d = {}
for line in sys.stdin:
    line = line.strip()
    if '=' in line:
        k, _, v = line.partition('=')
        try:    d[k] = float(v)
        except: d[k] = v
json.dump(d, sys.stdout)
"
}

T2_JSON=$(_parse_result "$_T2_RESULT")
T3_JSON=$(_parse_result "$_T3_RESULT")
T4_JSON=$(_parse_result "$_T4_RESULT")

python3 <<PYEOF
import json

t2 = ${T2_JSON}
t3 = ${T3_JSON}
t4 = ${T4_JSON}

def share(d, iface):    return d.get(f'share_{iface}')
def passed(d):          return d.get('passed', 'false') == 'true'
def dom_share(d):       return d.get('dominant_share', 0.0)

t1_pass = ('${T1_PASSED}' == 'true')
all_pass = t1_pass and passed(t2) and passed(t3) and passed(t4)

out = {
    'test':      'wrtt_weight',
    'commit':    '${CI_BENCH_COMMIT}',
    'timestamp': '${TIMESTAMP}',
    'scheduler': 'wrtt',
    'netem': {
        'path_a': {'iface': '${VETH_A0}', 'rtt_ms': 20,  'rate_mbit': 300},
        'path_b': {'iface': '${VETH_B0}', 'rtt_ms': 60,  'rate_mbit':  80},
        'path_c': {'iface': '${VETH_C0}', 'rtt_ms': 30,  'rate_mbit': 200},
        'path_d': {'iface': '${VETH_D0}', 'rtt_ms': 40,  'rate_mbit': 150},
    },
    'theoretical_max_mbps': 730,
    'results': {
        't1_aggregation': {
            'description':   'UDP DL ${IPERF_AGG_BW} 4-path vs 1-path (server→client)',
            'iperf_proto':   'udp', 'target_bw': '${IPERF_AGG_BW}',
            'direction':     'dl',
            'weights':       'all=1',
            '1path_mbps':   float('${MBPS_1P}'),
            '4path_mbps':   float('${MBPS_4P}'),
            '4path_gain_pct': float('${T1_GAIN}') if '${T1_GAIN}' != 'n/a' else None,
            'threshold_pct': ${AGGREGATION_PASS_THRESHOLD},
            'passed':        t1_pass,
        },
        't2_weight_priority': {
            'description':   'UDP UL ${IPERF_WEIGHT_A_BW} — high-weight A (w=10) vs B/C/D (w=1)',
            'iperf_proto':   'udp', 'target_bw': '${IPERF_WEIGHT_A_BW}',
            'weights':       'A=10 B=1 C=1 D=1',
            'throughput_mbps': float('${_T2_MBPS}'),
            'dominant_iface': '${VETH_A0}',
            'dominant_share_pct': dom_share(t2),
            'per_path_share_pct': {
                '${VETH_A0}': share(t2, '${VETH_A0}'),
                '${VETH_B0}': share(t2, '${VETH_B0}'),
                '${VETH_C0}': share(t2, '${VETH_C0}'),
                '${VETH_D0}': share(t2, '${VETH_D0}'),
            },
            'threshold_pct': ${WEIGHT_DOMINANT_THRESHOLD},
            'passed':        passed(t2),
        },
        't3_weight_gt_rtt': {
            'description':   'UDP UL ${IPERF_WEIGHT_B_BW} — high-weight B (w=10, worst RTT) vs A (w=1, best RTT)',
            'iperf_proto':   'udp', 'target_bw': '${IPERF_WEIGHT_B_BW}',
            'weights':       'A=1 B=10 C=1 D=1',
            'throughput_mbps': float('${_T3_MBPS}'),
            'dominant_iface': '${VETH_B0}',
            'dominant_share_pct': dom_share(t3),
            'per_path_share_pct': {
                '${VETH_A0}': share(t3, '${VETH_A0}'),
                '${VETH_B0}': share(t3, '${VETH_B0}'),
                '${VETH_C0}': share(t3, '${VETH_C0}'),
                '${VETH_D0}': share(t3, '${VETH_D0}'),
            },
            'threshold_pct': ${WEIGHT_DOMINANT_THRESHOLD},
            'passed':        passed(t3),
        },
        't4_rtt_tiebreaker': {
            'description':   'UDP UL ${IPERF_RTT_BW} — equal weights, lowest-RTT path A leads',
            'iperf_proto':   'udp', 'target_bw': '${IPERF_RTT_BW}',
            'weights':       'all=1',
            'throughput_mbps': float('${_T4_MBPS}'),
            'dominant_iface': '${VETH_A0}',
            'dominant_share_pct': dom_share(t4),
            'per_path_share_pct': {
                '${VETH_A0}': share(t4, '${VETH_A0}'),
                '${VETH_B0}': share(t4, '${VETH_B0}'),
                '${VETH_C0}': share(t4, '${VETH_C0}'),
                '${VETH_D0}': share(t4, '${VETH_D0}'),
            },
            'threshold_pct': ${WEIGHT_DOMINANT_THRESHOLD},
            'passed':        passed(t4),
        },
    },
    'summary': {
        'all_passed':         all_pass,
        't1_aggregation':     t1_pass,
        't2_weight_priority': passed(t2),
        't3_weight_gt_rtt':   passed(t3),
        't4_rtt_tiebreaker':  passed(t4),
    },
}

with open('${OUTPUT_FILE}', 'w') as f:
    json.dump(out, f, indent=2)

print()
print(f"  {'Test':<30}  {'Dominant share':>15}  {'Result':>8}")
print(f"  {'-'*30}  {'-'*15}  {'-'*8}")
for tid, r in out['results'].items():
    dom = r.get('dominant_share_pct', '-')
    dom_s = f"{dom:.1f}%" if isinstance(dom, float) else str(dom)
    status = 'PASS' if r['passed'] else 'FAIL'
    gain = r.get('4path_gain_pct')
    extra = f' (+{gain:.1f}%)' if gain is not None else ''
    print(f"  {tid.upper():<30}  {dom_s+extra:>15}  {status:>8}")
print(f"  {'-'*30}  {'-'*15}  {'-'*8}")
print(f"  {'OVERALL':<30}  {'':>15}  {'PASS' if all_pass else 'FAIL':>8}")
print()
PYEOF

echo "================================================================"
echo "  Result: ${OUTPUT_FILE}"
echo "================================================================"

python3 -c "
import json, sys
with open('${OUTPUT_FILE}') as f:
    d = json.load(f)
if not d['summary']['all_passed']:
    print('CI FAIL: one or more WRTT weight tests failed')
    sys.exit(1)
"
