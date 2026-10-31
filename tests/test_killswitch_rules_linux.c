// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_killswitch_rules_linux.c — PATH-stub integration tests for the Linux
 * iptables/ip6tables kill switch (src/platform/linux/killswitch.c):
 * setup_killswitch(), the TUN / server updates, cleanup_killswitch() and the
 * stale-rule flush driven end to end against fake `iptables`/`ip6tables` on
 * PATH (see tests/fake_cmd.h). Linux port of test_killswitch_rules_darwin.c.
 *
 * Every invocation carries `-w 5`, so the verb is the fake's "$3". The fakes
 * make every `-D` (delete) exit 1 AFTER logging: the delete-until-gone loops
 * stop after exactly one logged attempt while still recording the exact rule
 * shape being removed. `-S` prints the canned listing named by
 * $MQVPN_FAKE_IPT_S_CHAIN_FILE (`-S OUTPUT`) or $MQVPN_FAKE_IPT_S_ALL_FILE
 * (`-S`), nothing when unset. $MQVPN_FAKE_FAIL_SUBSTR stays free for
 * per-scenario failures.
 *
 * Every rule needle is pinned to the FULL logged line including the
 * PID-qualified comment tag and chain: the test runs setup_killswitch()
 * in-process, so getpid() here equals the pid production stamps into
 * ks_comment / ks_chain.
 */
#include "platform_internal.h"
#include "fake_cmd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_pass = 0, g_fail = 0;

#define ASSERT_EQ_INT(a, b, msg)                                               \
    do {                                                                       \
        if ((a) == (b)) {                                                      \
            g_pass++;                                                          \
        } else {                                                               \
            g_fail++;                                                          \
            fprintf(stderr, "FAIL [%s]: %d != %d\n", msg, (int)(a), (int)(b)); \
        }                                                                      \
    } while (0)

#define ASSERT_TRUE(cond, msg)                   \
    do {                                         \
        if (cond) {                              \
            g_pass++;                            \
        } else {                                 \
            g_fail++;                            \
            fprintf(stderr, "FAIL [%s]\n", msg); \
        }                                        \
    } while (0)

/* Order-pinning assertion over the fake-cmd log; matching semantics live
 * in fake_cmd_log_order() (tests/fake_cmd.h). */
static void
assert_log_order(const char *log, const char *const *needles, int n, const char *tag)
{
    if (fake_cmd_log_order(log, needles, n, tag) == 0)
        g_pass++;
    else
        g_fail++;
}

/* "mqvpn-ks:<pid>" / "MQVPN-KS-<pid>" — match production's ks_comment and
 * ks_chain because the functions under test run in this very process. */
static char g_tag[64];
static char g_chain[64];

#define NEEDLE_MAX 256
#define LOG_MAX    16384

/* "<bin>|-w|5|<rule>|-m|comment|--comment|<tag>" — the full logged line of
 * a tagged rule; `rule` may contain "%C" for the chain name. */
static void
tagged(char *buf, const char *bin, const char *rule)
{
    char r[NEEDLE_MAX];
    const char *c = strstr(rule, "%C");
    if (c)
        snprintf(r, sizeof(r), "%.*s%s%s", (int)(c - rule), rule, g_chain, c + 2);
    else
        snprintf(r, sizeof(r), "%s", rule);
    snprintf(buf, NEEDLE_MAX, "%s|-w|5|%s|-m|comment|--comment|%s", bin, r, g_tag);
}

/* "<bin>|-w|5|<verb>|<chain>" — untagged chain verbs (-N/-F/-X). */
static void
chain_cmd(char *buf, const char *bin, const char *verb, const char *chain)
{
    snprintf(buf, NEEDLE_MAX, "%s|-w|5|%s|%s\n", bin, verb, chain);
}

static int
host_v6_disabled(void)
{
    if (access("/proc/net/if_inet6", F_OK) != 0) return 1;
    FILE *fp = fopen("/proc/sys/net/ipv6/conf/all/disable_ipv6", "r");
    if (!fp) return 0;
    int v = 0;
    int ok = fscanf(fp, "%d", &v) == 1;
    fclose(fp);
    return ok && v == 1;
}

static char g_s_chain_file[FAKE_CMD_PATH_MAX];
static char g_s_all_file[FAKE_CMD_PATH_MAX];

