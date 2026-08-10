#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright (c) 2026 mp0rta and mqvpn contributors
#
# bench_stream_gso.sh — outer-UDP TX batching factor on the hybrid STREAM
# lane (investigation scaffold for the stream-lane deferred flush).
#
# Question this answers: xqc_datagram_send() queued exactly one packet per
# call, so the datagram lane's batching factor was structurally 1.00 until
# xquic's defer_send_flush landed. The stream lane looked like it should differ,
# since xqc_stream_send() splits one write into several packet_outs before the
# flush — but measurement said otherwise for the client uplink, because the
# lwIP netif MTU (not lwIP's TCP_MSS setting) bounds a relayed segment to a
# single QUIC packet. Server egress writes TCP_EGRESS_RELAY_CHUNK at a time,
# so its ceiling is that constant instead; see its comment in
# src/hybrid/tcp_egress.c, which carries the measured chunk sweep.
#
# DIRECTION MATTERS. Upload (the default) exercises the CLIENT uplink relay;
# the server's factor column then reflects ACK traffic and says nothing about
# the stream lane. Set IPERF_EXTRA="-R" to move the bulk send to the server
# (svr_tcp_egress relaying via xqc_h3_request_send_body) — that is the only
# configuration in which the server column, and any chunk-size conclusion
# drawn from it, means anything.
#
# Topology / lane engagement: identical to bench_hybrid_scheduler.sh — the
# iperf3 target is 10.222.0.1, a /32 loopback alias in the SERVER netns,
# outside the tunnel subnet and every path subnet, which is what makes the
# classifier lane a TCP flow to STREAM at all. Engagement is VERIFIED per rep
# via the server control API (get_stats tcp_flows_total > 0), never assumed:
# an in-subnet target would silently benchmark RAW under a "hybrid on" label.
#
# Arms — one binary, config-only switch (G20: no toolchain boundary between
# arms; the binary's sha256 is printed in the header for that reason):
#   on_gso    [Hybrid] Enabled=true/Tcp=stream + EgressAllow; UdpGso default
#             true. THE measurement.
#   on_nogso  same, plus [Advanced] UdpGso=false. Control: with no batched-send
#             callback registered every datagram takes its own sendto(), so
#             this must read exactly 1.0000 — a factor that drifts off 1 here
#             means the counters, not the lane, are being measured.
#   off_gso   no [Hybrid] section at all: TCP tunneled as raw IP over
#             DATAGRAM. Cross-check against the 17-19x the datagram lane
#             already measures on this tree (project memory, 2026-08-08).
#
# Metric: the "udp-tx: sends=N datagrams=M gso_config=X" teardown line
# (pinned wording — grep "udp-tx: sends=" in mqvpn_client_destroy /
# mqvpn_server_destroy) from BOTH
# endpoints, read only AFTER the process is reaped (it is written during
# destroy, so it does not exist while the process is alive). datagrams/sends
# is the achieved batching factor.
#
# Unshaped by default, deliberately: the datagram-lane numbers this compares
# against were taken in an unshaped netns, and shaping changes the answer in
# the direction of the hypothesis (a cwnd/pacer-blocked conn accumulates a
# queue the flush timing had nothing to do with). Set NETEM to shape.
#
# Usage: sudo [MQVPN=/path/to/mqvpn] ./bench_stream_gso.sh
# Env:   REPEAT=3 PVALUES="1 4" DURATION=15 ARMS="on_gso on_nogso off_gso"
#        NETEM="" (unshaped; e.g. "delay 25ms rate 100mbit limit 2000")
#        SCHED=wlb CTRL_PORT=9098

set -u

source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bench_env_setup.sh"

REPEAT="${REPEAT:-3}"
PVALUES="${PVALUES:-1 4}"
ARMS="${ARMS:-on_gso on_nogso off_gso}"
DURATION="${DURATION:-15}"
NETEM="${NETEM:-}"
SCHED="${SCHED:-wlb}"
CTRL_PORT="${CTRL_PORT:-9098}"
TARGET="10.222.0.1"

