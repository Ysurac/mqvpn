// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#include "addr_pool.h"
#include "log.h"

#include <string.h>
#include <stdlib.h>
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#endif

int
mqvpn_addr_pool_init(mqvpn_addr_pool_t *pool, const char *cidr)
{
    memset(pool, 0, sizeof(*pool));

    /* Parse "10.0.0.0/24" */
    char buf[32];
    strncpy(buf, cidr, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *slash = strchr(buf, '/');
    if (!slash) {
        LOG_ERR("addr_pool: invalid CIDR: %s", cidr);
        return -1;
    }
    *slash = '\0';
    char *endptr;
    long plen = strtol(slash + 1, &endptr, 10);
    if (endptr == slash + 1 || *endptr != '\0' || plen < 0 || plen > 32) {
        LOG_ERR("addr_pool: invalid prefix length in CIDR: %s", cidr);
        return -1;
    }
    pool->prefix_len = (int)plen;

    if (inet_pton(AF_INET, buf, &pool->base) != 1) {
        LOG_ERR("addr_pool: invalid address: %s", buf);
        return -1;
    }

    if (pool->prefix_len < 16 || pool->prefix_len > 30) {
        LOG_ERR("addr_pool: prefix length %d out of range [16,30]", pool->prefix_len);
        return -1;
    }

    uint32_t host_bits = 32 - pool->prefix_len;
    uint32_t total_hosts = (1U << host_bits) - 2; /* exclude network and broadcast */
    pool->pool_size =
        total_hosts > MQVPN_ADDR_POOL_MAX ? MQVPN_ADDR_POOL_MAX : total_hosts;
    pool->next = 2; /* .1 is reserved for server */

    /* Reject a pool whose highest handed-out address (base + pool_size) would
     * wrap past 255.255.255.255: a wrapped allocation can yield 0.0.0.0 —
     * which defeats every "assigned_ip.s_addr != 0" liveness check in the
     * server — and mqvpn_addr_pool_release's underflow guard can never
     * release a wrapped address. Non-aligned but non-wrapping bases stay
     * accepted (pinned by test_cidr_non_aligned). */
    uint32_t base_h = ntohl(pool->base.s_addr);
    if (base_h + pool->pool_size < base_h) {
        LOG_ERR("addr_pool: %s wraps past 255.255.255.255", cidr);
        return -1;
    }

    LOG_INF("addr_pool: %s, %u addresses available", cidr, pool->pool_size - 1);
    return 0;
}

int
mqvpn_addr_pool_alloc(mqvpn_addr_pool_t *pool, struct in_addr *out)
{
    /* Linear scan starting from pool->next */
    for (uint32_t i = 0; i < pool->pool_size; i++) {
        uint32_t off = ((pool->next - 1 + i) % pool->pool_size) + 1;
        /* skip offset 1 (server) */
        if (off == 1) continue;

        if (!pool->used[off]) {
            pool->used[off] = 1;
            pool->next = off + 1;
            uint32_t base_h = ntohl(pool->base.s_addr);
            out->s_addr = htonl(base_h + off);
            char str[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, out, str, sizeof(str));
            LOG_INF("addr_pool: allocated %s", str);
            return 0;
        }
    }

    LOG_ERR("addr_pool: exhausted");
    return -1;
}

int
mqvpn_addr_pool_alloc_at(mqvpn_addr_pool_t *pool, uint32_t offset, struct in_addr *out)
{
    if (offset < 2 || offset > pool->pool_size || pool->used[offset])
        return -1;
    pool->used[offset] = 1;
    uint32_t base_h = ntohl(pool->base.s_addr);
    out->s_addr = htonl(base_h + offset);
    char str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, out, str, sizeof(str));
    LOG_INF("addr_pool: allocated (pinned) %s", str);
    return 0;
}

void
mqvpn_addr_pool_release(mqvpn_addr_pool_t *pool, const struct in_addr *addr)
{
    uint32_t base_h = ntohl(pool->base.s_addr);
    uint32_t addr_h = ntohl(addr->s_addr);
    if (addr_h < base_h) {
        LOG_WRN("addr_pool: release underflow: addr outside pool range");
        return;
    }
    uint32_t off = addr_h - base_h;

    if (off >= 1 && off <= pool->pool_size) {
        pool->used[off] = 0;
        char str[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, addr, str, sizeof(str));
        LOG_INF("addr_pool: released %s", str);
    }
}

void
mqvpn_addr_pool_server_addr(const mqvpn_addr_pool_t *pool, struct in_addr *out)
{
    uint32_t base_h = ntohl(pool->base.s_addr);
    out->s_addr = htonl(base_h + 1);
}

