/*
 * routing.c — Split tunnel routing for Linux
 *
 * Manages ip route commands for VPN split tunneling:
 *   - Pin server route via original gateway
 *   - Catch-all 0.0.0.0/1 + 128.0.0.0/1 via TUN
 *   - IPv6 catch-all ::/1 + 8000::/1 via TUN
 */

#include "platform_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/wait.h>
#include <net/if.h>

#define ROUTE_DISCOVER_MAX_ATTEMPTS 6
#define ROUTE_DISCOVER_RETRY_DELAY_SEC 1

/* Try known absolute paths for the 'ip' binary before falling back to PATH.
 * On some distributions /usr/sbin is not in root's PATH under sudo. */
static void
exec_ip(char *const argv[])
{
    static const char *const paths[] = {
        "/usr/sbin/ip", "/sbin/ip", "/usr/bin/ip", "/bin/ip", NULL
    };
    for (int i = 0; paths[i]; i++)
        execv(paths[i], argv);
    execvp("ip", argv); /* last resort: search PATH */
}

static int
run_ip_cmd(const char *const argv[])
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        exec_ip((char *const *)argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

static int
discover_route(const char *server_ip, sa_family_t af, char *gateway, size_t gw_len,
               char *iface, size_t if_len)
{
    int fds[2];
    if (pipe(fds) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        const char *const a4[] = {"ip", "-4", "route", "get", server_ip, NULL};
        const char *const a6[] = {"ip", "-6", "route", "get", server_ip, NULL};
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        exec_ip((char *const *)((af == AF_INET6) ? a6 : a4));
        _exit(127);
    }

    close(fds[1]);
    /* Read until EOF — a single read() call may not capture all output. */
    char out[1024];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(out) - 1 &&
           (n = read(fds[0], out + total, sizeof(out) - 1 - total)) > 0)
        total += (size_t)n;
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;

    out[total] = '\0';

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || total == 0) {
        LOG_WRN("ip route get %s: exit=%d output='%s'",
                server_ip,
                WIFEXITED(status) ? WEXITSTATUS(status) : -1,
                out);
        return -1;
    }

    LOG_DBG("ip route get %s: %s", server_ip, out);

    gateway[0] = '\0';
    iface[0] = '\0';

    char *saveptr = NULL;
    for (char *tok = strtok_r(out, " \t\r\n", &saveptr); tok;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        if (strcmp(tok, "via") == 0) {
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
            if (tok) snprintf(gateway, gw_len, "%s", tok);
        } else if (strcmp(tok, "dev") == 0) {
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
            if (tok) snprintf(iface, if_len, "%s", tok);
        }
    }
    if (!iface[0])
        LOG_WRN("ip route get %s: no 'dev' token in output: '%s'", server_ip, out);
    return iface[0] ? 0 : -1;
}

/* Read the main routing table directly (not policy-routing-aware) via
 * "ip route show <server_ip>" and find the nexthop whose dev matches
 * want_iface (preferred).  If want_iface is NULL or not present in the
 * nexthops, falls back to the first nexthop found.
 * Returns 1 on success (gateway/iface filled), 0 on failure. */
