#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# scripts/vps_setup.sh — Run on VPS to start mqvpn server
#
# Undo with scripts/vps_teardown.sh. Every firewall rule added here carries
# the comment "mqvpn-vps-test", which is how the teardown finds them. To have
# the teardown stop an iperf3 server too, start it with
#   iperf3 -s -D -I /run/mqvpn-vps-iperf3.pid
set -e

PORT=${1:-10020}
SUBNET="10.0.0.0/24"
TUN="mqvpn0"
CERT_DIR="/root/mqvpn/certs"
BUILD_DIR="/root/mqvpn/build"
CONF="/root/mqvpn/vps-server.conf"
PIDFILE="/run/mqvpn-vps.pid"
COMMENT="mqvpn-vps-test"

# Generate certs if missing
if [ ! -f "$CERT_DIR/server.crt" ]; then
    mkdir -p "$CERT_DIR"
    (umask 077; openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 \
        -keyout "$CERT_DIR/server.key" -out "$CERT_DIR/server.crt" \
        -days 365 -nodes -subj "/CN=mqvpn-test")
    echo "Generated TLS certificates"
fi

# Add a rule unless an identical one exists: add_rule TABLE -A|-I CHAIN SPEC...
add_rule() {
    local table="$1" op="$2" chain="$3"
    shift 3
    iptables -t "$table" -C "$chain" "$@" -m comment --comment "$COMMENT" 2>/dev/null ||
        iptables -t "$table" "$op" "$chain" "$@" -m comment --comment "$COMMENT"
}

# Enable forwarding + NAT. Tunnel clients may reach the host only through
# the tunnel; MASQUERADE covers every egress except the tunnel itself.
sysctl -w net.ipv4.ip_forward=1
add_rule filter -I INPUT -i "$TUN" -s "$SUBNET" -j ACCEPT
add_rule filter -A INPUT -p udp --dport "$PORT" -j ACCEPT
add_rule nat -A POSTROUTING -s "$SUBNET" ! -o "$TUN" -j MASQUERADE
add_rule filter -I FORWARD -i "$TUN" -s "$SUBNET" -j ACCEPT
add_rule filter -I FORWARD -o "$TUN" -d "$SUBNET" -j ACCEPT

echo "NAT configured: $SUBNET (all egress but $TUN)"

# Generate PSK
PSK=$("$BUILD_DIR/mqvpn" --genkey 2>/dev/null)
echo "Generated PSK: ${PSK}"
echo "Use this key on the client with: --auth-key \"${PSK}\""

# The PSK goes in a 0600 config file, not on the command line, where any
# local user could read it from /proc.
(
    umask 077
    cat > "$CONF" <<EOF
[Interface]
Listen = 0.0.0.0:$PORT
Subnet = $SUBNET
TunName = $TUN
LogLevel = debug

[TLS]
Cert = $CERT_DIR/server.crt
Key = $CERT_DIR/server.key

[Auth]
Key = $PSK
EOF
)

# Start server (foreground; its PID is recorded for vps_teardown.sh)
echo "Starting mqvpn server on port $PORT..."
"$BUILD_DIR/mqvpn" --config "$CONF" > >(tee /root/mqvpn-server.log) 2>&1 &
echo $! > "$PIDFILE"
wait $!
