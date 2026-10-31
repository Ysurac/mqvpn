#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# mqvpn-server-nat.sh — NAT setup/teardown helper for mqvpn-server.service
#
# Usage:
#   mqvpn-server-nat.sh setup   CONFIG_PATH
#   mqvpn-server-nat.sh teardown CONFIG_PATH
#
# Reads Subnet/Subnet6 from the config file and manages:
#   - net.ipv4.ip_forward = 1 (+ net.ipv6.conf.all.forwarding if Subnet6 set)
#   - iptables MASQUERADE + FORWARD rules
#   - ip6tables FORWARD + NAT66 (MASQUERADE) rules
#
# Teardown removes the rules but leaves forwarding enabled: other services on
# the host (routers, containers, other VPNs) may rely on it, and turning it
# back off on every stop or restart would cut them off.

set -e

ACTION="$1"
CONFIG="$2"

if [ -z "$ACTION" ] || [ -z "$CONFIG" ]; then
    echo "Usage: $0 setup|teardown CONFIG_PATH" >&2
    exit 1
fi

COMMENT="mqvpn-server-nat"
STATE_DIR="/run/mqvpn"

setup() {
    if [ ! -f "$CONFIG" ]; then
        echo "Error: config file not found: $CONFIG" >&2
        exit 1
    fi

    # Capture the status before mapfile (process substitution would hide it).
    local nat_config
    nat_config=$("$(dirname "$0")/mqvpn-nat-config" "$CONFIG")
    local -a nat_values
    mapfile -t nat_values <<< "$nat_config"
    local SUBNET="${nat_values[0]}" SUBNET6="${nat_values[1]}" TUN_NAME="${nat_values[2]:-mqvpn0}"

    sysctl -w net.ipv4.ip_forward=1 >/dev/null

    # MASQUERADE by source subnet on every egress except the tunnel itself,
    # not on the interface that held the default route at start: that one
    # goes stale when the route moves (multi-WAN, failover, DHCP), and it
    # left client traffic to any other uplink un-NATed.
    echo "mqvpn-server-nat: setup subnet=$SUBNET tun=$TUN_NAME"

    iptables -t nat -A POSTROUTING -s "$SUBNET" ! -o "$TUN_NAME" -j MASQUERADE \
        -m comment --comment "$COMMENT"
    iptables -I FORWARD -i "$TUN_NAME" -s "$SUBNET" -j ACCEPT \
        -m comment --comment "$COMMENT"
    iptables -I FORWARD -o "$TUN_NAME" -d "$SUBNET" -j ACCEPT \
        -m comment --comment "$COMMENT"

    # IPv6 forwarding + FORWARD + NAT66 rules
    if [ -n "$SUBNET6" ]; then
        sysctl -w net.ipv6.conf.all.forwarding=1 >/dev/null
        ip6tables -I FORWARD -i "$TUN_NAME" -s "$SUBNET6" -j ACCEPT \
            -m comment --comment "$COMMENT"
        ip6tables -I FORWARD -o "$TUN_NAME" -d "$SUBNET6" -j ACCEPT \
            -m comment --comment "$COMMENT"
        ip6tables -t nat -A POSTROUTING -s "$SUBNET6" ! -o "$TUN_NAME" -j MASQUERADE \
            -m comment --comment "$COMMENT"
        echo "mqvpn-server-nat: IPv6 FORWARD + NAT66 rules added for $SUBNET6"
    fi
}

teardown() {
    echo "mqvpn-server-nat: teardown"

    # Remove rules by line number (reverse order to avoid index shift)
    # This handles any rule shape as long as it has our comment tag
    delete_by_comment() {
        local table="$1" chain="$2"
        while true; do
            local line
            line=$(iptables -t "$table" -L "$chain" --line-numbers -n 2>/dev/null \
                | grep "$COMMENT" | head -1 | awk '{print $1}')
            [ -z "$line" ] && break
            iptables -t "$table" -D "$chain" "$line" 2>/dev/null || break
        done
    }

    delete_by_comment nat POSTROUTING
    delete_by_comment filter FORWARD

    # Remove IPv6 FORWARD rules (unconditional — handles config changes between setup/teardown)
    delete_by_comment_v6() {
        local table="$1" chain="$2"
        while true; do
            local line
            line=$(ip6tables -t "$table" -L "$chain" --line-numbers -n 2>/dev/null \
                | grep "$COMMENT" | head -1 | awk '{print $1}')
            [ -z "$line" ] && break
            ip6tables -t "$table" -D "$chain" "$line" 2>/dev/null || break
        done
    }
    delete_by_comment_v6 nat POSTROUTING
    delete_by_comment_v6 filter FORWARD

    # Forwarding stays enabled (see the header). Drop the saved values an
    # older version of this script left behind; the config may have changed
    # or disappeared since setup, so every tunnel's file goes.
    rm -f "$STATE_DIR"/orig_ip_forward.* "$STATE_DIR"/orig_ip6_forward.*
}

case "$ACTION" in
    setup)    setup ;;
    teardown) teardown ;;
    *)
        echo "Unknown action: $ACTION (use setup or teardown)" >&2
        exit 1
        ;;
esac
