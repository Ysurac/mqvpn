/*
 * test_cc_config.c — unit tests for congestion-control configuration
 *
 * Covers every layer of CC configuration that does not require a live
 * xquic connection:
 *
 *   Group 1: File-config defaults
 *     - default CC string after mqvpn_config_defaults() is "bbr2"
 *
 *   Group 2: JSON config parsing
 *     - each supported CC string round-trips through the JSON parser
 *     - unknown CC string is stored as-is (validated later by main.c)
 *     - CC string longer than the field (15 chars) is safely truncated
 *     - missing "cc" key leaves the default "bbr2"
 *
 *   Group 3: Library API
 *     - mqvpn_config_new() default CC enum is MQVPN_CC_BBR2
 *     - mqvpn_config_set_cc() with each enum value stores the right value
 *     - invalid enum value (out of range) is rejected
 *
 * Links: config.c + log.c + mqvpn_config.c (via mqvpn_lib)
 *
 * Build (standalone, no xquic):
 *   cc -o tests/test_cc_config tests/test_cc_config.c \
 *      src/config.c src/log.c -I src -I include
 *
 * Build (via CMake): see CMakeLists.txt target test_cc_config
 */

#include "config.h"
#include "libmqvpn.h"
#include "mqvpn_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Test infrastructure ─────────────────────────────────────────────────── */

static int g_run    = 0;
static int g_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void)   \
    {                              \
        g_run++;                   \
        printf("  %-68s ", #name); \
        test_##name();             \
        g_passed++;                \
        printf("PASS\n");          \
    }                              \
    static void test_##name(void)

#define ASSERT_STR_EQ(a, b)                                                          \
    do {                                                                             \
        if (strcmp((a), (b)) != 0) {                                                 \
            printf("FAIL\n    %s:%d  \"%s\" != \"%s\"\n", __FILE__, __LINE__,        \
                   (a), (b));                                                        \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

#define ASSERT_EQ(a, b)                                                              \
    do {                                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                          \
        if (_a != _b) {                                                              \
            printf("FAIL\n    %s:%d  %s == %lld, expected %lld\n",                   \
                   __FILE__, __LINE__, #a, _a, _b);                                  \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

#define ASSERT_NE(a, b)                                                              \
    do {                                                                             \
        long long _a = (long long)(a), _b = (long long)(b);                          \
        if (_a == _b) {                                                              \
            printf("FAIL\n    %s:%d  %s == %lld (unexpected equal)\n",               \
                   __FILE__, __LINE__, #a, _a);                                      \
            exit(1);                                                                 \
        }                                                                            \
    } while (0)

/* Write a string to a temp file and return the static path. */
static const char *
write_tmp(const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/test_cc_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) { perror("mkstemp"); exit(1); }
    size_t len = strlen(content);
    if (write(fd, content, len) != (ssize_t)len) { perror("write"); exit(1); }
    close(fd);
    return path;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 1: File-config defaults
 * ═══════════════════════════════════════════════════════════════════════════ */

/* mqvpn_config_defaults() must set cc = "bbr2" */
TEST(file_config_default_cc_is_bbr2)
{
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_STR_EQ(cfg.cc, "bbr2");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 2: JSON config parsing
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Parse a minimal JSON snippet with a single "cc" key. */
static int
parse_cc_json(const char *cc_value, mqvpn_file_config_t *out)
{
    char json[256];
    snprintf(json, sizeof(json),
             "{\"server\":\"127.0.0.1:4433\",\"cc\":\"%s\"}", cc_value);
    const char *path = write_tmp(json);
    mqvpn_config_defaults(out);
    int rc = mqvpn_config_load(out, path);
    unlink(path);
    return rc;
}

TEST(json_cc_bbr2)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("bbr2", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "bbr2");
}

TEST(json_cc_bbr)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("bbr", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "bbr");
}

TEST(json_cc_cubic)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("cubic", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "cubic");
}

TEST(json_cc_new_reno)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("new_reno", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "new_reno");
}

TEST(json_cc_copa)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("copa", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "copa");
}

TEST(json_cc_unlimited)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("unlimited", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "unlimited");
}

/* Unknown CC string is stored verbatim — main.c validates at startup. */
TEST(json_cc_unknown_stored_verbatim)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("latency_magic", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "latency_magic");
}

/* CC string at exactly the field limit (15 chars) fits without truncation. */
TEST(json_cc_exactly_15_chars_fits)
{
    mqvpn_file_config_t cfg;
    /* "new_reno_extra1" is 15 chars — must fit in cfg.cc[16] with NUL */
    ASSERT_EQ(parse_cc_json("new_reno_extra1", &cfg), 0);
    ASSERT_EQ((int)strlen(cfg.cc), 15);
}

/* CC string longer than the field (>15 chars) is safely truncated — no crash. */
TEST(json_cc_overlong_truncated_safely)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_json("bbr2_with_a_very_long_suffix_that_overflows", &cfg), 0);
    /* cfg.cc is char[16], last byte must be NUL */
    ASSERT_EQ(cfg.cc[15], '\0');
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 2b: INI config parsing — [Multipath] CC key
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Parse a minimal INI snippet with CC in the [Multipath] section. */
static int
parse_cc_ini(const char *cc_value, mqvpn_file_config_t *out)
{
    char ini[256];
    snprintf(ini, sizeof(ini),
             "[Server]\nAddress = 127.0.0.1:4433\n[Multipath]\nCC = %s\n",
             cc_value);
    const char *path = write_tmp(ini);
    mqvpn_config_defaults(out);
    int rc = mqvpn_config_load(out, path);
    unlink(path);
    return rc;
}