static void
setup_fixture(fake_cmd_env_t *e)
{
    const char *body = "case \"$3\" in\n"
                       "  -D) exit 1 ;;\n"
                       "  -S)\n"
                       "    if [ -n \"$4\" ]; then f=\"$MQVPN_FAKE_IPT_S_CHAIN_FILE\";\n"
                       "    else f=\"$MQVPN_FAKE_IPT_S_ALL_FILE\"; fi\n"
                       "    [ -n \"$f\" ] && cat \"$f\"\n"
                       "    ;;\n"
                       "esac";
    fake_cmd_install(e, "iptables", body);
    fake_cmd_install(e, "ip6tables", body);
}

static void
set_listings(fake_cmd_env_t *e, const char *chain_listing, const char *all_listing)
{
    unsetenv("MQVPN_FAKE_IPT_S_CHAIN_FILE");
    unsetenv("MQVPN_FAKE_IPT_S_ALL_FILE");
    if (chain_listing) {
        fake_cmd_write_content_file(e, "s_chain.txt", chain_listing, g_s_chain_file,
                                    sizeof(g_s_chain_file));
        setenv("MQVPN_FAKE_IPT_S_CHAIN_FILE", g_s_chain_file, 1);
    }
    if (all_listing) {
        fake_cmd_write_content_file(e, "s_all.txt", all_listing, g_s_all_file,
                                    sizeof(g_s_all_file));
        setenv("MQVPN_FAKE_IPT_S_ALL_FILE", g_s_all_file, 1);
    }
}

static void
init_ctx(platform_ctx_t *p, const char *server)
{
    memset(p, 0, sizeof(*p));
    snprintf(p->tun.name, sizeof(p->tun.name), "mqvpn0");
    p->killswitch_enabled = 1;
    p->server_port = 51820;

    socklen_t out_len = 0;
    mqvpn_resolve_host(server, &p->server_addr, &out_len);
    p->server_addrlen = out_len;
    mqvpn_sa_set_port(&p->server_addr, 51820);
    /* server_ip_str deliberately left empty: the kill switch must not depend
     * on setup_routes() having run (KillSwitch + NoRoutes). */
}

/* ================================================================
 * 1. v4 server: block installed before any TUN, both families, server
 *    ACCEPT in v4 only; TUN ACCEPT follows the TUN; symmetric cleanup
 * ================================================================ */
