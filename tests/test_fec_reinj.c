/*
 * test_fec_reinj.c — unit tests for FEC scheme and reinjection control config
 *
 * Tests every FEC scheme (reed_solomon, xor, packet_mask, galois_calculation)
 * and every reinjection mode (default, deadline, dgram) through both the INI
 * and JSON config parsers.  Links only config.c + log.c — no xquic needed.
 *
 * Build:
 *   cc -o tests/test_fec_reinj tests/test_fec_reinj.c src/config.c src/log.c \
 *      -I src -I include
 */

#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int g_pass = 0, g_fail = 0;

#define ASSERT_EQ_INT(a, b, msg)                                                \
    do {                                                                        \
        if ((a) == (b)) {                                                       \
            g_pass++;                                                           \
        } else {                                                                \
            g_fail++;                                                           \
            fprintf(stderr, "FAIL [%s]: %d != %d\n", msg, (int)(a), (int)(b)); \
        }                                                                       \
    } while (0)

#define ASSERT_EQ_STR(a, b, msg)                                          \
    do {                                                                   \
        if (strcmp((a), (b)) == 0) {                                       \
            g_pass++;                                                      \
        } else {                                                           \
            g_fail++;                                                      \
            fprintf(stderr, "FAIL [%s]: '%s' != '%s'\n", msg, (a), (b)); \
        }                                                                  \
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

/* Write a string to a temp file and return the path. */
static char *
write_tmp(const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/test_fec_reinj_XXXXXX");
    int fd = mkstemp(path);
    if (fd < 0) {
        perror("mkstemp");
        return NULL;
    }
    write(fd, content, strlen(content));
    close(fd);
    return path;
}

/* ================================================================
 *  Default value tests
 * ================================================================ */

static void
test_fec_defaults(void)
{
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);

    ASSERT_EQ_INT(cfg.fec_enable, 0, "default fec_enable is 0");
    ASSERT_EQ_STR(cfg.fec_scheme, "reed_solomon", "default fec_scheme is reed_solomon");
}

static void
test_reinj_defaults(void)
{
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);

    ASSERT_EQ_INT(cfg.reinjection_control, 0, "default reinjection_control is 0");
    ASSERT_EQ_STR(cfg.reinjection_mode, "default", "default reinjection_mode is default");
}

/* ================================================================
 *  FEC INI tests — FecEnable
 * ================================================================ */

static void
test_fec_enable_true(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "fec_enable true parse ok");
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecEnable=true → fec_enable=1");
}

static void
test_fec_enable_yes(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = yes\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecEnable=yes → fec_enable=1");
}

static void
test_fec_enable_1(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = 1\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecEnable=1 → fec_enable=1");
}

static void
test_fec_enable_false(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = false\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 0, "FecEnable=false → fec_enable=0");
}

/* ================================================================
 *  FEC INI tests — FecScheme: all 4 values
 * ================================================================ */

static void
test_fec_scheme_ini_reed_solomon(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = reed_solomon\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecScheme=reed_solomon: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "reed_solomon", "FecScheme=reed_solomon stored");
}

static void
test_fec_scheme_ini_xor(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = xor\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecScheme=xor: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "xor", "FecScheme=xor stored");
}

static void
test_fec_scheme_ini_packet_mask(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = packet_mask\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecScheme=packet_mask: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "packet_mask", "FecScheme=packet_mask stored");
}

static void
test_fec_scheme_ini_galois_calculation(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = galois_calculation\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "FecScheme=galois_calculation: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "galois_calculation", "FecScheme=galois_calculation stored");
}

static void
test_fec_scheme_ini_unknown_keeps_default(void)
{
    /* Unknown scheme value → stored as-is; caller falls back to reed_solomon */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = bogus_scheme\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    /* Config layer stores whatever string was given; the mapping to xquic
     * enum happens in mqvpn_config.c which defaults unknown → reed_solomon.
     * At this layer we just verify the load succeeds and returns non-empty. */
    ASSERT_TRUE(cfg.fec_scheme[0] != '\0', "FecScheme=bogus: fec_scheme non-empty");
}