TEST(ini_cc_bbr2)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_ini("bbr2", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "bbr2");
}

TEST(ini_cc_bbr)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_ini("bbr", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "bbr");
}

TEST(ini_cc_cubic)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_ini("cubic", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "cubic");
}

TEST(ini_cc_new_reno)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_ini("new_reno", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "new_reno");
}

TEST(ini_cc_copa)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_ini("copa", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "copa");
}

TEST(ini_cc_unlimited)
{
    mqvpn_file_config_t cfg;
    ASSERT_EQ(parse_cc_ini("unlimited", &cfg), 0);
    ASSERT_STR_EQ(cfg.cc, "unlimited");
}

/* CongestionControl is an accepted alias for CC. */
TEST(ini_cc_alias_congestion_control)
{
    char ini[256];
    snprintf(ini, sizeof(ini),
             "[Server]\nAddress = 127.0.0.1:4433\n"
             "[Multipath]\nCongestionControl = cubic\n");
    const char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ(mqvpn_config_load(&cfg, path), 0);
    unlink(path);
    ASSERT_STR_EQ(cfg.cc, "cubic");
}

/* Missing CC key in INI leaves the default "bbr2". */
TEST(ini_no_cc_key_keeps_default)
{
    const char *ini = "[Server]\nAddress = 127.0.0.1:4433\n"
                      "[Multipath]\nScheduler = wlb\n";
    const char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ(mqvpn_config_load(&cfg, path), 0);
    unlink(path);
    ASSERT_STR_EQ(cfg.cc, "bbr2");
}

/* ─────────────────────────────────────────────────────────────────────────── */

/* Missing "cc" key leaves the default "bbr2". */
TEST(json_no_cc_key_keeps_default)
{
    const char *json = "{\"server\":\"127.0.0.1:4433\"}";
    const char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ(mqvpn_config_load(&cfg, path), 0);
    unlink(path);
    ASSERT_STR_EQ(cfg.cc, "bbr2");
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Group 3: Library API (mqvpn_config_new / set_cc)
 * ═══════════════════════════════════════════════════════════════════════════ */

/* Default CC after mqvpn_config_new() must be BBR2 (enum 0). */
TEST(api_default_cc_is_bbr2)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    ASSERT_EQ(cfg->cc, MQVPN_CC_BBR2);
    mqvpn_config_free(cfg);
}

/* Each set_cc() call stores the right enum value. */
TEST(api_set_cc_all_algorithms)
{
    mqvpn_config_t *cfg = mqvpn_config_new();

    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_BBR2),     MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_BBR2);

    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_BBR),      MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_BBR);

    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_CUBIC),    MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_CUBIC);

    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_NEW_RENO), MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_NEW_RENO);

    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_COPA),     MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_COPA);

    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_UNLIMITED), MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_UNLIMITED);

    mqvpn_config_free(cfg);
}

/* NULL config pointer must be rejected. */
TEST(api_set_cc_null_config_rejected)
{
    ASSERT_EQ(mqvpn_config_set_cc(NULL, MQVPN_CC_BBR2), MQVPN_ERR_INVALID_ARG);
}

/* Out-of-range enum value must be rejected. */
TEST(api_set_cc_invalid_enum_rejected)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    /* 99 is not a valid mqvpn_cc_t */
    ASSERT_NE(mqvpn_config_set_cc(cfg, (mqvpn_cc_t)99), MQVPN_OK);
    /* Original value must be unchanged */
    ASSERT_EQ(cfg->cc, MQVPN_CC_BBR2);
    mqvpn_config_free(cfg);
}

/* set_cc() is idempotent: setting the same value twice leaves it unchanged. */
TEST(api_set_cc_idempotent)
{
    mqvpn_config_t *cfg = mqvpn_config_new();
    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_CUBIC), MQVPN_OK);
    ASSERT_EQ(mqvpn_config_set_cc(cfg, MQVPN_CC_CUBIC), MQVPN_OK);
    ASSERT_EQ(cfg->cc, MQVPN_CC_CUBIC);
    mqvpn_config_free(cfg);
}

/* ── Runner ── */

int
main(void)
{
    printf("test_cc_config\n");

    printf("\n  Group 1: File-config defaults\n");
    run_file_config_default_cc_is_bbr2();

    printf("\n  Group 2: JSON config parsing\n");
    run_json_cc_bbr2();
    run_json_cc_bbr();
    run_json_cc_cubic();
    run_json_cc_new_reno();
    run_json_cc_copa();
    run_json_cc_unlimited();
    run_json_cc_unknown_stored_verbatim();
    run_json_cc_exactly_15_chars_fits();
    run_json_cc_overlong_truncated_safely();
    run_json_no_cc_key_keeps_default();

    printf("\n  Group 2b: INI config parsing ([Multipath] CC key)\n");
    run_ini_cc_bbr2();
    run_ini_cc_bbr();
    run_ini_cc_cubic();
    run_ini_cc_new_reno();
    run_ini_cc_copa();
    run_ini_cc_unlimited();
    run_ini_cc_alias_congestion_control();
    run_ini_no_cc_key_keeps_default();

    printf("\n  Group 3: Library API\n");
    run_api_default_cc_is_bbr2();
    run_api_set_cc_all_algorithms();
    run_api_set_cc_null_config_rejected();
    run_api_set_cc_invalid_enum_rejected();
    run_api_set_cc_idempotent();

    printf("\n%d/%d tests passed\n", g_passed, g_run);
    return (g_passed == g_run) ? 0 : 1;
}
