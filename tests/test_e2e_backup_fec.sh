#!/bin/bash
# test_e2e_backup_fec.sh — E2E smoke test for backup_fec scheduler.
#
# Verifies:
#   1. mqvpn server + client both start with Scheduler=backup_fec, connection
#      establishes, ping succeeds.
#   2. Compatibility scenario: client=backup_fec / server=wlb still connects
#      (FEC negotiation just disables silently — see plan Open Items).
#
# Requires: root (TUN + netns). Not added to CI; use perf-weekly.yml for
# automated bench coverage instead.
#
# Run manually:
#   sudo bash tests/test_e2e_backup_fec.sh [path/to/mqvpn]

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
source "${SCRIPT_DIR}/../benchmarks/bench_env_setup.sh"

MQVPN="${1:-${MQVPN}}"
BENCH_LOG_LEVEL="${BENCH_LOG_LEVEL:-info}"

PASS=0
FAIL=0
LOG_DIR="$(mktemp -d)"

trap 'bench_cleanup; rm -rf "$LOG_DIR"' EXIT

run_test() {
    local name="$1"
    shift
    echo ""
    echo "--- Test: $name ---"
    if "$@"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        FAIL=$((FAIL + 1))
    fi
}

assert_ping() {
    local msg="${1:-tunnel reachable}"
    if ip netns exec "$NS_CLIENT" ping -c 3 -W 2 "$TUNNEL_SERVER_IP" >/dev/null 2>&1; then
        echo "  assert_ping OK: $msg"
        return 0
    else
        echo "  assert_ping FAIL: $msg"
        return 1
    fi
}

# --- Test 1: symmetric backup_fec on both sides ---

test_symmetric_backup_fec() {
    local client_log="${LOG_DIR}/symmetric_client.log"
    bench_setup_netns
    BENCH_SCHEDULER="backup_fec"
    bench_start_vpn_server
    bench_start_vpn_client "--path veth-a0 --path veth-b0" "$client_log"

    sleep 2
    assert_ping "symmetric backup_fec ping" || return 1

    # Quick iperf3 sanity check (no loss injected — just confirms data flows)
    if command -v iperf3 >/dev/null; then
        ip netns exec "$NS_SERVER" iperf3 -s -D --pidfile /tmp/iperf3-fec.pid >/dev/null 2>&1 || true
        sleep 1
        if ip netns exec "$NS_CLIENT" iperf3 -c "$TUNNEL_SERVER_IP" -t 5 -R >/dev/null 2>&1; then
            echo "  iperf3 OK"
        else
            echo "  iperf3 failed (non-fatal — connection still verified by ping)"
        fi
        kill "$(cat /tmp/iperf3-fec.pid 2>/dev/null)" 2>/dev/null || true
    fi
    return 0
}

# --- Test 2: compatibility — client=backup_fec, server=wlb ---

test_compat_client_fec_server_wlb() {
    local client_log="${LOG_DIR}/compat_client.log"
    bench_cleanup
    bench_setup_netns

    BENCH_SCHEDULER="wlb"
    bench_start_vpn_server

    BENCH_SCHEDULER="backup_fec"
    bench_start_vpn_client "--path veth-a0 --path veth-b0" "$client_log"

    sleep 2
    assert_ping "compat (client=backup_fec, server=wlb) ping" || return 1
    return 0
}

run_test "symmetric backup_fec" test_symmetric_backup_fec
run_test "compat: client=backup_fec, server=wlb" test_compat_client_fec_server_wlb

echo ""
echo "================================================="
echo " Results: PASS=$PASS  FAIL=$FAIL"
echo "================================================="
[ "$FAIL" -eq 0 ]
