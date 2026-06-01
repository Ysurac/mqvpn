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

int
main(void)
{
    int failed = 0;
    failed += test_wrtt_dispatch();
    failed += test_wrtt_stateless();
    failed += test_wrtt_qos_level();
    failed += test_wrtt_no_precondition_warn();
    failed += test_path_desc_weight_zero_by_default();
    failed += test_path_desc_weight_round_trip();
    failed += test_wrtt_config_string();
    failed += test_wrtt_enum_value();
    if (failed) {
        fprintf(stderr, "test_weight: %d FAILED\n", failed);
        return 1;
    }
    fprintf(stderr, "test_weight: PASS\n");
    return 0;
}
