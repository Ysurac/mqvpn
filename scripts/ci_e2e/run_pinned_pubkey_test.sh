#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# run_pinned_pubkey_test.sh — PinnedPubkey (GHSA-qq6x-5r9f-2w3m) using netns
#
# Two servers in one namespace: the real one (certificate R, PSK K) and an
# on-path impostor (its own self-signed certificate F, another PSK). With
# Insecure alone the client hands K to the impostor (the advisory); with
# R's key pinned it must refuse the impostor before sending anything, even
# with Insecure still set, and still reach the real server with neither a
# CA nor a matching hostname.
#
# Usage: sudo ./run_pinned_pubkey_test.sh [path-to-mqvpn-binary] [--log-level LEVEL]

set -e

source "$(dirname "$0")/sanitizer_check.sh"

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
MQVPN=""
LOG_LEVEL="debug"

while [ $# -gt 0 ]; do
    case "$1" in
        --log-level) LOG_LEVEL="$2"; shift 2 ;;
        *) [ -z "$MQVPN" ] && MQVPN="$1"; shift ;;
    esac
done

MQVPN="${MQVPN:-${SCRIPT_DIR}/../../build/mqvpn}"

if [ ! -f "$MQVPN" ]; then
    echo "error: mqvpn binary not found at $MQVPN"
    echo "Build first: mkdir build && cd build && cmake .. && make"
    exit 1
fi

MQVPN="$(realpath "$MQVPN")"
WORK_DIR="$(mktemp -d)"
NS_S=pin-server
NS_C=pin-client
REAL=192.168.110.2:4433
IMPOSTOR=192.168.110.2:4434

PSK=$("$MQVPN" --genkey 2>/dev/null)
IMPOSTOR_PSK=$("$MQVPN" --genkey 2>/dev/null)

gen_cert() {
    openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "${WORK_DIR}/$1.key" -out "${WORK_DIR}/$1.crt" \
        -days 365 -nodes -subj "/CN=$2" 2>/dev/null
}
pin_of() {
    openssl x509 -in "${WORK_DIR}/$1.crt" -pubkey -noout \
        | openssl pkey -pubin -outform der | openssl dgst -sha256 -binary | openssl enc -base64
}
gen_cert real www.openmptcprouter.vps
gen_cert impostor www.openmptcprouter.vps
PIN_REAL=$(pin_of real)
PIN_IMPOSTOR=$(pin_of impostor)
echo "real pin:     sha256//${PIN_REAL}"
echo "impostor pin: sha256//${PIN_IMPOSTOR}"

SERVER_PID=""
IMPOSTOR_PID=""
CLIENT_PID=""
SANITIZER_FAIL=0

cleanup() {
    echo ""
    echo "Cleaning up..."
    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    stop_and_check_sanitizer "$IMPOSTOR_PID" "impostor" || SANITIZER_FAIL=1
    stop_and_check_sanitizer "$SERVER_PID" "server" || SANITIZER_FAIL=1
    sleep 1
    ip netns del "$NS_S" 2>/dev/null || true
    ip netns del "$NS_C" 2>/dev/null || true
    ip link del veth-pc 2>/dev/null || true
    rm -rf "$WORK_DIR"
    if [ "$SANITIZER_FAIL" -ne 0 ]; then
        echo "FAIL: sanitizer errors detected"
        exit 1
    fi
}
trap cleanup EXIT