if [ "$(id -u)" -ne 0 ]; then echo "error: needs root (netns/tc). sudo." >&2; exit 1; fi
bench_check_test_deps iperf3 python3 nc openssl tc || exit 1

# NOT ${RESULTS_DIR:-...}: bench_env_setup.sh is sourced above and already sets
# RESULTS_DIR to bench_results/, so that default could never fire and every run
# would litter the shared archive directory instead of its own subdirectory.
OUT_DIR="${STREAM_GSO_OUT_DIR:-${BENCH_DIR}/../bench_results/stream_gso}"
mkdir -p "$OUT_DIR"
STAMP="$(date +%s)"
CSV="${OUT_DIR}/stream_gso_${STAMP}.csv"
LOG_DIR="${OUT_DIR}/logs_${STAMP}"
mkdir -p "$LOG_DIR"
echo "arm,P,rep,bw_mbps,c_sends,c_dgrams,c_factor,s_sends,s_dgrams,s_factor,tcp_flows_total,status" >"$CSV"

WORK="$(mktemp -d)"
INI_ON="${WORK}/hybrid_on.ini"
cat >"$INI_ON" <<EOF
[Hybrid]
Enabled = true
Tcp = stream
EgressAllow = 10.222.0.0/24
EOF

# Same [Hybrid] policy, UdpGso off. The [Advanced] fixture mirrors
# tests/test_config.c's test_advanced_udp_gso / run_udp_gso_config_test.sh
# Arm B exactly.
INI_ON_NOGSO="${WORK}/hybrid_on_nogso.ini"
cat >"$INI_ON_NOGSO" <<EOF
[Hybrid]
Enabled = true
Tcp = stream
EgressAllow = 10.222.0.0/24

[Advanced]
UdpGso = false
EOF

declare -A R_BW R_CF R_SF

trap 'bench_cleanup; rm -rf "$WORK"' EXIT

# ── helpers ─────────────────────────────────────────────────────────────────

# run_iperf_p <target> <duration> <P> — echo receiver Mbps ("0.0" on failure).
run_iperf_p() {
    local target="$1" duration="$2" P="$3" json ipid
    json="$(mktemp)"
    ip netns exec "$NS_SERVER" iperf3 -s -B "$target" -1 &>/dev/null &
    ipid=$!
    sleep 1
    # IPERF_EXTRA="-R" flips the transfer to download, which is what moves the
    # STREAM-lane bulk send to the SERVER side (svr_tcp_egress relays recv()ed
    # egress bytes with xqc_h3_request_send_body). Upload only ever exercises
    # the client's uplink relay, so the server's factor column reads ACK
    # traffic and says nothing about the stream lane.
    ip netns exec "$NS_CLIENT" timeout $((duration + 20)) \
        iperf3 -c "$target" -t "$duration" -O 3 -P "$P" ${IPERF_EXTRA:-} --json \
        >"$json" 2>/dev/null || true
    kill "$ipid" 2>/dev/null || true; wait "$ipid" 2>/dev/null || true
    python3 -c "
import json
try:
    e=json.load(open('$json')).get('end',{})
    print(f\"{e['sum_received']['bits_per_second']/1e6:.1f}\" if 'sum_received' in e else '0.0')
except Exception: print('0.0')"
    rm -f "$json"
}

server_tcp_flows_total() {
    bench_query_control "$1" get_stats | python3 -c "
import sys, json
try: print(json.load(sys.stdin).get('tcp_flows_total',0))
except Exception: print(0)"
}

# udp_tx_of <logfile> — echo "sends datagrams factor" from the teardown line,
# or "NA NA NA" when the line is absent (process never carried traffic, or was
# killed before destroy ran). awk, not `grep | head`: under `set -o pipefail`
# an early-exiting reader SIGPIPEs the writer and the pipeline fails falsely
# (G19). Reads to EOF and keeps the LAST match, so a log with several
# teardowns reports the final one.
udp_tx_of() {
    [ -f "$1" ] || { echo "NA NA NA"; return; }
    awk '
        /udp-tx: / {
            s = ""; d = ""
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^sends=/)     s = substr($i, 7)
                if ($i ~ /^datagrams=/) d = substr($i, 11)
            }
            if (s != "" && d != "") { last_s = s; last_d = d }
        }
        END {
            if (last_s != "" && last_s + 0 > 0) printf "%s %s %.4f", last_s, last_d, last_d / last_s
            else print "NA NA NA"
        }' "$1"
}