static void
test_v4_basic(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");

    int rc = setup_killswitch(&p);
    ASSERT_EQ_INT(rc, 0, "v4 setup rc");
    ASSERT_EQ_INT(p.killswitch_active, 1, "v4 killswitch_active");
    ASSERT_TRUE(strcmp(p.ks_comment, g_tag) == 0, "v4 comment is mqvpn-ks:<pid>");
    ASSERT_TRUE(strcmp(p.ks_chain, g_chain) == 0, "v4 chain is MQVPN-KS-<pid>");

    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char n[12][NEEDLE_MAX];
    chain_cmd(n[0], "iptables", "-N", g_chain);
    tagged(n[1], "iptables", "-A|%C|-o|lo|-j|ACCEPT");
    tagged(n[2], "iptables", "-I|OUTPUT|1|-j|%C");
    snprintf(n[3], NEEDLE_MAX, "iptables|-w|5|-S|OUTPUT");
    tagged(n[4], "iptables", "-I|OUTPUT|2|-j|DROP");
    chain_cmd(n[5], "ip6tables", "-N", g_chain);
    tagged(n[6], "ip6tables", "-A|%C|-o|lo|-j|ACCEPT");
    tagged(n[7], "ip6tables", "-A|%C|-p|icmpv6|--icmpv6-type|135|-j|ACCEPT");
    tagged(n[8], "ip6tables", "-I|OUTPUT|1|-j|%C");
    tagged(n[9], "ip6tables", "-I|OUTPUT|2|-j|DROP");
    tagged(n[10], "iptables", "-A|%C|-p|udp|-d|203.0.113.9/32|--dport|51820|-j|ACCEPT");
    const char *seq[] = {n[0], n[1], n[2], n[3], n[4], n[5],
                         n[6], n[7], n[8], n[9], n[10]};
    assert_log_order(log, seq, 11, "v4 setup rule order");
    char v6udp[NEEDLE_MAX];
    snprintf(v6udp, sizeof(v6udp), "ip6tables|-w|5|-A|%s|-p|udp", g_chain);
    ASSERT_TRUE(strstr(log, v6udp) == NULL, "v4 server: no ip6tables server ACCEPT");
    ASSERT_TRUE(strstr(log, "-o|mqvpn0") == NULL, "no TUN rule before the TUN exists");

    /* TUN lifecycle: ACCEPT added at the top of the chain, removed again. */
    fake_cmd_reset(e);
    ASSERT_EQ_INT(killswitch_tun_up(&p), 0, "v4 tun_up rc");
    fake_cmd_read_log(e, log, sizeof(log));
    tagged(n[0], "iptables", "-I|%C|1|-o|mqvpn0|-j|ACCEPT");
    ASSERT_TRUE(strstr(log, n[0]) != NULL, "tun_up inserts the v4 TUN ACCEPT");
    ASSERT_TRUE(strstr(log, "ip6tables") == NULL, "tun_up: no v6 TUN ACCEPT (has_v6=0)");

    fake_cmd_reset(e);
    killswitch_tun_down(&p);
    ASSERT_EQ_INT(p.killswitch_active, 1, "tun_down keeps the block");
    fake_cmd_read_log(e, log, sizeof(log));
    tagged(n[0], "iptables", "-D|%C|-o|mqvpn0|-j|ACCEPT");
    ASSERT_TRUE(strstr(log, n[0]) != NULL, "tun_down deletes the v4 TUN ACCEPT");
    ASSERT_TRUE(strstr(log, "|DROP|") == NULL && strstr(log, "|-X|") == NULL,
                "tun_down leaves the DROP and the chain alone");

    fake_cmd_reset(e);
    cleanup_killswitch(&p);
    ASSERT_EQ_INT(p.killswitch_active, 0, "v4 cleanup clears killswitch_active");
    fake_cmd_read_log(e, log, sizeof(log));
    tagged(n[0], "iptables", "-D|OUTPUT|-j|%C");
    tagged(n[1], "iptables", "-D|OUTPUT|-j|DROP");
    chain_cmd(n[2], "iptables", "-F", g_chain);
    chain_cmd(n[3], "iptables", "-X", g_chain);
    tagged(n[4], "ip6tables", "-D|OUTPUT|-j|%C");
    tagged(n[5], "ip6tables", "-D|OUTPUT|-j|DROP");
    chain_cmd(n[6], "ip6tables", "-F", g_chain);
    chain_cmd(n[7], "ip6tables", "-X", g_chain);
    const char *cseq[] = {n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]};
    assert_log_order(log, cseq, 8, "v4 cleanup rule order");
}

/* ================================================================
 * 2. v4 insert fails -> rc -1, self-cleanup runs, active ends 0
 * ================================================================ */
static void
test_insert_failure_self_cleanup(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    fake_cmd_set_fail_substr("iptables|-w|5|-I|OUTPUT|1");

    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");

    int rc = setup_killswitch(&p);
    ASSERT_EQ_INT(rc, -1, "insert-failure rc");
    ASSERT_EQ_INT(p.killswitch_active, 0, "insert-failure ends inactive");

    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char d1[NEEDLE_MAX], d2[NEEDLE_MAX];
    tagged(d1, "iptables", "-D|OUTPUT|-j|DROP");
    chain_cmd(d2, "iptables", "-X", g_chain);
    ASSERT_TRUE(strstr(log, d1) != NULL, "insert-failure: self-cleanup deletes the DROP");
    ASSERT_TRUE(strstr(log, d2) != NULL,
                "insert-failure: self-cleanup deletes the chain");

    fake_cmd_clear_fail();
}

/* ================================================================
 * 2b. server-accept insert fails after the block -> self-cleanup
 * ================================================================ */
static void
test_server_accept_failure_self_cleanup(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    fake_cmd_set_fail_substr("-p|udp");

    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");

    int rc = setup_killswitch(&p);
    ASSERT_EQ_INT(rc, -1, "server-accept-failure rc");
    ASSERT_EQ_INT(p.killswitch_active, 0, "server-accept-failure ends inactive");

    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char n1[NEEDLE_MAX], d1[NEEDLE_MAX];
    tagged(n1, "iptables", "-I|OUTPUT|2|-j|DROP"); /* block fully installed */
    tagged(d1, "iptables", "-D|OUTPUT|-j|DROP");   /* then torn down again */
    const char *seq[] = {n1, d1};
    assert_log_order(log, seq, 2, "server-accept-failure install-then-teardown");

    fake_cmd_clear_fail();
}

