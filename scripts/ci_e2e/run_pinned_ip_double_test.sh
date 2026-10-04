#!/bin/bash
# run_pinned_ip_double_test.sh — a second connection for a fixed-IP user must
# supersede the first, not corrupt the session table.
#
# A user with a fixed (pinned) IP maps every connection to the same pool
# offset. When such a user reconnects before the previous tunnel idle-times
# out (router reboot, lost close on a dead WAN), the server used to overwrite
# sessions[off] and increment n_sessions a second time; the old connection's
# later close then skipped its decrement, leaking a client slot until the
# server hit max_clients and rejected everyone. A sanitizer build aborts in
# svr_check_session_invariants.
#
# This drives two client processes that authenticate as the same fixed-IP
# user over two separate paths, while the first tunnel is still up, and checks
# the server stays alive and reports exactly one client.
#
# Topology:
#   vpn-client1 ── veth ── vpn-server ── veth ── vpn-client2
# The server address lives on the server's lo so both clients can reach it.
#
# Usage: sudo ./scripts/ci_e2e/run_pinned_ip_double_test.sh [path-to-mqvpn]
# Requires: root, iproute2, openssl, netcat (nc)

set -eu
source "$(dirname "$0")/sanitizer_check.sh"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MQVPN="${1:-${SCRIPT_DIR}/../../build/mqvpn}"
[ -f "$MQVPN" ] || { echo "error: mqvpn binary not found at $MQVPN"; exit 1; }
MQVPN="$(realpath "$MQVPN")"

WORK_DIR="$(mktemp -d)"
NS_S=vpn-server-pd NS_C1=vpn-client-pd1 NS_C2=vpn-client-pd2
V_S1=veth-pd-s1 V_C1=veth-pd-c1
V_S2=veth-pd-s2 V_C2=veth-pd-c2
SRV=10.90.0.1 FIXED_IP=10.0.0.5 CTRL=9193
SERVER_PID=""
C1_PID=""
C2_PID=""
SANITIZER_FAIL=0

cleanup() {
    [ -n "$C1_PID" ] && { kill "$C1_PID" 2>/dev/null || true; wait "$C1_PID" 2>/dev/null || true; }
    [ -n "$C2_PID" ] && { kill "$C2_PID" 2>/dev/null || true; wait "$C2_PID" 2>/dev/null || true; }
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    sleep 1
    ip netns del "$NS_S"  2>/dev/null || true
    ip netns del "$NS_C1" 2>/dev/null || true
    ip netns del "$NS_C2" 2>/dev/null || true
    ip link del "$V_S1" 2>/dev/null || true
    ip link del "$V_S2" 2>/dev/null || true
    rm -rf "$WORK_DIR"
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: sanitizer errors detected (the session-table invariant aborted the server)"
        exit 1
    fi
}
trap cleanup EXIT

ctrl() {
    # Never let the pipeline's exit status trip `set -e` (nc -w2 may return
    # non-zero); callers read stdout only.
    { (echo "$1"; sleep 0.2) | ip netns exec "$NS_S" nc -w2 127.0.0.1 "$CTRL" 2>/dev/null; } || true
}
n_clients() { ctrl '{"cmd":"get_status"}' | sed -n 's/.*"n_clients":\([0-9][0-9]*\).*/\1/p' | head -1; }
wait_ctrl_ready() {
    local i
    for i in $(seq 1 30); do
        ip netns exec "$NS_S" nc -z 127.0.0.1 "$CTRL" 2>/dev/null && return 0
        sleep 1
    done
    return 1
}

