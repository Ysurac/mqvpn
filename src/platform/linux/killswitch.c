// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * killswitch.c — iptables/ip6tables kill switch for Linux
 *
 * Blocks all traffic outside the VPN tunnel. Per address family (iptables
 * and ip6tables alike) one process owns:
 *
 *   OUTPUT          -j MQVPN-KS-<pid>   inserted at position 1
 *   OUTPUT          -j DROP             inserted right after the last
 *                                       MQVPN-KS-* jump in OUTPUT
 *   MQVPN-KS-<pid>  -o <tun> ACCEPT     only while the TUN exists
 *                   -o lo ACCEPT
 *                   ICMPv6 NDP ACCEPT   (ip6tables only: neighbour discovery
 *                                       towards an IPv6 server's gateway)
 *                   -p udp -d <server> --dport <port> ACCEPT
 *                                       (in the server's family only)
 *
 * every rule tagged with the comment mqvpn-ks:<pid>. The ACCEPT chains of
 * every running instance are consulted before any instance's DROP, so two
 * clients with a kill switch (mqvpn-client@ units) do not block each other,
 * and the DROP sits above whatever else OUTPUT holds.
 *
 * Lifecycle: the block (jumps, DROPs, lo and server ACCEPTs) is installed
 * once, as soon as the server address is known and before the first
 * connect, and stays through every reconnect and TUN rebuild; only the TUN
 * ACCEPT follows the TUN (killswitch_tun_up / killswitch_tun_down). It is
 * removed only at final shutdown. A crashed instance's rules are removed by
 * the next instance that starts with the kill switch on
 * (killswitch_flush_stale), which only touches rules of pids that are no
 * longer running.
 *
 * The server-bound UDP ACCEPT deliberately has no -o <iface> match:
 * multipath egresses on every path interface, and a reconnect may pick
 * a different interface than the one discovered at startup.
 *
 * IPv6 is blocked even when the tunnel carries no IPv6. If ip6tables is
 * missing or refuses the rules, setup fails (as for IPv4) unless IPv6 is
 * disabled on the host, in which case there is nothing to leak.
 */

#include "platform_internal.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>

/* -w: wait for the xtables lock instead of failing when another tool
 * (firewalld, docker, a concurrent mqvpn) holds it. */
#define KS_LOCK_WAIT_SEC "5"
#define KS_CHAIN_PREFIX  "MQVPN-KS-"
#define KS_TAG_PREFIX    "mqvpn-ks:"
/* Upper bound for the delete-until-gone loops: a duplicate rule is removed
 * on each pass, and an iptables that keeps saying yes cannot spin us. */
#define KS_DELETE_MAX 16
/* Largest `iptables -S` listing read (busy docker hosts run to a few
 * hundred KiB). */
#define KS_CAPTURE_MAX (4u << 20)

/* Run `iptables|ip6tables -w 5 <args...>`. quiet: discard stderr (deletes
 * that are expected to fail once the rule is gone). 0 on exit status 0. */
static int
run_iptables(int v6, int quiet, const char *const args[])
{
    const char *argv[32];
    int n = 0;
    argv[n++] = v6 ? "ip6tables" : "iptables";
    argv[n++] = "-w";
    argv[n++] = KS_LOCK_WAIT_SEC;
    for (int i = 0; args[i] && n < (int)(sizeof(argv) / sizeof(argv[0])) - 1; i++)
        argv[n++] = args[i];
    argv[n] = NULL;

    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        if (quiet) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 0 : -1;
}

/* Delete one rule spec until iptables reports it gone (duplicates included). */
static void
delete_all(int v6, const char *const args[])
{
    for (int i = 0; i < KS_DELETE_MAX; i++)
        if (run_iptables(v6, 1, args) != 0) break;
}

/* `iptables|ip6tables -w 5 -S [chain]` into a malloc'd NUL-terminated buffer,
 * or NULL (command missing or failed, output too large). */
static char *
capture_rules(int v6, const char *chain)
{
    int fds[2];
    if (pipe(fds) < 0) return NULL;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return NULL;
    }
    if (pid == 0) {
        const char *argv[] = {
            v6 ? "ip6tables" : "iptables", "-w", KS_LOCK_WAIT_SEC, "-S", chain, NULL};
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }
    close(fds[1]);

    size_t cap = 8192, used = 0;
    char *buf = malloc(cap);
    int failed = (buf == NULL);
    while (!failed) {
        if (used + 1 >= cap) {
            if (cap >= KS_CAPTURE_MAX) {
                failed = 1;
                break;
            }
            char *nb = realloc(buf, cap * 2);
            if (!nb) {
                failed = 1;
                break;
            }
            buf = nb;
            cap *= 2;
        }
        ssize_t r = read(fds[0], buf + used, cap - 1 - used);
        if (r < 0) {
            if (errno == EINTR) continue;
            failed = 1;
            break;
        }
        if (r == 0) break;
        used += (size_t)r;
    }
    close(fds[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) {
            failed = 1;
            break;
        }
    if (failed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        free(buf);
        return NULL;
    }
    buf[used] = '\0';
    return buf;
}

/* Position for this process's DROP in OUTPUT: right after the last jump to
 * any MQVPN-KS-* chain (ours was just inserted at 1), so every running
 * instance's ACCEPT chain precedes every DROP. */
static int
drop_position(int v6)
{
    int pos = 2;
    char *out = capture_rules(v6, "OUTPUT");
    if (!out) return pos;
    int idx = 0;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "-A OUTPUT ", 10) != 0) continue;
        idx++;
        if (strstr(line, "-j " KS_CHAIN_PREFIX) && idx + 1 > pos) pos = idx + 1;
    }
    free(out);
    return pos;
}