/* ================================================================
 * 3. ip6tables refuses the rules: fail closed unless the host has IPv6
 *    disabled (then IPv4 only, rc 0)
 * ================================================================ */
static void
test_v6_failure(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    fake_cmd_set_fail_substr("ip6tables|-w|5|-I|OUTPUT|1");

    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");

    int rc = setup_killswitch(&p);
    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char d1[NEEDLE_MAX];
    chain_cmd(d1, "ip6tables", "-X", g_chain);
    ASSERT_TRUE(strstr(log, d1) != NULL, "v6-failure: partial ip6tables install removed");
    if (host_v6_disabled()) {
        ASSERT_EQ_INT(rc, 0, "v6-failure on an IPv6-less host: continue IPv4-only");
        ASSERT_EQ_INT(p.ks_v6_active, 0, "v6-failure: v6 half inactive");
        cleanup_killswitch(&p);
    } else {
        ASSERT_EQ_INT(rc, -1, "v6-failure with IPv6 enabled: fail closed");
        ASSERT_EQ_INT(p.killswitch_active, 0, "v6-failure ends inactive");
    }
    fake_cmd_clear_fail();
}

/* ================================================================
 * 4. v6 server: server ACCEPT in ip6tables; v6 data plane TUN ACCEPT
 * ================================================================ */
static void
test_v6_server(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    platform_ctx_t p;
    init_ctx(&p, "2001:db8::9");

    int rc = setup_killswitch(&p);
    ASSERT_EQ_INT(rc, 0, "v6-server setup rc");

    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char n1[NEEDLE_MAX], n2[NEEDLE_MAX];
    tagged(n1, "ip6tables", "-I|OUTPUT|2|-j|DROP");
    tagged(n2, "ip6tables", "-A|%C|-p|udp|-d|2001:db8::9/128|--dport|51820|-j|ACCEPT");
    const char *seq[] = {n1, n2};
    assert_log_order(log, seq, 2, "v6-server rule order");
    char v4udp[NEEDLE_MAX];
    snprintf(v4udp, sizeof(v4udp), "\niptables|-w|5|-A|%s|-p|udp", g_chain);
    ASSERT_TRUE(strstr(log, v4udp) == NULL, "v6 server: no iptables server ACCEPT");
    tagged(n1, "iptables", "-I|OUTPUT|2|-j|DROP");
    ASSERT_TRUE(strstr(log, n1) != NULL, "v6 server: IPv4 is blocked too");

    fake_cmd_reset(e);
    p.has_v6 = 1;
    ASSERT_EQ_INT(killswitch_tun_up(&p), 0, "v6 tun_up rc");
    fake_cmd_read_log(e, log, sizeof(log));
    tagged(n1, "iptables", "-I|%C|1|-o|mqvpn0|-j|ACCEPT");
    tagged(n2, "ip6tables", "-I|%C|1|-o|mqvpn0|-j|ACCEPT");
    const char *tseq[] = {n1, n2};
    assert_log_order(log, tseq, 2, "v6 tun_up: v4 then v6 TUN ACCEPT");

    fake_cmd_reset(e);
    killswitch_tun_down(&p);
    fake_cmd_read_log(e, log, sizeof(log));
    tagged(n2, "ip6tables", "-D|%C|-o|mqvpn0|-j|ACCEPT");
    ASSERT_TRUE(strstr(log, n2) != NULL, "v6 tun_down deletes the v6 TUN ACCEPT");

    fake_cmd_reset(e);
    cleanup_killswitch(&p);
    ASSERT_EQ_INT(p.killswitch_active, 0, "v6-server cleanup clears active");
}

/* ================================================================
 * 5. server re-resolution: new ACCEPT first, then the old one goes
 * ================================================================ */