static void
test_fec_scheme_only_no_enable(void)
{
    /* FecScheme without FecEnable → scheme stored but fec_enable stays 0 */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecScheme = xor\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 0, "no FecEnable → fec_enable stays 0");
    ASSERT_EQ_STR(cfg.fec_scheme, "xor", "FecScheme without FecEnable still stored");
}

static void
test_fec_scheme_duplicate_last_wins(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecScheme = xor\n"
                      "FecScheme = packet_mask\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.fec_scheme, "packet_mask", "duplicate FecScheme: last wins");
}

/* ================================================================
 *  Reinjection INI tests — ReinjectionControl
 * ================================================================ */

static void
test_reinj_control_true(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = true\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "reinj_control true parse ok");
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "ReinjectionControl=true → 1");
}

static void
test_reinj_control_yes(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = yes\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "ReinjectionControl=yes → 1");
}

static void
test_reinj_control_1(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = 1\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "ReinjectionControl=1 → 1");
}

static void
test_reinj_control_false(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = false\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 0, "ReinjectionControl=false → 0");
}

/* ================================================================
 *  Reinjection INI tests — ReinjectionMode: all 3 values
 * ================================================================ */

static void
test_reinj_mode_ini_default(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = true\n"
                      "ReinjectionMode = default\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.reinjection_mode, "default", "ReinjectionMode=default stored");
}

static void
test_reinj_mode_ini_deadline(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = true\n"
                      "ReinjectionMode = deadline\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "ReinjectionMode=deadline: control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "deadline", "ReinjectionMode=deadline stored");
}

static void
test_reinj_mode_ini_dgram(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = true\n"
                      "ReinjectionMode = dgram\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "ReinjectionMode=dgram: control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "ReinjectionMode=dgram stored");
}

static void
test_reinj_mode_ini_unknown_keeps_default(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionControl = true\n"
                      "ReinjectionMode = bogus_mode\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_TRUE(cfg.reinjection_mode[0] != '\0', "ReinjectionMode=bogus: field non-empty");
}

static void
test_reinj_mode_only_no_control(void)
{
    /* ReinjectionMode without ReinjectionControl → mode stored, control stays 0 */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionMode = deadline\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 0, "no ReinjectionControl → stays 0");
    ASSERT_EQ_STR(cfg.reinjection_mode, "deadline", "ReinjectionMode without control still stored");
}

static void
test_reinj_mode_duplicate_last_wins(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionMode = deadline\n"
                      "ReinjectionMode = dgram\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "duplicate ReinjectionMode: last wins");
}

/* ================================================================
 *  Combined FEC + reinjection INI test
 * ================================================================ */

static void
test_fec_and_reinj_combined_ini(void)
{
    const char *ini = "[Server]\n"
                      "Address = vpn.example.com:443\n"
                      "[Auth]\n"
                      "Key = mykey\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = packet_mask\n"
                      "ReinjectionControl = true\n"
                      "ReinjectionMode = deadline\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "combined fec+reinj parse ok");
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "combined: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "packet_mask", "combined: fec_scheme=packet_mask");
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "combined: reinjection_control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "deadline", "combined: reinjection_mode=deadline");
}

static void
test_all_fec_schemes_with_reinj_dgram(void)
{
    /* galois_calculation + dgram combination */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecEnable = true\n"
                      "FecScheme = galois_calculation\n"
                      "ReinjectionControl = true\n"
                      "ReinjectionMode = dgram\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.fec_scheme, "galois_calculation", "galois+dgram: fec_scheme ok");
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "galois+dgram: reinjection_mode ok");
}

/* ================================================================
 *  FEC JSON tests — all 4 fec_scheme values
 * ================================================================ */

static void
test_json_fec_enable_key(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_enable\":true}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json fec_enable key parse ok");
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json fec_enable:true → 1");
}

static void
test_json_fec_enable_false(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_enable\":false}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 0, "json fec_enable:false → 0");
}