# _stats <list> — "mean min max n" in one pass ("- - - 0" when empty).
# 4 decimals, not 2: the on_nogso control's whole claim is "exactly 1.0000",
# and a non-batching arm that actually read 1.0005 (a value already seen on
# this tree) would print as a passing "1.00" at 2 decimals.
_stats() {
    awk '{for(i=1;i<=NF;i++){v=$i;s+=v;n++; if(n==1||v<mn)mn=v; if(n==1||v>mx)mx=v}}
         END{ if(!n){print "- - - 0";exit}
              printf "%.4f %.4f %.4f %d", s/n, mn, mx, n }' <<<"$1"
}

# ── one cell (all reps) ─────────────────────────────────────────────────────

run_cell() {
    local arm="$1" P="$2"
    local key="${arm}_P${P}"
    BENCH_SCHEDULER="$SCHED"

    local cfg=""
    case "$arm" in
        on_gso)   cfg="--config $INI_ON" ;;
        on_nogso) cfg="--config $INI_ON_NOGSO" ;;
        off_gso)  cfg="" ;;
        *) echo "ERROR: unknown arm '$arm'" >&2; exit 2 ;;
    esac

    local rep
    for rep in $(seq 1 "$REPEAT"); do
        printf "    %-9s P=%-2s rep %d/%d ... " "$arm" "$P" "$rep" "$REPEAT"

        local slog="${LOG_DIR}/${key}_r${rep}_server.log"
        local clog="${LOG_DIR}/${key}_r${rep}_client.log"

        if ! bench_start_vpn_server "--control-port $CTRL_PORT $cfg" "$slog" >/dev/null 2>&1 \
           || ! bench_start_vpn_client \
                "--path $(bench_path_veth_client 0) --path $(bench_path_veth_client 1) $cfg" \
                "$clog" >/dev/null 2>&1 \
           || ! bench_wait_tunnel 25 >/dev/null 2>&1; then
            echo "VPN_FAIL"; { echo "      ── server log tail ──"; tail -5 "$slog"; } >&2
            echo "${arm},${P},${rep},,,,,,,,,VPN_FAIL" >>"$CSV"
            bench_stop_vpn >/dev/null 2>&1; continue
        fi
        bench_wait_for_n_paths 2 20 "$CTRL_PORT" >/dev/null 2>&1 || true

        local bw; bw="$(run_iperf_p "$TARGET" "$DURATION" "$P")"
        local flows_total; flows_total="$(server_tcp_flows_total "$CTRL_PORT")"

        # Reap BOTH endpoints before reading udp-tx: the line is written by
        # mqvpn_{client,server}_destroy, so it does not exist until the
        # process has actually torn down.
        bench_stop_vpn >/dev/null 2>&1

        local cs cd cf ss sd sf
        read -r cs cd cf <<<"$(udp_tx_of "$clog")"
        read -r ss sd sf <<<"$(udp_tx_of "$slog")"

        local status="OK"
        if ! awk -v b="$bw" 'BEGIN{exit !(b+0>0)}'; then
            status="ZERO_BW"
        elif [ "${arm#on_}" != "$arm" ] && [ "${flows_total:-0}" = "0" ]; then
            status="NO_LANE"
        elif [ "$cf" = "NA" ] || [ "$sf" = "NA" ]; then
            status="NO_TXLINE"
        fi

        echo "${arm},${P},${rep},${bw},${cs},${cd},${cf},${ss},${sd},${sf},${flows_total},${status}" >>"$CSV"
        if [ "$status" = "OK" ]; then
            R_BW["$key"]="${R_BW[$key]:-} ${bw}"
            R_CF["$key"]="${R_CF[$key]:-} ${cf}"
            R_SF["$key"]="${R_SF[$key]:-} ${sf}"
        fi

        printf "%7s Mbps  tx-factor c=%-8s s=%-8s flows=%-4s [%s]\n" \
            "$bw" "$cf" "$sf" "${flows_total:-0}" "$status"
    done
}