static void
test_update_server(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");
    ASSERT_EQ_INT(setup_killswitch(&p), 0, "update-server setup rc");

    fake_cmd_reset(e);
    ASSERT_EQ_INT(killswitch_update_server(&p), 0, "same address: rc 0");
    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    ASSERT_TRUE(log[0] == '\0', "same address: no commands");

    socklen_t len = 0;
    mqvpn_resolve_host("198.51.100.7", &p.server_addr, &len);
    mqvpn_sa_set_port(&p.server_addr, 51820);
    fake_cmd_reset(e);
    ASSERT_EQ_INT(killswitch_update_server(&p), 0, "new address: rc 0");
    fake_cmd_read_log(e, log, sizeof(log));
    char n1[NEEDLE_MAX], n2[NEEDLE_MAX];
    tagged(n1, "iptables", "-A|%C|-p|udp|-d|198.51.100.7/32|--dport|51820|-j|ACCEPT");
    tagged(n2, "iptables", "-D|%C|-p|udp|-d|203.0.113.9/32|--dport|51820|-j|ACCEPT");
    const char *seq[] = {n1, n2};
    assert_log_order(log, seq, 2, "update-server add-then-delete");
    ASSERT_TRUE(strcmp(p.ks_server_rule, "198.51.100.7/32") == 0, "server rule recorded");

    fake_cmd_reset(e);
    cleanup_killswitch(&p);
}

/* ================================================================
 * 6. DROP lands after every running instance's jump
 * ================================================================ */
static void
test_drop_after_other_instances(fake_cmd_env_t *e)
{
    fake_cmd_reset(e);
    char listing[512];
    snprintf(listing, sizeof(listing),
             "-P OUTPUT ACCEPT\n"
             "-A OUTPUT -m comment --comment \"%s\" -j %s\n"
             "-A OUTPUT -m comment --comment \"mqvpn-ks:1\" -j MQVPN-KS-1\n"
             "-A OUTPUT -m comment --comment \"mqvpn-ks:1\" -j DROP\n"
             "-A OUTPUT -o eth0 -j ACCEPT\n",
             g_tag, g_chain);
    set_listings(e, listing, NULL);

    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");
    ASSERT_EQ_INT(setup_killswitch(&p), 0, "drop-position setup rc");
    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char n1[NEEDLE_MAX];
    tagged(n1, "iptables", "-I|OUTPUT|3|-j|DROP");
    ASSERT_TRUE(strstr(log, n1) != NULL, "DROP inserted after the last MQVPN-KS jump");

    fake_cmd_reset(e);
    set_listings(e, NULL, NULL);
    cleanup_killswitch(&p);
}

/* ================================================================
 * 7. stale flush: rules and chains of dead pids go, live pids' stay
 * ================================================================ */
static void
test_flush_stale(fake_cmd_env_t *e)
{
    pid_t dead = fork();
    if (dead == 0) _exit(0);
    waitpid(dead, NULL, 0);
    pid_t live = getppid();

    char out_listing[1024], all_listing[512];
    snprintf(out_listing, sizeof(out_listing),
             "-P OUTPUT ACCEPT\n"
             "-A OUTPUT -m comment --comment \"mqvpn-ks:%d\" -j MQVPN-KS-%d\n"
             "-A OUTPUT -m comment --comment \"mqvpn-ks:%d\" -j MQVPN-KS-%d\n"
             "-A OUTPUT -o lo -m comment --comment mqvpn-ks:%d -j ACCEPT\n"
             "-A OUTPUT -m comment --comment \"mqvpn-ks:%d\" -j DROP\n"
             "-A OUTPUT -m comment --comment \"mqvpn-ks-test:%d\" -j ACCEPT\n",
             (int)live, (int)live, (int)dead, (int)dead, (int)dead, (int)live, (int)dead);
    snprintf(all_listing, sizeof(all_listing),
             "-P INPUT ACCEPT\n-P OUTPUT ACCEPT\n-N MQVPN-KS-%d\n-N MQVPN-KS-%d\n"
             "-N OTHER-CHAIN\n",
             (int)live, (int)dead);

    fake_cmd_reset(e);
    set_listings(e, out_listing, all_listing);
    killswitch_flush_stale();

    char log[LOG_MAX];
    fake_cmd_read_log(e, log, sizeof(log));
    char n[6][NEEDLE_MAX];
    snprintf(n[0], NEEDLE_MAX,
             "iptables|-w|5|-D|OUTPUT|-m|comment|--comment|mqvpn-ks:%d|-j|MQVPN-KS-%d",
             (int)dead, (int)dead);
    snprintf(n[1], NEEDLE_MAX,
             "iptables|-w|5|-D|OUTPUT|-o|lo|-m|comment|--comment|mqvpn-ks:%d|-j|ACCEPT",
             (int)dead);
    snprintf(n[2], NEEDLE_MAX, "iptables|-w|5|-F|MQVPN-KS-%d\n", (int)dead);
    snprintf(n[3], NEEDLE_MAX, "iptables|-w|5|-X|MQVPN-KS-%d\n", (int)dead);
    snprintf(n[4], NEEDLE_MAX, "ip6tables|-w|5|-X|MQVPN-KS-%d\n", (int)dead);
    const char *seq[] = {n[0], n[1], n[2], n[3], n[4]};
    assert_log_order(log, seq, 5, "stale flush removes the dead pid's rules, then chain");

    char live_tag[64];
    snprintf(live_tag, sizeof(live_tag), "mqvpn-ks:%d|", (int)live);
    char live_chain[64];
    snprintf(live_chain, sizeof(live_chain), "MQVPN-KS-%d\n", (int)live);
    ASSERT_TRUE(strstr(log, live_tag) == NULL, "stale flush keeps a live pid's rules");
    ASSERT_TRUE(strstr(log, live_chain) == NULL, "stale flush keeps a live pid's chain");
    ASSERT_TRUE(strstr(log, "mqvpn-ks-test") == NULL, "stale flush ignores other tags");
    ASSERT_TRUE(strstr(log, "OTHER-CHAIN") == NULL, "stale flush ignores other chains");

    set_listings(e, NULL, NULL);
}