/* 1 when IPv6 is disabled host-wide (or not compiled in): an ip6tables
 * failure then leaves nothing to leak. */
static int
host_ipv6_disabled(void)
{
    if (access("/proc/net/if_inet6", F_OK) != 0) return 1;
    FILE *fp = fopen("/proc/sys/net/ipv6/conf/all/disable_ipv6", "r");
    if (!fp) return 0;
    int v = 0;
    int ok = fscanf(fp, "%d", &v) == 1;
    fclose(fp);
    return ok && v == 1;
}

static void
remove_family(platform_ctx_t *p, int v6)
{
    const char *jump[] = {"-D",      "OUTPUT",    "-j",          p->ks_chain, "-m",
                          "comment", "--comment", p->ks_comment, NULL};
    delete_all(v6, jump);
    const char *drop[] = {"-D",      "OUTPUT",    "-j",          "DROP", "-m",
                          "comment", "--comment", p->ks_comment, NULL};
    delete_all(v6, drop);
    const char *flush[] = {"-F", p->ks_chain, NULL};
    (void)run_iptables(v6, 1, flush);
    const char *del[] = {"-X", p->ks_chain, NULL};
    (void)run_iptables(v6, 1, del);
}

static int
install_family(platform_ctx_t *p, int v6)
{
    const char *create[] = {"-N", p->ks_chain, NULL};
    if (run_iptables(v6, 1, create) != 0) {
        /* Left over by an earlier process with our pid: start it empty. */
        const char *flush[] = {"-F", p->ks_chain, NULL};
        if (run_iptables(v6, 0, flush) != 0) return -1;
    }
    const char *lo[] = {"-A", p->ks_chain, "-o",        "lo",          "-j", "ACCEPT",
                        "-m", "comment",   "--comment", p->ks_comment, NULL};
    if (run_iptables(v6, 0, lo) != 0) return -1;
    if (v6) {
        /* Router solicitation, neighbour solicitation and advertisement:
         * link-local control traffic an IPv6 server path needs to resolve
         * its gateway. */
        static const char *const ndp[] = {"133", "135", "136"};
        for (size_t i = 0; i < sizeof(ndp) / sizeof(ndp[0]); i++) {
            const char *a[] = {
                "-A",        p->ks_chain,   "-p",     "icmpv6", "--icmpv6-type",
                ndp[i],      "-j",          "ACCEPT", "-m",     "comment",
                "--comment", p->ks_comment, NULL};
            if (run_iptables(v6, 0, a) != 0) return -1;
        }
    }
    const char *jump[] = {"-I", "OUTPUT",  "1",         "-j",          p->ks_chain,
                          "-m", "comment", "--comment", p->ks_comment, NULL};
    if (run_iptables(v6, 0, jump) != 0) return -1;
    char pos[16];
    snprintf(pos, sizeof(pos), "%d", drop_position(v6));
    const char *drop[] = {"-I", "OUTPUT",  pos,         "-j",          "DROP",
                          "-m", "comment", "--comment", p->ks_comment, NULL};
    if (run_iptables(v6, 0, drop) != 0) return -1;
    return 0;
}

