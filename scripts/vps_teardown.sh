#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
# scripts/vps_teardown.sh — Stop mqvpn server and clean up after vps_setup.sh
set -e

COMMENT="mqvpn-vps-test"

# Stop only what vps_setup.sh (and an iperf3 started with
# -I /run/mqvpn-vps-iperf3.pid) recorded, never every process of that name.
for pidfile in /run/mqvpn-vps.pid /run/mqvpn-vps-iperf3.pid; do
    [ -f "$pidfile" ] || continue
    kill "$(cat "$pidfile")" 2>/dev/null || true
    rm -f "$pidfile"
done

# Delete every rule vps_setup.sh added, matched by its comment, whatever its
# exact spec (highest line number first so the others keep their numbers).
delete_tagged() {
    local table="$1" chain="$2" line
    for line in $(iptables -t "$table" -L "$chain" --line-numbers -n 2>/dev/null |
        awk -v c="$COMMENT" 'index($0, "/* " c " */") { print $1 }' | sort -rn); do
        iptables -t "$table" -D "$chain" "$line" 2>/dev/null || true
    done
}
delete_tagged filter INPUT
delete_tagged filter FORWARD
delete_tagged nat POSTROUTING

ip link del mqvpn0 2>/dev/null || true

echo "Cleaned up"