# ── main ────────────────────────────────────────────────────────────────────

echo "================================================================"
echo "  mqvpn STREAM-lane outer-UDP TX batching factor"
echo "  Binary:   $MQVPN"
echo "  SHA256:   $(sha256sum "$MQVPN" | awk '{print $1}')"
echo "  Version:  $("$MQVPN" --version 2>/dev/null | head -1)"
echo "  Kernel:   $(uname -r)"
echo "  Target:   ${TARGET} (out-of-tunnel egress, STREAM-lane eligible)"
echo "  Link:     ${NETEM:-unshaped netns}"
echo "  iperf3:   TCP uplink -P {${PVALUES// /,}} -t ${DURATION}s, ${REPEAT} reps"
echo "  Sched:    ${SCHED}"
echo "  Arms:     ${ARMS}"
echo "  CSV:      $CSV"
echo "  Date:     $(date '+%Y-%m-%d %H:%M')"
echo "================================================================"

bench_setup_netns_n 2
bench_add_server_host_routes 2
ip netns exec "$NS_SERVER" ip addr add "${TARGET}/32" dev lo
if [ -n "$NETEM" ]; then
    bench_apply_netem "$NETEM" "$NETEM" || { echo "FATAL: tc netem apply failed" >&2; exit 1; }
fi

for arm in $ARMS; do
    echo ""
    echo "━━━ arm=${arm} ━━━"
    for P in $PVALUES; do
        run_cell "$arm" "$P"
    done
done

# ── summary ─────────────────────────────────────────────────────────────────

echo ""; echo ""
echo "================================================================"
echo "  Results — outer-UDP TX batching factor (datagrams/sends)"
echo "================================================================"
echo ""
if [ -n "${IPERF_EXTRA:-}" ]; then
    BULK_SIDE="server (download: -R)"
    BULK_NOTE="  BULK SIDE: server. The SERVER column measures the stream lane; client = ACKs."
else
    BULK_SIDE="client (upload: default)"
    BULK_NOTE="  BULK SIDE: client. The SERVER column is ACK traffic, NOT the stream lane —
             set IPERF_EXTRA=-R to measure the server egress relay."
fi
printf "  %-9s │ %-2s │ %-26s │ %-26s │ %s\n" \
    "arm" "P" "client factor mean(min-max)" "server factor mean(min-max)" "Mbps"
echo "  ──────────┼────┼────────────────────────────┼────────────────────────────┼───────"
for arm in $ARMS; do
    for P in $PVALUES; do
        read -r cm cl ch cn <<<"$(_stats "${R_CF[${arm}_P${P}]:-}")"
        read -r sm sl sh sn <<<"$(_stats "${R_SF[${arm}_P${P}]:-}")"
        read -r bm bl bh bn <<<"$(_stats "${R_BW[${arm}_P${P}]:-}")"
        printf "  %-9s │ %-2s │ %9s (%8s-%8s) %d │ %9s (%8s-%8s) %d │ %s\n" \
            "$arm" "$P" "$cm" "$cl" "$ch" "$cn" "$sm" "$sl" "$sh" "$sn" "$bm"
    done
done
echo ""
echo "  Direction: ${BULK_SIDE}"
echo "$BULK_NOTE"
echo "  factor = datagrams/sends from the 'udp-tx:' teardown line, per endpoint."
echo "  on_nogso MUST read exactly 1.0000 on both endpoints — it is the control,"
echo "    which is why this table prints 4 decimals rather than 2."
echo "  Only status=OK reps enter the means; see $CSV for the rest."
echo "  Logs: $LOG_DIR"
echo "================================================================"
