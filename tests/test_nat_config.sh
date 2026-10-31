#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors

set -euo pipefail
HELPER="$1"
SCRIPT="$2"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

check() {
    local subnet="$1" subnet6="$2" tun="$3" actual expected
    # An explicit fourth argument checks the raw name before setup's default.
    expected=$(printf '%s\n%s\n%s\n' "$subnet" "$subnet6" "${4-$tun}")
    actual=$("$HELPER" "$tmp/config")
    if [ "$actual" != "$expected" ]; then
        printf 'FAIL: expected <%s>, got <%s>\n' "$expected" "$actual" >&2
        exit 1
    fi

    : >"$tmp/calls"
    PATH="$tmp/bin:$PATH" bash "$tmp/mqvpn-server-nat.sh" setup "$tmp/config" >/dev/null
    cat >"$tmp/expected" <<EOF
sysctl -w net.ipv4.ip_forward=1
iptables -t nat -A POSTROUTING -s $subnet ! -o $tun -j MASQUERADE -m comment --comment mqvpn-server-nat
iptables -I FORWARD -i $tun -s $subnet -j ACCEPT -m comment --comment mqvpn-server-nat
iptables -I FORWARD -o $tun -d $subnet -j ACCEPT -m comment --comment mqvpn-server-nat
EOF
    if [ -n "$subnet6" ]; then
        cat >>"$tmp/expected" <<EOF
sysctl -w net.ipv6.conf.all.forwarding=1
ip6tables -I FORWARD -i $tun -s $subnet6 -j ACCEPT -m comment --comment mqvpn-server-nat
ip6tables -I FORWARD -o $tun -d $subnet6 -j ACCEPT -m comment --comment mqvpn-server-nat
ip6tables -t nat -A POSTROUTING -s $subnet6 ! -o $tun -j MASQUERADE -m comment --comment mqvpn-server-nat
EOF
    fi
    diff -u "$tmp/expected" "$tmp/calls"
}

# Invalid input must stop the shell helper before any firewall/forwarding work.
ln -s "$HELPER" "$tmp/mqvpn-nat-config"
cp "$SCRIPT" "$tmp/mqvpn-server-nat.sh"
mkdir "$tmp/bin"
cat >"$tmp/bin/sysctl" <<'EOF'
#!/bin/sh
echo 'FAIL: NAT setup reached sysctl after invalid config' >&2
exit 99
EOF
chmod +x "$tmp/bin/sysctl"
printf -v long_name '%064d' 0
for bad in $'[Interface]\nSubnet=10.23.0.0/24' \
           '{"tun_name":"first\nsecond"}' '{"subnet":' '{"users":"x"}' \
           "{\"users\":[{\"name\":\"$long_name\",\"key\":\"k\"}]}" \
           "{\"users\":[\"$long_name:k\"]}"; do
    # Bash strings cannot carry NUL, so append one explicitly in the first case.
    printf '%s' "$bad" >"$tmp/config"
    if [[ "$bad" == '[Interface]'* ]]; then printf '\0' >>"$tmp/config"; fi
    if PATH="$tmp/bin:$PATH" bash "$tmp/mqvpn-server-nat.sh" setup "$tmp/config" \
        >"$tmp/out" 2>&1; then
        echo 'FAIL: invalid NAT config accepted' >&2
        exit 1
    fi
    if grep -q 'FAIL:' "$tmp/out"; then cat "$tmp/out" >&2; exit 1; fi
    if [[ "$bad" == '{"users":'* ]]; then
        grep -Fq 'config: invalid "users"' "$tmp/out"
        grep -Fq 'names up to 63 bytes' "$tmp/out"
    fi
done

# Teardown removes the rules and any forwarding state an older version saved,
# but leaves forwarding enabled, even if the config was broken, removed or
# edited.
sed "s|^STATE_DIR=.*|STATE_DIR=\"$tmp/state\"|" "$SCRIPT" >"$tmp/mqvpn-server-nat.sh"
grep -qxF "STATE_DIR=\"$tmp/state\"" "$tmp/mqvpn-server-nat.sh" || {
    echo 'FAIL: STATE_DIR override did not apply' >&2
    exit 1
}
mkdir "$tmp/state"
export NAT_TEST_DIR="$tmp"
cat >"$tmp/bin/iptables" <<'EOF'
#!/bin/bash
set -eu
marker="$NAT_TEST_DIR/$(basename "$0").$2.$4.deleted"
case "$3" in
    -L) if [ ! -f "$marker" ]; then echo '1 ACCEPT /* mqvpn-server-nat */'; fi ;;
    -D) [ "$5" = 1 ]; touch "$marker" ;;
    *) exit 99 ;;