static int
discover_route_from_show(const char *server_ip, sa_family_t af,
                         char *gateway, size_t gw_len,
                         char *iface,   size_t if_len,
                         const char *want_iface)
{
    int fds[2];
    if (pipe(fds) < 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 0; }

    if (pid == 0) {
        const char *v = (af == AF_INET6) ? "-6" : "-4";
        const char *argv[] = {"ip", v, "route", "show", server_ip, NULL};
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        exec_ip((char *const *)argv);
        _exit(127);
    }

    close(fds[1]);
    char out[2048];
    size_t total = 0;
    ssize_t n;
    while (total < sizeof(out) - 1 &&
           (n = read(fds[0], out + total, sizeof(out) - 1 - total)) > 0)
        total += (size_t)n;
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return 0;

    out[total] = '\0';
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0 || total == 0) return 0;

    LOG_DBG("ip route show %s: %s", server_ip, out);

    /* Walk nexthop blocks.  Each nexthop starts with the keyword "nexthop"
     * followed by "via <gw> dev <iface> ...".
     * We prefer the nexthop whose dev matches want_iface; if that is not
     * found we fall back to the first nexthop encountered.  Single-path
     * entries (no "nexthop" keyword) are handled by scanning the whole line. */
    char pref_gw[INET6_ADDRSTRLEN]  = {0}; /* nexthop matching want_iface */
    char pref_dev[IFNAMSIZ]         = {0};
    char first_gw[INET6_ADDRSTRLEN] = {0}; /* first nexthop seen (fallback) */
    char first_dev[IFNAMSIZ]        = {0};
    char cur_gw[INET6_ADDRSTRLEN]   = {0};
    char cur_dev[IFNAMSIZ]          = {0};

    char *saveptr = NULL;
    for (char *tok = strtok_r(out, " \t\r\n", &saveptr); tok;
         tok = strtok_r(NULL, " \t\r\n", &saveptr)) {
        if (strcmp(tok, "nexthop") == 0) {
            /* Commit the nexthop we just finished parsing */
            if (cur_dev[0]) {
                if (!first_dev[0]) {
                    snprintf(first_gw,  sizeof(first_gw),  "%s", cur_gw);
                    snprintf(first_dev, sizeof(first_dev), "%s", cur_dev);
                }
                if (!pref_dev[0] && want_iface && strcmp(cur_dev, want_iface) == 0) {
                    snprintf(pref_gw,  sizeof(pref_gw),  "%s", cur_gw);
                    snprintf(pref_dev, sizeof(pref_dev), "%s", cur_dev);
                }
            }
            cur_gw[0] = cur_dev[0] = '\0';
        } else if (strcmp(tok, "via") == 0) {
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
            if (tok) snprintf(cur_gw, sizeof(cur_gw), "%s", tok);
        } else if (strcmp(tok, "dev") == 0) {
            tok = strtok_r(NULL, " \t\r\n", &saveptr);
            if (tok) snprintf(cur_dev, sizeof(cur_dev), "%s", tok);
        }
    }
    /* Commit the last nexthop block (or single-path entry) */
    if (cur_dev[0]) {
        if (!first_dev[0]) {
            snprintf(first_gw,  sizeof(first_gw),  "%s", cur_gw);
            snprintf(first_dev, sizeof(first_dev), "%s", cur_dev);
        }
        if (!pref_dev[0] && want_iface && strcmp(cur_dev, want_iface) == 0) {
            snprintf(pref_gw,  sizeof(pref_gw),  "%s", cur_gw);
            snprintf(pref_dev, sizeof(pref_dev), "%s", cur_dev);
        }
    }

    /* Use preferred match; fall back to first nexthop if not found */
    const char *use_gw  = pref_dev[0] ? pref_gw  : first_gw;
    const char *use_dev = pref_dev[0] ? pref_dev : first_dev;

    if (!use_dev[0]) return 0;

    snprintf(gateway, gw_len, "%s", use_gw);
    snprintf(iface,   if_len,  "%s", use_dev);
    return 1;
}

