// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "libmqvpn.h"
#include "mqvpn_internal.h"
#include "mqvpn_scheduler.h"
#include "config.h"

#include <xquic/xquic.h>

#define ASSERT_EQ(a, b)                                                            \
    do {                                                                           \
        if ((a) != (b)) {                                                          \
            fprintf(stderr, "FAIL %s:%d: %s != %s\n", __FILE__, __LINE__, #a, #b); \
            return 1;                                                              \
        }                                                                          \
    } while (0)

#define ASSERT_STR_EQ(a, b)                                                        \
    do {                                                                           \
        if (strcmp((a), (b)) != 0) {                                               \
            fprintf(stderr, "FAIL %s:%d: \"%s\" != \"%s\"\n",                     \
                    __FILE__, __LINE__, (a), (b));                                 \
            return 1;                                                              \
        }                                                                          \
    } while (0)

/* --- scheduler dispatch and properties --- */

static int
test_wrtt_dispatch(void)
{
    xqc_conn_settings_t cs;
    memset(&cs, 0, sizeof(cs));
    mqvpn_apply_scheduler(&cs, MQVPN_SCHED_WRTT);
    ASSERT_EQ(cs.scheduler_callback.xqc_scheduler_get_path,
              xqc_wrtt_scheduler_cb.xqc_scheduler_get_path);
    return 0;
}

static int
test_wrtt_stateless(void)
{
    /* WRTT keeps no per-connection state — size must be 0 */
    ASSERT_EQ(xqc_wrtt_scheduler_cb.xqc_scheduler_size(), (size_t)0);
    return 0;
}

static int
test_wrtt_qos_level(void)
{
    ASSERT_EQ(mqvpn_dgram_qos_level(MQVPN_SCHED_WRTT), XQC_DATA_QOS_HIGH);
    return 0;
}

static int
test_wrtt_no_precondition_warn(void)
{
    /* Single-path WRTT is fine — unlike backup_fec it does not need 2 paths */
    ASSERT_EQ(mqvpn_check_scheduler_preconditions(MQVPN_SCHED_WRTT, 1), false);
    ASSERT_EQ(mqvpn_check_scheduler_preconditions(MQVPN_SCHED_WRTT, 2), false);
    return 0;
}

static int
test_wrr_dispatch(void)
{
    xqc_conn_settings_t cs;
    memset(&cs, 0, sizeof(cs));
    mqvpn_apply_scheduler(&cs, MQVPN_SCHED_WRR);
    ASSERT_EQ(cs.scheduler_callback.xqc_scheduler_get_path,
              xqc_wrr_scheduler_cb.xqc_scheduler_get_path);
    return 0;
}

static int
test_wrr_stateful(void)
{
    /* Unlike WRTT, WRR tracks smooth-WRR rotation state per path — size must
       be nonzero. */
    ASSERT_EQ(xqc_wrr_scheduler_cb.xqc_scheduler_size() > 0, true);
    return 0;
}

static int
test_wrr_qos_level(void)
{
    ASSERT_EQ(mqvpn_dgram_qos_level(MQVPN_SCHED_WRR), XQC_DATA_QOS_HIGH);
    return 0;
}

static int
test_wrr_no_precondition_warn(void)
{
    /* Single-path WRR is fine — unlike backup_fec it does not need 2 paths */
    ASSERT_EQ(mqvpn_check_scheduler_preconditions(MQVPN_SCHED_WRR, 1), false);
    ASSERT_EQ(mqvpn_check_scheduler_preconditions(MQVPN_SCHED_WRR, 2), false);
    return 0;
}

/* --- path descriptor weight field --- */

static int
test_path_desc_weight_zero_by_default(void)
{
    mqvpn_path_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    ASSERT_EQ(desc.weight, 0u);
    return 0;
}

static int
test_path_desc_weight_round_trip(void)
{
    mqvpn_path_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    desc.weight = 7;
    ASSERT_EQ(desc.weight, 7u);
    desc.weight = 1;
    ASSERT_EQ(desc.weight, 1u);
    return 0;
}

/* --- path descriptor dscp_mask field --- */

static int
test_path_desc_dscp_mask_zero_by_default(void)
{
    mqvpn_path_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    ASSERT_EQ(desc.dscp_mask, (uint64_t)0);
    return 0;
}

static int
test_path_desc_dscp_mask_round_trip(void)
{
    mqvpn_path_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* EF (46) + AF41 (34), analogous to a path carrying two DSCP classes. */
    desc.dscp_mask = MQVPN_DSCP_BIT(46) | MQVPN_DSCP_BIT(34);
    ASSERT_EQ(desc.dscp_mask, (1ULL << 46) | (1ULL << 34));
    return 0;
}

static int
test_dscp_bit_masks_low_bits(void)
{
    /* MQVPN_DSCP_BIT masks the input to 6 bits (0-63), same as xquic's
       XQC_DSCP_BIT — a caller passing an out-of-range value should not
       silently overflow into an unrelated bit position. */
    ASSERT_EQ(MQVPN_DSCP_BIT(64), MQVPN_DSCP_BIT(0));
    ASSERT_EQ(MQVPN_DSCP_BIT(70), MQVPN_DSCP_BIT(6));
    return 0;
}

/* --- config file parsing --- */

static char *
write_tmp(const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/test_weight_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return NULL;
    }
    (void)write(fd, content, strlen(content));
    close(fd);
    return path;
}