int
setup_killswitch(platform_ctx_t *p)
{
    if (!p->killswitch_enabled || p->killswitch_active) return 0;

    snprintf(p->ks_comment, sizeof(p->ks_comment), KS_TAG_PREFIX "%d", (int)getpid());
    snprintf(p->ks_chain, sizeof(p->ks_chain), KS_CHAIN_PREFIX "%d", (int)getpid());
    p->ks_server_rule[0] = '\0';
    p->ks_tun_name[0] = '\0';
    p->ks_tun_v6 = 0;
    p->ks_v6_active = 0;
    /* From here on cleanup_killswitch() undoes whatever got installed. */
    p->killswitch_active = 1;

    if (install_family(p, 0) < 0) {
        LOG_WRN("failed to set up iptables kill switch rules");
        cleanup_killswitch(p);
        return -1;
    }
    if (install_family(p, 1) < 0) {
        remove_family(p, 1);
        if (!host_ipv6_disabled()) {
            LOG_WRN("failed to set up ip6tables kill switch rules; refusing to run "
                    "with IPv6 traffic unblocked");
            cleanup_killswitch(p);
            return -1;
        }
        LOG_WRN("kill switch: ip6tables rules not installed, IPv6 is disabled on "
                "this host");
    } else {
        p->ks_v6_active = 1;
    }
    if (killswitch_update_server(p) < 0) {
        cleanup_killswitch(p);
        return -1;
    }
    LOG_INF("kill switch enabled (comment=%s)", p->ks_comment);
    return 0;
}

int
killswitch_update_server(platform_ctx_t *p)
{
    if (!p->killswitch_active) return 0;

    char ip[INET6_ADDRSTRLEN];
    if (!mqvpn_sa_ntop(&p->server_addr, ip, sizeof(ip))) return -1;
    int v6 = (p->server_addr.ss_family == AF_INET6);
    char cidr[sizeof(p->ks_server_rule)];
    snprintf(cidr, sizeof(cidr), "%s/%d", ip, mqvpn_sa_host_prefix(&p->server_addr));
    if (p->ks_server_rule[0] && p->ks_server_v6 == v6 &&
        strcmp(p->ks_server_rule, cidr) == 0)
        return 0;
    if (v6 && !p->ks_v6_active) {
        LOG_WRN("kill switch: cannot allow IPv6 server %s without ip6tables rules", ip);
        return -1;
    }

    char ps[8];
    snprintf(ps, sizeof(ps), "%d", p->server_port);
    const char *add[] = {"-A", p->ks_chain, "-p",        "udp",         "-d",
                         cidr, "--dport",   ps,          "-j",          "ACCEPT",
                         "-m", "comment",   "--comment", p->ks_comment, NULL};
    if (run_iptables(v6, 0, add) != 0) {
        LOG_WRN("kill switch: failed to allow the server %s", cidr);
        return -1;
    }
    if (p->ks_server_rule[0]) {
        const char *del[] = {"-D",        p->ks_chain,       "-p",      "udp",
                             "-d",        p->ks_server_rule, "--dport", ps,
                             "-j",        "ACCEPT",          "-m",      "comment",
                             "--comment", p->ks_comment,     NULL};
        delete_all(p->ks_server_v6, del);
        LOG_INF("kill switch: server rule moved from %s to %s", p->ks_server_rule, cidr);
    }
    snprintf(p->ks_server_rule, sizeof(p->ks_server_rule), "%s", cidr);
    p->ks_server_v6 = v6;
    return 0;
}

int
killswitch_tun_up(platform_ctx_t *p)
{
    if (!p->killswitch_active) return 0;
    if (p->ks_tun_name[0]) killswitch_tun_down(p);

    snprintf(p->ks_tun_name, sizeof(p->ks_tun_name), "%s", p->tun.name);
    const char *a[] = {"-I",           p->ks_chain, "1",           "-o",
                       p->ks_tun_name, "-j",        "ACCEPT",      "-m",
                       "comment",      "--comment", p->ks_comment, NULL};
    if (run_iptables(0, 0, a) != 0) {
        LOG_WRN("kill switch: failed to allow traffic on %s", p->ks_tun_name);
        p->ks_tun_name[0] = '\0';
        return -1;
    }
    if (p->ks_v6_active && p->has_v6) {
        if (run_iptables(1, 0, a) != 0) {
            LOG_WRN("kill switch: failed to allow IPv6 traffic on %s", p->ks_tun_name);
            killswitch_tun_down(p);
            return -1;
        }
        p->ks_tun_v6 = 1;
    }
    return 0;
}

void
killswitch_tun_down(platform_ctx_t *p)
{
    if (!p->killswitch_active || !p->ks_tun_name[0]) return;
    const char *del[] = {"-D", p->ks_chain, "-o",        p->ks_tun_name, "-j", "ACCEPT",
                         "-m", "comment",   "--comment", p->ks_comment,  NULL};
    delete_all(0, del);
    if (p->ks_tun_v6) delete_all(1, del);
    p->ks_tun_name[0] = '\0';
    p->ks_tun_v6 = 0;
}