/* ================================================================
 * 8. gating: disabled or already-active setup is a silent no-op
 * ================================================================ */
static void
test_gating_noops(fake_cmd_env_t *e)
{
    char log[LOG_MAX];
    set_listings(e, NULL, NULL);

    fake_cmd_reset(e);
    platform_ctx_t p;
    init_ctx(&p, "203.0.113.9");
    p.killswitch_enabled = 0;
    ASSERT_EQ_INT(setup_killswitch(&p), 0, "disabled setup rc");
    ASSERT_EQ_INT(killswitch_tun_up(&p), 0, "disabled tun_up rc");
    killswitch_tun_down(&p);
    ASSERT_EQ_INT(killswitch_update_server(&p), 0, "disabled update_server rc");
    fake_cmd_read_log(e, log, sizeof(log));
    ASSERT_TRUE(log[0] == '\0', "disabled kill switch runs no commands");

    fake_cmd_reset(e);
    init_ctx(&p, "203.0.113.9");
    p.killswitch_active = 1; /* already set up */
    ASSERT_EQ_INT(setup_killswitch(&p), 0, "already-active setup rc");
    fake_cmd_read_log(e, log, sizeof(log));
    ASSERT_TRUE(log[0] == '\0', "already-active setup runs no commands");

    /* cleanup on a never-activated ctx is likewise a no-op */
    fake_cmd_reset(e);
    init_ctx(&p, "203.0.113.9");
    cleanup_killswitch(&p);
    fake_cmd_read_log(e, log, sizeof(log));
    ASSERT_TRUE(log[0] == '\0', "inactive cleanup runs no commands");
}

int
main(void)
{
    snprintf(g_tag, sizeof(g_tag), "mqvpn-ks:%d", (int)getpid());
    snprintf(g_chain, sizeof(g_chain), "MQVPN-KS-%d", (int)getpid());

    fake_cmd_env_t e;
    if (fake_cmd_env_init(&e) < 0) {
        fprintf(stderr, "fake_cmd_env_init failed\n");
        return 1;
    }
    setup_fixture(&e);

    test_v4_basic(&e);
    test_insert_failure_self_cleanup(&e);
    test_server_accept_failure_self_cleanup(&e);
    test_v6_failure(&e);
    test_v6_server(&e);
    test_update_server(&e);
    test_drop_after_other_instances(&e);
    test_flush_stale(&e);
    test_gating_noops(&e);

    fake_cmd_env_cleanup(&e);

    printf("\n=== test_killswitch_rules_linux: %d passed, %d failed ===\n", g_pass,
           g_fail);
    return g_fail ? 1 : 0;
}
