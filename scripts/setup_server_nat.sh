#!/bin/bash
# setup_server_nat.sh — Configure NAT for mqvpn server
# Run on the VPN server after mqvpn-server starts.
#
# Usage: sudo ./setup_server_nat.sh [SUBNET] [IFACE] [SUBNET6]

set -e

SUBNET="${1:-10.0.0.0/24}"
IFACE="${2:-eth0}"
SUBNET6="${3:-}"

echo "Enabling IP forwarding..."
sysctl -w net.ipv4.ip_forward=1

echo "Setting up NAT: $SUBNET → $IFACE"
iptables -t nat -A POSTROUTING -s "$SUBNET" -o "$IFACE" -j MASQUERADE
iptables -A FORWARD -s "$SUBNET" -j ACCEPT
iptables -A FORWARD -d "$SUBNET" -j ACCEPT

if [ -n "$SUBNET6" ]; then
    echo "Enabling IPv6 forwarding..."
    sysctl -w net.ipv6.conf.all.forwarding=1
    echo "Setting up IPv6 FORWARD: $SUBNET6"
    ip6tables -I FORWARD -s "$SUBNET6" -j ACCEPT
    ip6tables -I FORWARD -d "$SUBNET6" -j ACCEPT
fi

echo "NAT configured successfully."
echo ""
echo "To remove these rules later:"
echo "  iptables -t nat -D POSTROUTING -s $SUBNET -o $IFACE -j MASQUERADE"
echo "  iptables -D FORWARD -s $SUBNET -j ACCEPT"
echo "  iptables -D FORWARD -d $SUBNET -j ACCEPT"
if [ -n "$SUBNET6" ]; then
    echo "  ip6tables -D FORWARD -s $SUBNET6 -j ACCEPT"
    echo "  ip6tables -D FORWARD -d $SUBNET6 -j ACCEPT"
fi