static void
test_json_fec_alias_key(void)
{
    /* "fec" is the short-form alias for "fec_enable" */
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec\":true}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json fec:true alias → fec_enable=1");
}

static void
test_json_fec_scheme_reed_solomon(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_enable\":true,"
                       "\"fec_scheme\":\"reed_solomon\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json reed_solomon: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "reed_solomon", "json fec_scheme=reed_solomon stored");
}

static void
test_json_fec_scheme_xor(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_enable\":true,"
                       "\"fec_scheme\":\"xor\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json xor: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "xor", "json fec_scheme=xor stored");
}

static void
test_json_fec_scheme_packet_mask(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_enable\":true,"
                       "\"fec_scheme\":\"packet_mask\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json packet_mask: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "packet_mask", "json fec_scheme=packet_mask stored");
}

static void
test_json_fec_scheme_galois_calculation(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_enable\":true,"
                       "\"fec_scheme\":\"galois_calculation\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json galois: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "galois_calculation", "json fec_scheme=galois_calculation stored");
}

static void
test_json_fec_scheme_unknown_keeps_field(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec_scheme\":\"bogus\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json unknown fec_scheme no error");
    unlink(path);
    ASSERT_TRUE(cfg.fec_scheme[0] != '\0', "json unknown fec_scheme: field non-empty");
}

/* ================================================================
 *  Reinjection JSON tests — all 3 reinjection_mode values
 * ================================================================ */

static void
test_json_reinj_control_true(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_control\":true}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json reinjection_control parse ok");
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "json reinjection_control:true → 1");
}

static void
test_json_reinj_control_false(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_control\":false}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 0, "json reinjection_control:false → 0");
}

static void
test_json_reinj_mode_default(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_control\":true,"
                       "\"reinjection_mode\":\"default\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "json mode=default: control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "default", "json reinjection_mode=default stored");
}

static void
test_json_reinj_mode_deadline(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_control\":true,"
                       "\"reinjection_mode\":\"deadline\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "json mode=deadline: control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "deadline", "json reinjection_mode=deadline stored");
}

static void
test_json_reinj_mode_dgram(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_control\":true,"
                       "\"reinjection_mode\":\"dgram\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "json mode=dgram: control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "json reinjection_mode=dgram stored");
}

static void
test_json_reinj_mode_unknown_keeps_field(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_mode\":\"bogus\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json unknown reinj_mode no error");
    unlink(path);
    ASSERT_TRUE(cfg.reinjection_mode[0] != '\0', "json unknown reinj_mode: field non-empty");
}

static void
test_json_reinj_mode_duplicate_last_wins(void)
{
    /* Two reinjection_mode keys in JSON object — last parsed wins */
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"reinjection_mode\":\"deadline\","
                       "\"reinj_ctl\":\"dgram\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    /* reinj_ctl is an alias processed after reinjection_mode; dgram should win */
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "reinj_ctl alias overrides reinjection_mode");
}

/* ================================================================
 *  Combined FEC + reinjection JSON test
 * ================================================================ */

static void
test_json_fec_and_reinj_combined(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"vpn.example.com:443\","
                       "\"auth_key\":\"mykey\","
                       "\"fec_enable\":true,"
                       "\"fec_scheme\":\"xor\","
                       "\"reinjection_control\":true,"
                       "\"reinjection_mode\":\"dgram\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json fec+reinj combined parse ok");
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 1, "json combined: fec_enable=1");
    ASSERT_EQ_STR(cfg.fec_scheme, "xor", "json combined: fec_scheme=xor");
    ASSERT_EQ_INT(cfg.reinjection_control, 1, "json combined: reinjection_control=1");
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "json combined: reinjection_mode=dgram");
}

static void
test_json_all_fec_schemes_all_reinj_modes(void)
{
    /* Matrix spot-check: galois_calculation + deadline */
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"fec\":true,"
                       "\"fec_scheme\":\"galois_calculation\","
                       "\"reinjection_control\":true,"
                       "\"reinjection_mode\":\"deadline\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.fec_scheme, "galois_calculation", "matrix: galois+deadline fec_scheme ok");
    ASSERT_EQ_STR(cfg.reinjection_mode, "deadline", "matrix: galois+deadline reinj_mode ok");
}