esac
EOF
ln -s iptables "$tmp/bin/ip6tables"
cat >"$tmp/bin/sysctl" <<'EOF'
#!/bin/sh
[ "$1" = -w ] || exit 99
echo "$2" >>"$NAT_TEST_DIR/sysctl.log"
EOF
chmod +x "$tmp/bin/iptables" "$tmp/bin/sysctl"
for state in broken missing renamed; do
    case "$state" in
        broken) printf '{"subnet":' >"$tmp/config" ;;
        missing) rm "$tmp/config" ;;
        renamed) printf '{"tun_name":"mqvpn7"}' >"$tmp/config" ;;
    esac
    printf '0\n' >"$tmp/state/orig_ip_forward.mqvpn9"
    printf '1\n' >"$tmp/state/orig_ip6_forward.mqvpn9"
    rm -f "$tmp/"*.deleted "$tmp/sysctl.log"
    PATH="$tmp/bin:$PATH" bash "$tmp/mqvpn-server-nat.sh" teardown "$tmp/config" >/dev/null
    for tool in iptables ip6tables; do
        test -f "$tmp/$tool.nat.POSTROUTING.deleted"
        test -f "$tmp/$tool.filter.FORWARD.deleted"
    done
    test ! -f "$tmp/sysctl.log"
    test ! -f "$tmp/state/orig_ip_forward.mqvpn9"
    test ! -f "$tmp/state/orig_ip6_forward.mqvpn9"
    # A second teardown with no saved state is harmless.
    PATH="$tmp/bin:$PATH" bash "$tmp/mqvpn-server-nat.sh" teardown "$tmp/config" >/dev/null
    test ! -f "$tmp/sysctl.log"
done

# Exercise the real setup/mapfile path without touching host networking.
cat >"$tmp/bin/iptables" <<'EOF'
#!/bin/sh
echo "$(basename "$0") $*" >>"$NAT_TEST_DIR/calls"
EOF
cat >"$tmp/bin/sysctl" <<'EOF'
#!/bin/sh
case "$*" in
    '-w net.ipv4.ip_forward=1'|'-w net.ipv6.conf.all.forwarding=1')
        echo "sysctl $*" >>"$NAT_TEST_DIR/calls" ;;
    *) exit 99 ;;
esac
EOF

# BOM, CRLF, lowercase keys, last key wins, and section scoping.
printf '\357\273\277[interface]\r\nsubnet=10.22.0.0/24\r\nsubnet=10.23.0.0/24\r\ntunname=mqvpn9\r\n[Other]\r\nSubnet=10.99.0.0/24\r\n' >"$tmp/config"
check '10.23.0.0/24' '' mqvpn9

cat >"$tmp/config" <<'EOF'
{"subnet":"10.23.0.0/24","subnet":"10.99.0.0/24",
 "subnet6":"fd00::/112","tun_name":"mqvpn\u0039",
 "unused":{"subnet":"10.88.0.0/24"}}
EOF
check '10.23.0.0/24' 'fd00::/112' mqvpn9
: >"$tmp/config"
check '10.0.0.0/24' '' mqvpn0

# Empty names must use the same default as the server, with or without IPv6.
for subnet6 in '' 'fd00::/112'; do
    printf '[Interface]\nTunName=\nSubnet6=%s\n' "$subnet6" >"$tmp/config"
    check '10.0.0.0/24' "$subnet6" mqvpn0 ''
    printf '{"tun_name":"","subnet6":"%s"}\n' "$subnet6" >"$tmp/config"
    check '10.0.0.0/24' "$subnet6" mqvpn0 ''
done
echo 'PASS: NAT uses the shared parser for setup and keeps forwarding on teardown'