static int
test_wrtt_config_string(void)
{
    const char *ini = "[Server]\n"
                      "Address = vpn.example.com:443\n"
                      "\n"
                      "[Auth]\n"
                      "Key = testkey\n"
                      "\n"
                      "[Multipath]\n"
                      "Scheduler = wrtt\n"
                      "Path = eth0\n";

    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    int rc = mqvpn_config_load(&cfg, path);
    unlink(path);

    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(cfg.scheduler, "wrtt");
    return 0;
}

/* --- weight constant sanity --- */

static int
test_wrtt_enum_value(void)
{
    /* WRTT must have a distinct value so scheduler dispatch switch is unambiguous */
    ASSERT_EQ((int)MQVPN_SCHED_WRTT, 6);
    return 0;
}

static int
test_wrr_config_string(void)
{
    const char *ini = "[Server]\n"
                      "Address = vpn.example.com:443\n"
                      "\n"
                      "[Auth]\n"
                      "Key = testkey\n"
                      "\n"
                      "[Multipath]\n"
                      "Scheduler = wrr\n"
                      "Path = eth0\n";

    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    int rc = mqvpn_config_load(&cfg, path);
    unlink(path);

    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(cfg.scheduler, "wrr");
    return 0;
}

static int
test_wrr_enum_value(void)
{
    /* WRR must have a distinct value so scheduler dispatch switch is unambiguous */
    ASSERT_EQ((int)MQVPN_SCHED_WRR, 7);
    return 0;
}

static int
test_dscp_config_string(void)
{
    const char *ini = "[Server]\n"
                      "Address = vpn.example.com:443\n"
                      "\n"
                      "[Auth]\n"
                      "Key = testkey\n"
                      "\n"
                      "[Multipath]\n"
                      "Scheduler = dscp\n"
                      "Path = eth0\n";

    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    int rc = mqvpn_config_load(&cfg, path);
    unlink(path);

    ASSERT_EQ(rc, 0);
    ASSERT_STR_EQ(cfg.scheduler, "dscp");
    return 0;
}

int
main(void)
{
    int failed = 0;
    failed += test_wrtt_dispatch();
    failed += test_wrtt_stateless();
    failed += test_wrtt_qos_level();
    failed += test_wrtt_no_precondition_warn();
    failed += test_wrr_dispatch();
    failed += test_wrr_stateful();
    failed += test_wrr_qos_level();
    failed += test_wrr_no_precondition_warn();
    failed += test_path_desc_weight_zero_by_default();
    failed += test_path_desc_weight_round_trip();
    failed += test_path_desc_dscp_mask_zero_by_default();
    failed += test_path_desc_dscp_mask_round_trip();
    failed += test_dscp_bit_masks_low_bits();
    failed += test_wrtt_config_string();
    failed += test_wrtt_enum_value();
    failed += test_wrr_config_string();
    failed += test_wrr_enum_value();
    failed += test_dscp_config_string();
    if (failed) {
        fprintf(stderr, "test_weight: %d FAILED\n", failed);
        return 1;
    }
    fprintf(stderr, "test_weight: PASS\n");
    return 0;
}