/* ---- IPv6 pool (shares offsets with IPv4) ---- */

/* Host bits of the low 32-bit word for an IPv6 prefix in [96,128]. */
static uint32_t
host6_mask(int prefix6)
{
    int host_bits = 128 - prefix6;
    if (host_bits >= 32) return 0xFFFFFFFFu;
    if (host_bits <= 0) return 0;
    return (1u << host_bits) - 1;
}

int
mqvpn_addr_pool_init6(mqvpn_addr_pool_t *pool, const char *cidr6)
{
    char buf[64];
    strncpy(buf, cidr6, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    char *slash = strchr(buf, '/');
    if (!slash) {
        LOG_ERR("addr_pool6: invalid CIDR: %s", cidr6);
        return -1;
    }
    *slash = '\0';
    char *endptr;
    long plen = strtol(slash + 1, &endptr, 10);
    if (endptr == slash + 1 || *endptr != '\0' || plen < 96 || plen > 126) {
        LOG_ERR("addr_pool6: prefix length %ld out of range [96,126]", plen);
        return -1;
    }

    struct in6_addr base6;
    if (inet_pton(AF_INET6, buf, &base6) != 1) {
        LOG_ERR("addr_pool6: invalid address: %s", buf);
        return -1;
    }

    /* Every offset the shared pool hands out (1..pool_size, sized by the
     * IPv4 prefix) must land inside this prefix: base6's host part plus
     * pool_size may neither carry into the network bits nor wrap the low 32
     * bits. Otherwise clients get addresses outside the server's subnet6,
     * which it never routes back (e.g. a /24 IPv4 pool with a /126 IPv6
     * prefix). A non-aligned base is kept as given, as for IPv4. */
    uint32_t hostmask = host6_mask((int)plen);
    uint32_t base_low;
    memcpy(&base_low, &base6.s6_addr[12], sizeof(base_low));
    uint64_t last = (uint64_t)(ntohl(base_low) & hostmask) + pool->pool_size;
    if (last > hostmask) {
        LOG_ERR("addr_pool6: %s cannot hold the %u addresses of the IPv4 pool; use a "
                "shorter IPv6 prefix or a smaller IPv4 pool",
                cidr6, pool->pool_size);
        return -1;
    }

    pool->base6 = base6;
    pool->prefix6 = (int)plen;
    pool->has_v6 = 1;

    char v6str[INET6_ADDRSTRLEN];
    inet_ntop(AF_INET6, &pool->base6, v6str, sizeof(v6str));
    LOG_INF("addr_pool6: %s/%d (sharing IPv4 offsets)", v6str, pool->prefix6);
    return 0;
}

int
mqvpn_addr_pool_get6(const mqvpn_addr_pool_t *pool, uint32_t offset, struct in6_addr *out)
{
    memcpy(out, &pool->base6, sizeof(*out));
    /* Add offset to the host part of the low 32 bits (bytes 12-15). An
     * offset that would carry into the network bits (or wrap) leaves *out at
     * the base and fails, rather than yield an address outside the prefix. */
    uint32_t low;
    memcpy(&low, &out->s6_addr[12], sizeof(low));
    low = ntohl(low);
    uint32_t hostmask = host6_mask(pool->prefix6);
    uint64_t host = (uint64_t)(low & hostmask) + offset;
    if (host > hostmask) return -1;
    low = htonl((low & ~hostmask) | (uint32_t)host);
    memcpy(&out->s6_addr[12], &low, sizeof(low));
    return 0;
}

void
mqvpn_addr_pool_server_addr6(const mqvpn_addr_pool_t *pool, struct in6_addr *out)
{
    mqvpn_addr_pool_get6(pool, 1, out);
}

uint32_t
mqvpn_addr_pool_offset6(const mqvpn_addr_pool_t *pool, const struct in6_addr *addr)
{
    /* Verify the high 12 bytes match the base (valid for prefix >= /96) */
    if (memcmp(addr->s6_addr, pool->base6.s6_addr, 12) != 0) {
        return 0;
    }
    uint32_t addr_low, base_low;
    memcpy(&addr_low, &addr->s6_addr[12], sizeof(addr_low));
    memcpy(&base_low, &pool->base6.s6_addr[12], sizeof(base_low));
    addr_low = ntohl(addr_low);
    base_low = ntohl(base_low);
    /* ...and the network bits of the low word too (prefixes past /96). */
    uint32_t hostmask = host6_mask(pool->prefix6);
    if ((addr_low & ~hostmask) != (base_low & ~hostmask)) return 0;
    if (addr_low < base_low) return 0;
    return addr_low - base_low;
}