void
cleanup_killswitch(platform_ctx_t *p)
{
    if (!p->killswitch_active) return;

    /* Both families unconditionally: the deletes are quiet and idempotent,
     * and a partial ip6tables install is covered without bookkeeping. */
    remove_family(p, 0);
    remove_family(p, 1);
    p->ks_server_rule[0] = '\0';
    p->ks_tun_name[0] = '\0';
    p->ks_tun_v6 = 0;
    p->ks_v6_active = 0;
    p->killswitch_active = 0;
    LOG_INF("kill switch rules removed");
}

/* A tag's pid is stale when no such process runs. Our own pid counts as
 * stale too: killswitch_flush_stale() runs only while this process has no
 * rules installed, so rules carrying our pid were left by an earlier process
 * that had it (pid reuse after a reboot with persisted iptables rules). */
static int
pid_is_stale(long pid)
{
    if (pid <= 0 || pid > 0x7fffffffL) return 0;
    if ((pid_t)pid == getpid()) return 1;
    if (kill((pid_t)pid, 0) == 0) return 0;
    return errno == ESRCH;
}

/* Split an `iptables -S` line into argv in place (quotes stripped: the
 * nft-based iptables quotes comments). Returns argc. */
static int
split_rule(char *line, const char *argv[], int max)
{
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(line, " \t", &save); tok && n < max - 1;
         tok = strtok_r(NULL, " \t", &save)) {
        size_t len = strlen(tok);
        if (len >= 2 && tok[0] == '"' && tok[len - 1] == '"') {
            tok[len - 1] = '\0';
            tok++;
        }
        argv[n++] = tok;
    }
    argv[n] = NULL;
    return n;
}

static void
flush_stale_family(int v6)
{
    const char *bin = v6 ? "ip6tables" : "iptables";
    long logged[8];
    int n_logged = 0;

    /* OUTPUT rules first (jumps, DROPs, and the plain ACCEPT/DROP rules of
     * versions that had no per-process chain): a chain cannot be deleted
     * while a rule still references it. */
    char *out = capture_rules(v6, "OUTPUT");
    if (out) {
        char *save = NULL;
        for (char *line = strtok_r(out, "\n", &save); line;
             line = strtok_r(NULL, "\n", &save)) {
            if (strncmp(line, "-A OUTPUT ", 10) != 0) continue;
            const char *tag = strstr(line, KS_TAG_PREFIX);
            if (!tag) continue;
            long pid = strtol(tag + strlen(KS_TAG_PREFIX), NULL, 10);
            if (!pid_is_stale(pid)) continue;
            int seen = 0;
            for (int i = 0; i < n_logged; i++)
                if (logged[i] == pid) seen = 1;
            if (!seen) {
                LOG_WRN("kill switch: removing stale %s rules of mqvpn pid %ld", bin,
                        pid);
                if (n_logged < (int)(sizeof(logged) / sizeof(logged[0])))
                    logged[n_logged++] = pid;
            }
            const char *argv[64];
            int argc = split_rule(line, argv, (int)(sizeof(argv) / sizeof(argv[0])));
            if (argc < 2) continue;
            argv[0] = "-D";
            (void)run_iptables(v6, 1, argv);
        }
        free(out);
    }

    out = capture_rules(v6, NULL);
    if (!out) return;
    char *save = NULL;
    for (char *line = strtok_r(out, "\n", &save); line;
         line = strtok_r(NULL, "\n", &save)) {
        if (strncmp(line, "-N " KS_CHAIN_PREFIX, 3 + strlen(KS_CHAIN_PREFIX)) != 0)
            continue;
        const char *chain = line + 3;
        char *end = NULL;
        long pid = strtol(chain + strlen(KS_CHAIN_PREFIX), &end, 10);
        if (!end || (*end != '\0' && *end != ' ')) continue;
        if (!pid_is_stale(pid)) continue;
        if (*end == ' ') *end = '\0';
        LOG_WRN("kill switch: removing stale %s chain %s", bin, chain);
        const char *flush[] = {"-F", chain, NULL};
        (void)run_iptables(v6, 1, flush);
        const char *del[] = {"-X", chain, NULL};
        (void)run_iptables(v6, 1, del);
    }
    free(out);
}

void
killswitch_flush_stale(void)
{
    flush_stale_family(0);
    flush_stale_family(1);
}
