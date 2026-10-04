#!/bin/bash
# run_xuser_spoof_test.sh — a shared-key client must not claim a configured
# user's identity via the x-user header.
#
# When the server has a shared global key, a client that authenticates with it
# is recorded as "(global)" and may name itself with x-user. Nothing stopped
# that name from being a configured per-user-key account, so any holder of the
# shared key could send `x-user: alice` and be treated as alice, taking
# alice's fixed IP. The fix rejects an x-user that names a configured user
# (and one with forbidden characters), falling back to "(global)".
#
# This connects an attacker client that authenticates with the shared key and
# sends x-user: alice (alice is a configured user with her own key), then reads
# the connected client's identity from the control API's get_status.
#   before the fix: user "alice"      (spoof succeeded)
#   after  the fix: user "(global)"   (spoof rejected)
#
# Usage: sudo ./scripts/ci_e2e/run_xuser_spoof_test.sh [path-to-mqvpn]
# Requires: root, iproute2, openssl, netcat (nc)

set -eu
source "$(dirname "$0")/sanitizer_check.sh"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MQVPN="${1:-${SCRIPT_DIR}/../../build/mqvpn}"
[ -f "$MQVPN" ] || { echo "error: mqvpn binary not found at $MQVPN"; exit 1; }
MQVPN="$(realpath "$MQVPN")"

WORK_DIR="$(mktemp -d)"
NS_S=vpn-server-xu NS_C=vpn-client-xu
V_S=veth-xu-s V_C=veth-xu-c
SRV=10.92.0.1 CTRL=9194
SERVER_PID=""
C_PID=""
SANITIZER_FAIL=0

cleanup() {
    [ -n "$C_PID" ] && { kill "$C_PID" 2>/dev/null || true; wait "$C_PID" 2>/dev/null || true; }
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    sleep 1
    ip netns del "$NS_S" 2>/dev/null || true
    ip netns del "$NS_C" 2>/dev/null || true
    ip link del "$V_S" 2>/dev/null || true
    rm -rf "$WORK_DIR"
    [ "$SANITIZER_FAIL" -eq 0 ] || { echo "FAIL: sanitizer errors detected"; exit 1; }
}
trap cleanup EXIT

ctrl() { { (echo "$1"; sleep 0.2) | ip netns exec "$NS_S" nc -w2 127.0.0.1 "$CTRL" 2>/dev/null; } || true; }
wait_ctrl_ready() { local i; for i in $(seq 1 30); do ip netns exec "$NS_S" nc -z 127.0.0.1 "$CTRL" 2>/dev/null && return 0; sleep 1; done; return 1; }
status_user() {
    # first clients[].user in get_status
    ctrl '{"cmd":"get_status"}' | sed -n 's/.*"clients":\[{"user":"\([^"]*\)".*/\1/p' | head -1
}
n_clients() { ctrl '{"cmd":"get_status"}' | sed -n 's/.*"n_clients":\([0-9][0-9]*\).*/\1/p' | head -1; }

echo "=== setup ==="
ip netns add "$NS_S"; ip netns add "$NS_C"
ip link add "$V_S" type veth peer name "$V_C"
ip link set "$V_S" netns "$NS_S"; ip link set "$V_C" netns "$NS_C"
ip netns exec "$NS_S" ip addr add 10.92.0.1/24 dev "$V_S"
ip netns exec "$NS_C" ip addr add 10.92.0.2/24 dev "$V_C"
for ns in "$NS_S" "$NS_C"; do ip netns exec "$ns" ip link set lo up; done
ip netns exec "$NS_S" ip link set "$V_S" up; ip netns exec "$NS_C" ip link set "$V_C" up

SHARED=$("$MQVPN" --genkey 2>/dev/null)   # the shared global key the attacker holds
ALICE=$("$MQVPN" --genkey 2>/dev/null)     # alice's own per-user key (attacker does NOT have it)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
    -keyout "$WORK_DIR/k" -out "$WORK_DIR/c" -days 1 -nodes -subj "/CN=xu" 2>/dev/null

echo "=== start server with a shared global key + control API ==="
ip netns exec "$NS_S" stdbuf -oL -eL "$MQVPN" --mode server \
    --listen "0.0.0.0:4433" --subnet 10.0.0.0/24 \
    --cert "$WORK_DIR/c" --key "$WORK_DIR/k" \
    --auth-key "$SHARED" \
    --control-port "$CTRL" --control-addr 127.0.0.1 \
    --log-level debug >"$WORK_DIR/server.log" 2>&1 &
SERVER_PID=$!
sleep 2
kill -0 "$SERVER_PID" 2>/dev/null || { echo "FAIL: server died"; cat "$WORK_DIR/server.log"; exit 1; }
wait_ctrl_ready || { echo "FAIL: control socket not ready"; tail -20 "$WORK_DIR/server.log"; exit 1; }

echo "=== configure a per-user-key account 'alice' with a fixed IP ==="
echo "  add_user resp: $(ctrl "{\"cmd\":\"add_user\",\"name\":\"alice\",\"key\":\"$ALICE\",\"fixed_ip\":\"10.0.0.8\"}")"

echo "=== attacker connects with the SHARED key and sends x-user: alice ==="
# A real attacker crafts the request; the stock client emits x-user from
# [Auth] Username, so a client config with the shared Key and Username=alice
# reproduces it. The attacker never has alice's own key.
cat > "$WORK_DIR/attacker.ini" <<INI
[Server]
Address = $SRV:4433
Insecure = true
[Auth]
Key = $SHARED
Username = alice
INI
ip netns exec "$NS_C" stdbuf -oL -eL "$MQVPN" --mode client \
    --config "$WORK_DIR/attacker.ini" --path "$V_C" \
    --no-reconnect --log-level info >"$WORK_DIR/client.log" 2>&1 &
C_PID=$!

echo "=== wait for the tunnel ==="
ok=0
for i in $(seq 1 60); do [ "$(n_clients)" = "1" ] && { ok=1; break; }; sleep 1; done
[ "$ok" = 1 ] || { echo "FAIL: attacker tunnel did not establish"; tail -20 "$WORK_DIR/client.log"; exit 1; }

USER="$(status_user)"
echo "connected client identity (get_status clients[].user) = '${USER:-<none>}'"
if [ "$USER" = "alice" ]; then
    echo "SPOOFED: the shared-key client was accepted as the configured user 'alice' (bug present)"
    exit 1
fi
if [ "$USER" = "(global)" ]; then
    echo "OK: x-user spoof rejected — the client is '(global)', not 'alice'"
    echo "=== PASS ==="
    exit 0
fi
echo "FAIL: unexpected identity '${USER:-<none>}'"
tail -20 "$WORK_DIR/server.log"
exit 1
