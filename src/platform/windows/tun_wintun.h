// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * tun_wintun.h — Wintun-based TUN device for Windows
 *
 * Provides the same logical interface as the Linux tun.h but backed
 * by the Wintun ring-buffer driver. A dedicated reader thread feeds
 * packets to the libevent loop through a socketpair.
 */

#ifndef MQVPN_TUN_WINTUN_H
#define MQVPN_TUN_WINTUN_H

#ifdef _WIN32

#  include <winsock2.h>
#  include <windows.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <stdint.h>
#  include <stddef.h>

#  include <event2/util.h>

/* Forward-declare Wintun opaque handle types (same as wintun.h) */
typedef struct _WINTUN_ADAPTER *WINTUN_ADAPTER_HANDLE;
typedef struct _TUN_SESSION *WINTUN_SESSION_HANDLE;

/* mqvpn_tun_write() returns this when the ring is full. */
#  define MQVPN_TUN_EAGAIN (-2)

/* One socketpair frame: 2-byte big-endian length + up to 65535 bytes. */
#  define MQVPN_TUN_WIN_FRAME_MAX (2 + 65535)

typedef struct {
    /* Wintun handles */
    WINTUN_ADAPTER_HANDLE adapter;
    WINTUN_SESSION_HANDLE session;
    HANDLE read_event;

    /* Adapter identification */
    NET_LUID luid;
    DWORD if_index;
    char name[256];
    int mtu;

    /* Reader thread → libevent bridge (socketpair) */
    HANDLE reader_thread;
    evutil_socket_t pipe_rd; /* libevent monitors this */
    evutil_socket_t pipe_wr; /* reader thread writes here */
    volatile LONG stop;      /* InterlockedExchange flag */
    uint8_t *rx_buf;         /* main-thread frame reassembly (FRAME_MAX bytes) */
    int rx_len;              /* bytes of the current frame received so far */

    /* Address state */
    struct in_addr addr;
    struct in_addr peer_addr;
    struct in6_addr addr6;
    int has_v6;
} mqvpn_tun_win_t;

/* Load wintun.dll and resolve function pointers. Call once at startup. */
int mqvpn_wintun_load(void);

/* Create a Wintun adapter and start a session. */
int mqvpn_tun_win_create(mqvpn_tun_win_t *tun, const char *dev_name);

/* Assign IPv4 point-to-point address. */
int mqvpn_tun_win_set_addr(mqvpn_tun_win_t *tun, const char *addr, const char *peer_addr,
                           int prefix_len);

/* Assign IPv6 address. */
int mqvpn_tun_win_set_addr6(mqvpn_tun_win_t *tun, const char *addr6, int prefix_len);

/* Set MTU on the adapter (IPv6 too once an IPv6 address is set; setting the
 * IPv6 address applies the current MTU to IPv6). */
int mqvpn_tun_win_set_mtu(mqvpn_tun_win_t *tun, int mtu);

/* Pin the adapter's interface metric (automatic metric off), IPv4 and, once
 * an IPv6 address is set, IPv6. Lives and dies with the adapter. */
int mqvpn_tun_win_set_metric(mqvpn_tun_win_t *tun, ULONG metric);

/* Start the reader thread that bridges Wintun → socketpair. */
int mqvpn_tun_win_start_reader(mqvpn_tun_win_t *tun);

/* Main thread: the next complete packet from the reader thread's pipe,
 * reassembled across partial reads. Returns its length (> 0) with *pkt valid
 * until the next call, 0 if no complete packet is buffered yet, -1 on a pipe
 * error. */
int mqvpn_tun_win_read(mqvpn_tun_win_t *tun, const uint8_t **pkt);

/* Write a single IP packet to Wintun (called from main thread). */
int mqvpn_tun_win_write(mqvpn_tun_win_t *tun, const uint8_t *buf, size_t len);

/* Destroy adapter, stop reader, close socketpair. */
void mqvpn_tun_win_destroy(mqvpn_tun_win_t *tun);

#endif /* _WIN32 */
#endif /* MQVPN_TUN_WINTUN_H */