int
setup_routes(platform_ctx_t *p)
{
    sa_family_t af = p->server_addr.ss_family;
    int prefix = mqvpn_sa_host_prefix(&p->server_addr);
    mqvpn_sa_ntop(&p->server_addr, p->server_ip_str, sizeof(p->server_ip_str));

    {
        /* On OpenMPTCProuter, policy rules/routes can appear shortly after
         * process startup. Retry route discovery before giving up. */
        const char *first_iface = (p->path_mgr.n_paths > 0 &&
                                   p->path_mgr.paths[0].iface[0])
                                  ? p->path_mgr.paths[0].iface : NULL;
        int route_ok = 0;

        for (int attempt = 1; attempt <= ROUTE_DISCOVER_MAX_ATTEMPTS; attempt++) {
            if (discover_route(p->server_ip_str, af,
                               p->orig_gateway, sizeof(p->orig_gateway),
                               p->orig_iface,   sizeof(p->orig_iface)) == 0) {
                route_ok = 1;
                break;
            }

            if (discover_route_from_show(p->server_ip_str, af,
                                         p->orig_gateway, sizeof(p->orig_gateway),
                                         p->orig_iface,   sizeof(p->orig_iface),
                                         first_iface)) {
                if (first_iface && strcmp(p->orig_iface, first_iface) == 0)
                    LOG_INF("route lookup fallback via first path %s: gw=%s",
                            first_iface, p->orig_gateway);
                else
                    LOG_INF("route lookup fallback via first nexthop %s: gw=%s",
                            p->orig_iface, p->orig_gateway);
                route_ok = 1;
                break;
            }

            if (attempt < ROUTE_DISCOVER_MAX_ATTEMPTS) {
                LOG_WRN("route discovery attempt %d/%d failed for %s, retrying in %ds",
                        attempt, ROUTE_DISCOVER_MAX_ATTEMPTS,
                        p->server_ip_str, ROUTE_DISCOVER_RETRY_DELAY_SEC);
                sleep(ROUTE_DISCOVER_RETRY_DELAY_SEC);
            }
        }

        if (!route_ok) {
            if (first_iface) {
                /* Last-resort fail-open mode: continue using the first
                 * configured multipath interface even without a discovered
                 * gateway. This avoids hard startup failure on systems where
                 * policy routes are transiently unavailable. */
                p->orig_gateway[0] = '\0';
                snprintf(p->orig_iface, sizeof(p->orig_iface), "%s", first_iface);
                LOG_WRN("could not determine original route for %s; "
                        "falling back to first path iface %s",
                        p->server_ip_str, p->orig_iface);
            } else {
                LOG_WRN("could not determine original iface for %s",
                        p->server_ip_str);
                return -1;
            }
        }
    }

    char host_cidr[INET6_ADDRSTRLEN + 5];
    snprintf(host_cidr, sizeof(host_cidr), "%s/%d", p->server_ip_str, prefix);
    const char *ip_flag = (af == AF_INET6) ? "-6" : "-4";

    if (p->orig_gateway[0] != '\0') {
        LOG_INF("split tunnel: server %s via %s dev %s", p->server_ip_str,
                p->orig_gateway, p->orig_iface);
        const char *const pin[] = {"ip",          ip_flag, "route",         "replace",
                                   host_cidr,     "via",   p->orig_gateway, "dev",
                                   p->orig_iface, NULL};
        if (run_ip_cmd(pin) < 0) {
            LOG_WRN("failed to pin server route");
            return -1;
        }
    } else {
        LOG_INF("split tunnel: server %s on-link dev %s", p->server_ip_str,
                p->orig_iface);
    }

    if (p->route_via_server) {
        /* Remove any stale /1 catch-all routes left by a previous session that
         * ran without route_via_server (e.g. crash or config change). */
        const char *const del_low[]  = {"ip", "route", "del", "0.0.0.0/1",
                                        "dev", p->tun.name, NULL};
        const char *const del_high[] = {"ip", "route", "del", "128.0.0.0/1",
                                        "dev", p->tun.name, NULL};
        (void)run_ip_cmd(del_low);
        (void)run_ip_cmd(del_high);

        const char *const dflt[] = {"ip",  "route",           "replace", "default",
                                    "via", p->server_tunnel_ip, "dev", p->tun.name, NULL};
        if (run_ip_cmd(dflt) < 0) {
            LOG_WRN("failed to set default route via %s on %s", p->server_tunnel_ip,
                    p->tun.name);
            if (p->orig_gateway[0]) {
                const char *u[] = {"ip",  ip_flag,         "route", "del",         host_cidr,
                                   "via", p->orig_gateway, "dev",   p->orig_iface, NULL};
                (void)run_ip_cmd(u);
            }
            return -1;
        }
    } else {
        const char *const low[] = {"ip",  "route",     "replace", "0.0.0.0/1",
                                   "dev", p->tun.name, NULL};
        const char *const high[] = {"ip",  "route",     "replace", "128.0.0.0/1",
                                    "dev", p->tun.name, NULL};
        if (run_ip_cmd(low) < 0 || run_ip_cmd(high) < 0) {
            LOG_WRN("failed to set catch-all routes via %s", p->tun.name);
            const char *u1[] = {"ip", "route", "del", "0.0.0.0/1", "dev", p->tun.name, NULL};
            const char *u2[] = {"ip",  "route",     "del", "128.0.0.0/1",
                                "dev", p->tun.name, NULL};
            (void)run_ip_cmd(u1);
            (void)run_ip_cmd(u2);
            if (p->orig_gateway[0]) {
                const char *u3[] = {"ip",  ip_flag,         "route", "del",         host_cidr,
                                    "via", p->orig_gateway, "dev",   p->orig_iface, NULL};
                (void)run_ip_cmd(u3);
            }
            return -1;
        }
    }
    p->routing_configured = 1;

    /* IPv6 catch-all routes */
    if (p->has_v6) {
        const char *v6l[] = {"ip",   "-6",  "route",     "replace",
                             "::/1", "dev", p->tun.name, NULL};
        const char *v6h[] = {"ip",       "-6",  "route",     "replace",
                             "8000::/1", "dev", p->tun.name, NULL};
        if (run_ip_cmd(v6l) == 0 && run_ip_cmd(v6h) == 0) {
            p->routing6_configured = 1;
            LOG_INF("IPv6 catch-all routes set via %s", p->tun.name);
        } else {
            LOG_WRN("failed to set IPv6 catch-all routes (continuing IPv4-only)");
        }
    }
    return 0;
}

