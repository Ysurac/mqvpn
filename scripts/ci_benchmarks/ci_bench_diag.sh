#!/bin/bash
# Diagnostic: test if B=100 actually dominates A=50 after 20s wait
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/ci_bench_env.sh"

MQVPN="$(realpath "${1:-${MQVPN}}")"
CTRL_PORT="9291"
VETH_C0="ci-c0"; VETH_C1="ci-c1"
VETH_D0="ci-d0"; VETH_D1="ci-d1"
IP_C_CLIENT="10.201.0.2/24"; IP_C_SERVER="10.201.0.1/24"
IP_D_CLIENT="10.202.0.2/24"; IP_D_SERVER="10.202.0.1/24"
WORK=$(mktemp -d)
SRV_PID=""; CLI_PID=""

cleanup() {
    [ -n "$CLI_PID" ] && { kill "$CLI_PID" 2>/dev/null || true; wait "$CLI_PID" 2>/dev/null || true; }
    [ -n "$SRV_PID" ] && { kill "$SRV_PID" 2>/dev/null || true; wait "$SRV_PID" 2>/dev/null || true; }
    ip netns exec "$NS_SERVER" pkill -f iperf3 2>/dev/null || true
    ip netns exec "$NS_CLIENT" pkill -f iperf3 2>/dev/null || true
    sleep 1
    ip netns del "$NS_SERVER" 2>/dev/null || true
    ip netns del "$NS_CLIENT" 2>/dev/null || true
    for lnk in ci-a0 ci-b0 ci-c0 ci-d0; do ip link del "$lnk" 2>/dev/null || true; done
    rm -rf "$WORK"
}
trap cleanup EXIT

ci_bench_setup_netns

ip link add "$VETH_C0" type veth peer name "$VETH_C1"
ip link set "$VETH_C0" netns "$NS_CLIENT"; ip link set "$VETH_C1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_C_CLIENT" dev "$VETH_C0"
ip netns exec "$NS_SERVER" ip addr add "$IP_C_SERVER" dev "$VETH_C1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_C0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_C1" up

ip link add "$VETH_D0" type veth peer name "$VETH_D1"
ip link set "$VETH_D0" netns "$NS_CLIENT"; ip link set "$VETH_D1" netns "$NS_SERVER"
ip netns exec "$NS_CLIENT" ip addr add "$IP_D_CLIENT" dev "$VETH_D0"
ip netns exec "$NS_SERVER" ip addr add "$IP_D_SERVER" dev "$VETH_D1"
ip netns exec "$NS_CLIENT" ip link set "$VETH_D0" up
ip netns exec "$NS_SERVER" ip link set "$VETH_D1" up

for iface in ci-c1 ci-d1; do ip netns exec "$NS_SERVER" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null; done
for iface in ci-c0 ci-d0; do ip netns exec "$NS_CLIENT" sysctl -w "net.ipv4.conf.${iface}.rp_filter=0" >/dev/null; done
ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.201.0.1 dev ci-c0 metric 201
ip netns exec "$NS_CLIENT" ip route add 10.100.0.0/24 via 10.202.0.1 dev ci-d0 metric 202

ci_bench_apply_netem "delay 10ms rate 300mbit" "delay 30ms rate 80mbit"
for pair in "ci-c0:${NS_CLIENT}" "ci-c1:${NS_SERVER}"; do
    dev="${pair%%:*}"; ns="${pair##*:}"
    ip netns exec "$ns" tc qdisc add dev "$dev" root netem delay 15ms rate 200mbit
done
for pair in "ci-d0:${NS_CLIENT}" "ci-d1:${NS_SERVER}"; do
    dev="${pair%%:*}"; ns="${pair##*:}"
    ip netns exec "$ns" tc qdisc add dev "$dev" root netem delay 20ms rate 150mbit
done

PSK=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$WORK/server.key" -out "$WORK/server.crt" \
    -days 1 -nodes -subj "/CN=diag" 2>/dev/null

ip netns exec "$NS_SERVER" "$MQVPN" \
    --mode server --listen "0.0.0.0:4433" --subnet 10.0.0.0/24 \
    --cert "$WORK/server.crt" --key "$WORK/server.key" --auth-key "$PSK" \
    --scheduler wrtt --mtu 1350 --cc bbr2 --log-level info \
    >"$WORK/server.log" 2>&1 &
SRV_PID=$!
sleep 2

ip netns exec "$NS_CLIENT" "$MQVPN" \
    --mode client --server "10.100.0.1:4433" \
    --path ci-a0 --path ci-b0 --path ci-c0 --path ci-d0 \
    --auth-key "$PSK" --scheduler wrtt --mtu 1350 --cc bbr2 \
    --control-port "$CTRL_PORT" --insecure --log-level debug \
    >"$WORK/client.log" 2>&1 &
CLI_PID=$!

echo "Sampling TX bytes every 2s for 20s..."
for t in 2 4 6 8 10 12 14 16 18 20; do
    sleep 2
    printf "  t=%2ds" "$t"
    for dev in ci-a0 ci-b0 ci-c0 ci-d0; do
        txb=$(ip netns exec "$NS_CLIENT" cat "/sys/class/net/${dev}/statistics/tx_bytes" 2>/dev/null || echo 0)
        printf "  %s=%d" "$dev" "$txb"
    done
    echo ""
done

echo ""
echo "=== Setting B=100, A=C=D=50 ==="
ctrl() {
    printf '%s\n' "$1" | ip netns exec "$NS_CLIENT" timeout 3 nc 127.0.0.1 "$CTRL_PORT" 2>/dev/null || echo "ctrl_failed"
}
ctrl '{"cmd":"set_path_weight","iface":"ci-a0","weight":50}'
ctrl '{"cmd":"set_path_weight","iface":"ci-b0","weight":100}'
ctrl '{"cmd":"set_path_weight","iface":"ci-c0","weight":50}'
ctrl '{"cmd":"set_path_weight","iface":"ci-d0","weight":50}'
sleep 3

echo ""
echo "=== TX snapshot before iperf ==="
declare -A pre
for dev in ci-a0 ci-b0 ci-c0 ci-d0; do
    pre[$dev]=$(ip netns exec "$NS_CLIENT" cat "/sys/class/net/${dev}/statistics/tx_bytes" 2>/dev/null || echo 0)
    echo "  $dev: ${pre[$dev]}"
done

echo "Running 10s UDP UL at 20M..."
ip netns exec "$NS_SERVER" iperf3 -s -B 10.0.0.1 -1 >/dev/null 2>&1 &
sleep 1
ip netns exec "$NS_CLIENT" iperf3 -c 10.0.0.1 -t 10 -u -b 20M >/dev/null 2>&1 || true

echo ""
echo "=== TX delta after iperf (B=100 should dominate) ==="
for dev in ci-a0 ci-b0 ci-c0 ci-d0; do
    txb=$(ip netns exec "$NS_CLIENT" cat "/sys/class/net/${dev}/statistics/tx_bytes" 2>/dev/null || echo 0)
    delta=$((txb - ${pre[$dev]}))
    echo "  $dev: delta=$delta"
done

echo ""
echo "=== Client log: path activation / weight messages ==="
grep -iE "path_live|xquic_path|set_path_weight|wrtt|path_state|activate|create_path|weight|WARN|ERROR" \
    "$WORK/client.log" 2>/dev/null | head -80 || echo "(no matching lines)"
echo ""
echo "=== Client log last 20 lines ==="
tail -20 "$WORK/client.log" 2>/dev/null || true