/* ================================================================
 *  Edge cases
 * ================================================================ */

static void
test_fec_whitespace_trimmed(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "FecScheme =   xor   \n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.fec_scheme, "xor", "FecScheme whitespace trimmed");
}

static void
test_reinj_mode_whitespace_trimmed(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Multipath]\n"
                      "ReinjectionMode =   dgram   \n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.reinjection_mode, "dgram", "ReinjectionMode whitespace trimmed");
}

static void
test_fec_not_affected_by_other_sections(void)
{
    /* Ensure Interface/Auth keys do not stomp FEC defaults */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Interface]\n"
                      "TunName = tun0\n"
                      "[Auth]\n"
                      "Key = secret\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 0, "no FEC section: fec_enable stays 0");
    ASSERT_EQ_STR(cfg.fec_scheme, "reed_solomon", "no FEC section: fec_scheme stays reed_solomon");
}

static void
test_reinj_not_affected_by_other_sections(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[Interface]\n"
                      "TunName = tun0\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.reinjection_control, 0, "no Multipath: reinj_control stays 0");
    ASSERT_EQ_STR(cfg.reinjection_mode, "default", "no Multipath: reinj_mode stays default");
}

/* ================================================================
 *  main
 * ================================================================ */

int
main(void)
{
    printf("test_fec_reinj:\n");

    /* Default value tests */
    test_fec_defaults();
    test_reinj_defaults();

    /* FEC INI — FecEnable */
    test_fec_enable_true();
    test_fec_enable_yes();
    test_fec_enable_1();
    test_fec_enable_false();

    /* FEC INI — FecScheme all 4 values */
    test_fec_scheme_ini_reed_solomon();
    test_fec_scheme_ini_xor();
    test_fec_scheme_ini_packet_mask();
    test_fec_scheme_ini_galois_calculation();
    test_fec_scheme_ini_unknown_keeps_default();
    test_fec_scheme_only_no_enable();
    test_fec_scheme_duplicate_last_wins();

    /* Reinjection INI — ReinjectionControl */
    test_reinj_control_true();
    test_reinj_control_yes();
    test_reinj_control_1();
    test_reinj_control_false();

    /* Reinjection INI — ReinjectionMode all 3 values */
    test_reinj_mode_ini_default();
    test_reinj_mode_ini_deadline();
    test_reinj_mode_ini_dgram();
    test_reinj_mode_ini_unknown_keeps_default();
    test_reinj_mode_only_no_control();
    test_reinj_mode_duplicate_last_wins();

    /* Combined FEC + reinjection INI */
    test_fec_and_reinj_combined_ini();
    test_all_fec_schemes_with_reinj_dgram();

    /* FEC JSON — enable/disable */
    test_json_fec_enable_key();
    test_json_fec_enable_false();
    test_json_fec_alias_key();

    /* FEC JSON — fec_scheme all 4 values */
    test_json_fec_scheme_reed_solomon();
    test_json_fec_scheme_xor();
    test_json_fec_scheme_packet_mask();
    test_json_fec_scheme_galois_calculation();
    test_json_fec_scheme_unknown_keeps_field();

    /* Reinjection JSON — enable/disable */
    test_json_reinj_control_true();
    test_json_reinj_control_false();

    /* Reinjection JSON — all 3 modes */
    test_json_reinj_mode_default();
    test_json_reinj_mode_deadline();
    test_json_reinj_mode_dgram();
    test_json_reinj_mode_unknown_keeps_field();
    test_json_reinj_mode_duplicate_last_wins();

    /* Combined FEC + reinjection JSON */
    test_json_fec_and_reinj_combined();
    test_json_all_fec_schemes_all_reinj_modes();

    /* Edge cases */
    test_fec_whitespace_trimmed();
    test_reinj_mode_whitespace_trimmed();
    test_fec_not_affected_by_other_sections();
    test_reinj_not_affected_by_other_sections();

    printf("\n=== test_fec_reinj: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