void
cleanup_routes(platform_ctx_t *p)
{
    if (!p->routing_configured) return;

    if (p->routing6_configured) {
        const char *d1[] = {"ip", "-6", "route", "del", "::/1", "dev", p->tun.name, NULL};
        const char *d2[] = {"ip",       "-6",  "route",     "del",
                            "8000::/1", "dev", p->tun.name, NULL};
        (void)run_ip_cmd(d1);
        (void)run_ip_cmd(d2);
        p->routing6_configured = 0;
    }

    /* Always attempt to remove all possible route types so that stale routes
     * from a previous session (crash or config change) are fully cleaned up. */
    const char *dd[]  = {"ip", "route", "del", "default",    "dev", p->tun.name, NULL};
    const char *d3[]  = {"ip", "route", "del", "0.0.0.0/1",  "dev", p->tun.name, NULL};
    const char *d4[]  = {"ip", "route", "del", "128.0.0.0/1","dev", p->tun.name, NULL};
    (void)run_ip_cmd(dd);
    (void)run_ip_cmd(d3);
    (void)run_ip_cmd(d4);

    if (p->orig_gateway[0]) {
        const char *fl = (p->server_addr.ss_family == AF_INET6) ? "-6" : "-4";
        int pfx = mqvpn_sa_host_prefix(&p->server_addr);
        char hc[INET6_ADDRSTRLEN + 5];
        snprintf(hc, sizeof(hc), "%s/%d", p->server_ip_str, pfx);
        const char *d5[] = {
            "ip",          fl,  "route", "del", hc, "via", p->orig_gateway, "dev",
            p->orig_iface, NULL};
        (void)run_ip_cmd(d5);
    }
    p->routing_configured = 0;
    LOG_INF("split tunnel routes cleaned up");
}