echo "=== setup ==="
ip netns add "$NS_S"; ip netns add "$NS_C1"; ip netns add "$NS_C2"
ip link add "$V_S1" type veth peer name "$V_C1"
ip link set "$V_S1" netns "$NS_S"; ip link set "$V_C1" netns "$NS_C1"
ip link add "$V_S2" type veth peer name "$V_C2"
ip link set "$V_S2" netns "$NS_S"; ip link set "$V_C2" netns "$NS_C2"
ip netns exec "$NS_S"  ip addr add 10.90.0.1/24 dev "$V_S1"
ip netns exec "$NS_C1" ip addr add 10.90.0.2/24 dev "$V_C1"
ip netns exec "$NS_S"  ip addr add 10.91.0.1/24 dev "$V_S2"
ip netns exec "$NS_C2" ip addr add 10.91.0.2/24 dev "$V_C2"
for ns in "$NS_S" "$NS_C1" "$NS_C2"; do ip netns exec "$ns" ip link set lo up; done
ip netns exec "$NS_S" ip link set "$V_S1" up; ip netns exec "$NS_C1" ip link set "$V_C1" up
ip netns exec "$NS_S" ip link set "$V_S2" up; ip netns exec "$NS_C2" ip link set "$V_C2" up
# Client2 reaches the server's 10.90.0.1 via its own link.
ip netns exec "$NS_C2" ip route add 10.90.0.0/24 via 10.91.0.1 dev "$V_C2"
ip netns exec "$NS_S" sysctl -w net.ipv4.ip_forward=1 >/dev/null

PSK=$("$MQVPN" --genkey 2>/dev/null)
ALICE=$("$MQVPN" --genkey 2>/dev/null)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$WORK_DIR/k" -out "$WORK_DIR/c" -days 1 -nodes -subj "/CN=pd" 2>/dev/null

echo "=== start server (control API on 127.0.0.1:$CTRL) ==="
ip netns exec "$NS_S" stdbuf -oL -eL "$MQVPN" --mode server \
    --listen "0.0.0.0:4433" --subnet 10.0.0.0/24 \
    --cert "$WORK_DIR/c" --key "$WORK_DIR/k" \
    --user "bootstrap:$PSK" \
    --control-port "$CTRL" --control-addr 127.0.0.1 \
    --log-level debug >"$WORK_DIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 2
kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server died"; cat "$WORK_DIR/server.log"; exit 1; }

wait_ctrl_ready || { echo "FAIL: control socket not ready"; cat "$WORK_DIR/server.log" | tail -20; exit 1; }
echo "=== add a fixed-IP user via the control API ==="
echo "  add_user resp: $(ctrl "{\"cmd\":\"add_user\",\"name\":\"alice\",\"key\":\"$ALICE\",\"fixed_ip\":\"$FIXED_IP\"}")"

start_client() {  # ns iface var
    ip netns exec "$1" stdbuf -oL -eL "$MQVPN" --mode client \
        --server "$SRV:4433" --path "$2" --auth-key "$ALICE" --insecure \
        --no-reconnect --log-level info >"$WORK_DIR/$3.log" 2>&1 &
    echo $!
}
wait_clients() {  # expected
    local want=$1 i
    for i in $(seq 1 60); do [ "$(n_clients)" = "$want" ] && return 0; sleep 1; done
    return 1
}

echo "=== client 1 (alice) ==="
C1_PID=$(start_client "$NS_C1" "$V_C1" c1)
wait_clients 1 || { echo "FAIL: first tunnel did not establish"; cat "$WORK_DIR/c1.log"; exit 1; }
echo "OK: n_clients=1 after client 1"

echo "=== client 2 (alice again, same fixed IP, first tunnel still up) ==="
C2_PID=$(start_client "$NS_C2" "$V_C2" c2)
# Give the second establishment time to land (and, on the buggy build, to
# corrupt the table / abort the sanitizer server).
sleep 8

if ! kill -0 "$SERVER_PID" 2>/dev/null; then
    echo "FAIL: server exited after the second connection (session-table corruption)"
    cat "$WORK_DIR/server.log" | tail -30
    exit 1
fi
N=$(n_clients)
echo "n_clients after client 2 = ${N:-<no response>}"
if [ "$N" != "1" ]; then
    echo "FAIL: expected exactly 1 client (newest supersedes old); got '${N:-<none>}'"
    grep -iE "session|invariant|clients=" "$WORK_DIR/server.log" | tail -20
    exit 1
fi
echo "OK: server alive, n_clients=1 — the second connection superseded the first"
echo "=== PASS ==="