dump_logs() {
    for f in "${WORK_DIR}"/*.log; do
        echo ""
        echo "--- $(basename "$f") ---"
        cat "$f"
    done
}
fail() {
    echo "=== FAIL: $* ==="
    dump_logs
    exit 1
}

# start_client LOGNAME ARGS... — client in the background, CLIENT_PID set
start_client() {
    local log="$1"
    shift
    ip netns exec "$NS_C" "$MQVPN" --mode client --auth-key "$PSK" \
        --log-level "$LOG_LEVEL" "$@" > "${WORK_DIR}/${log}.log" 2>&1 &
    CLIENT_PID=$!
}
stop_client() {
    stop_and_check_sanitizer "$CLIENT_PID" "client" || SANITIZER_FAIL=1
    CLIENT_PID=""
    sleep 1
}

ip netns del "$NS_S" 2>/dev/null || true
ip netns del "$NS_C" 2>/dev/null || true
ip link del veth-pc 2>/dev/null || true

echo "=== Setting up network namespaces ==="
ip netns add "$NS_S"
ip netns add "$NS_C"
ip link add veth-pc type veth peer name veth-ps
ip link set veth-pc netns "$NS_C"
ip link set veth-ps netns "$NS_S"
ip netns exec "$NS_C" ip addr add 192.168.110.1/24 dev veth-pc
ip netns exec "$NS_S" ip addr add 192.168.110.2/24 dev veth-ps
ip netns exec "$NS_C" ip link set veth-pc up
ip netns exec "$NS_S" ip link set veth-ps up
ip netns exec "$NS_C" ip link set lo up
ip netns exec "$NS_S" ip link set lo up
ip netns exec "$NS_C" ping -c 1 -W 1 192.168.110.2 >/dev/null

echo "=== Starting the real server and the impostor ==="
ip netns exec "$NS_S" "$MQVPN" --mode server --listen "$REAL" --subnet 10.0.0.0/24 \
    --tun-name mqvpn0 --cert "${WORK_DIR}/real.crt" --key "${WORK_DIR}/real.key" \
    --auth-key "$PSK" --log-level "$LOG_LEVEL" > "${WORK_DIR}/server.log" 2>&1 &
SERVER_PID=$!
ip netns exec "$NS_S" "$MQVPN" --mode server --listen "$IMPOSTOR" --subnet 10.1.0.0/24 \
    --tun-name mqvpn1 --cert "${WORK_DIR}/impostor.crt" --key "${WORK_DIR}/impostor.key" \
    --auth-key "$IMPOSTOR_PSK" --log-level "$LOG_LEVEL" > "${WORK_DIR}/impostor.log" 2>&1 &
IMPOSTOR_PID=$!
sleep 2
kill -0 "$SERVER_PID" 2>/dev/null || fail "server died"
kill -0 "$IMPOSTOR_PID" 2>/dev/null || fail "impostor died"

impostor_tokens() { grep -c "invalid or missing PSK" "${WORK_DIR}/impostor.log" || true; }

echo ""
echo "=== Test 1: pinned key reaches the real server (no Insecure, no CA) ==="
start_client pin_ok --server "$REAL" --pinned-pubkey "sha256//${PIN_REAL}"
sleep 3
kill -0 "$CLIENT_PID" 2>/dev/null || fail "pinned client died"
ip netns exec "$NS_C" ping -c 3 -W 2 10.0.0.1 >/dev/null || fail "no tunnel with the right pin"
grep -q "server public key pinned (1 pin)" "${WORK_DIR}/pin_ok.log" ||
    fail "pin not reported at startup"
echo "PASS: tunnel up with the pinned key"
stop_client

echo ""
echo "=== Test 2 (control): Insecure alone leaks the PSK to the impostor ==="
start_client leak --server "$IMPOSTOR" --insecure
sleep 3
stop_client
[ "$(impostor_tokens)" -gt 0 ] || fail "control: impostor received no token, the test cannot see a leak"
echo "PASS: impostor received the token ($(impostor_tokens) attempt(s)), as in the advisory"
LEAKS_BEFORE=$(impostor_tokens)

echo ""
echo "=== Test 3: a pin refuses the impostor before authentication, Insecure or not ==="
start_client pin_bad --server "$IMPOSTOR" --insecure \
    --pinned-pubkey "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=; sha256//${PIN_REAL}"
sleep 4
grep -q "does not match PinnedPubkey (server sha256//${PIN_IMPOSTOR})" "${WORK_DIR}/pin_bad.log" ||
    fail "pin mismatch not logged"
grep -q "Insecure ignored, PinnedPubkey is enforced" "${WORK_DIR}/pin_bad.log" ||
    fail "Insecure override not logged"
if grep -q "tunnel 200 OK" "${WORK_DIR}/pin_bad.log"; then fail "tunnel established with the impostor"; fi
[ "$(impostor_tokens)" -eq "$LEAKS_BEFORE" ] || fail "impostor received the PSK despite the pin"
echo "PASS: impostor refused, no token sent"
stop_client

echo ""
echo "=== Test 4: a malformed pin is a startup error ==="
set +e
ip netns exec "$NS_C" timeout 10 "$MQVPN" --mode client --server "$REAL" --auth-key "$PSK" \
    --pinned-pubkey "not-a-pin" > "${WORK_DIR}/pin_syntax.log" 2>&1
RC=$?
set -e
[ "$RC" -ne 0 ] && [ "$RC" -ne 124 ] || fail "malformed pin accepted (rc=$RC)"
grep -q "invalid PinnedPubkey" "${WORK_DIR}/pin_syntax.log" || fail "malformed pin not reported"
echo "PASS: rejected at startup (rc=$RC)"

echo ""
echo "=== ALL PINNED PUBKEY TESTS PASSED ==="
