/*
 * test_ciphers.c — unit tests for TLS cipher suite configuration
 *
 * Tests the tls_ciphers field through both the INI ([TLS] section) and JSON
 * config parsers.  Links only config.c + log.c — no xquic needed.
 *
 * Build:
 *   cc -o tests/test_ciphers tests/test_ciphers.c src/config.c src/log.c \
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

#define ASSERT_EQ_STR(a, b, msg)                                           \
    do {                                                                    \
        if (strcmp((a), (b)) == 0) {                                        \
            g_pass++;                                                       \
        } else {                                                            \
            g_fail++;                                                       \
            fprintf(stderr, "FAIL [%s]: '%s' != '%s'\n", msg, (a), (b));  \
        }                                                                   \
    } while (0)

#define ASSERT_TRUE(cond, msg)                    \
    do {                                          \
        if (cond) {                               \
            g_pass++;                             \
        } else {                                  \
            g_fail++;                             \
            fprintf(stderr, "FAIL [%s]\n", msg);  \
        }                                         \
    } while (0)

/* Write a string to a temp file and return the path. */
static char *
write_tmp(const char *content)
{
    static char path[256];
    snprintf(path, sizeof(path), "/tmp/test_ciphers_XXXXXX");
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
test_cipher_default(void)
{
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);

    ASSERT_EQ_STR(cfg.tls_ciphers, "", "default tls_ciphers is empty string");
}

/* ================================================================
 *  INI — [TLS] section
 * ================================================================ */

static void
test_cipher_ini_cipher_key(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Cipher = TLS_AES_128_GCM_SHA256\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "ini Cipher key: parse ok");
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_128_GCM_SHA256",
                  "ini Cipher key: value stored");
}

static void
test_cipher_ini_ciphers_key(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Ciphers = TLS_AES_256_GCM_SHA384\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "ini Ciphers key: parse ok");
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_256_GCM_SHA384",
                  "ini Ciphers key: value stored");
}

static void
test_cipher_ini_chacha20(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Cipher = TLS_CHACHA20_POLY1305_SHA256\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_CHACHA20_POLY1305_SHA256",
                  "ini Cipher: chacha20 stored");
}

static void
test_cipher_ini_two_ciphers(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Ciphers = TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers,
                  "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256",
                  "ini Ciphers: two ciphers colon-separated");
}

static void
test_cipher_ini_all_three(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Ciphers = TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
                      "TLS_CHACHA20_POLY1305_SHA256\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers,
                  "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
                  "TLS_CHACHA20_POLY1305_SHA256",
                  "ini Ciphers: all three ciphers stored");
}

static void
test_cipher_ini_case_insensitive_key(void)
{
    /* Key matching is case-insensitive: "cipher" should work like "Cipher" */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "cipher = TLS_AES_256_GCM_SHA384\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_256_GCM_SHA384",
                  "ini cipher lowercase key: case-insensitive match");
}

static void
test_cipher_ini_ciphers_uppercase_key(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "CIPHERS = TLS_AES_128_GCM_SHA256\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_128_GCM_SHA256",
                  "ini CIPHERS uppercase key: case-insensitive match");
}

static void
test_cipher_ini_whitespace_trimmed(void)
{
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Cipher =   TLS_AES_128_GCM_SHA256   \n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_128_GCM_SHA256",
                  "ini Cipher: leading/trailing whitespace trimmed");
}

static void
test_cipher_ini_last_value_wins(void)
{
    /* When both Cipher and Ciphers appear, the last one processed wins */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Cipher = TLS_AES_128_GCM_SHA256\n"
                      "Ciphers = TLS_AES_256_GCM_SHA384\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_256_GCM_SHA384",
                  "ini: last cipher key wins");
}

static void
test_cipher_not_affected_by_other_ini_sections(void)
{
    /* No [TLS] section — tls_ciphers stays empty */
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
    ASSERT_EQ_STR(cfg.tls_ciphers, "",
                  "no [TLS] section: tls_ciphers stays empty");
}

static void
test_cipher_ini_does_not_affect_fec(void)
{
    /* Setting TLS cipher must not clobber FEC defaults */
    const char *ini = "[Server]\n"
                      "Address = host:443\n"
                      "[TLS]\n"
                      "Cipher = TLS_AES_128_GCM_SHA256\n";
    char *path = write_tmp(ini);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 0,
                  "cipher ini: fec_enable stays 0");
    ASSERT_EQ_STR(cfg.fec_scheme, "reed_solomon",
                  "cipher ini: fec_scheme stays reed_solomon");
}

/* ================================================================
 *  JSON — tls_ciphers / ciphers / cipher keys
 * ================================================================ */

static void
test_cipher_json_tls_ciphers_key(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"TLS_AES_256_GCM_SHA384\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json tls_ciphers key: parse ok");
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_256_GCM_SHA384",
                  "json tls_ciphers key: value stored");
}

static void
test_cipher_json_ciphers_alias(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"ciphers\":\"TLS_AES_128_GCM_SHA256\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json ciphers alias: parse ok");
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_128_GCM_SHA256",
                  "json ciphers alias: value stored");
}

static void
test_cipher_json_cipher_alias(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"cipher\":\"TLS_CHACHA20_POLY1305_SHA256\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    ASSERT_EQ_INT(mqvpn_config_load(&cfg, path), 0, "json cipher alias: parse ok");
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_CHACHA20_POLY1305_SHA256",
                  "json cipher alias: value stored");
}

static void
test_cipher_json_two_ciphers(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"TLS_AES_128_GCM_SHA256:"
                       "TLS_CHACHA20_POLY1305_SHA256\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers,
                  "TLS_AES_128_GCM_SHA256:TLS_CHACHA20_POLY1305_SHA256",
                  "json tls_ciphers: two ciphers colon-separated");
}

static void
test_cipher_json_all_three(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"TLS_AES_128_GCM_SHA256:"
                       "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers,
                  "TLS_AES_128_GCM_SHA256:TLS_AES_256_GCM_SHA384:"
                  "TLS_CHACHA20_POLY1305_SHA256",
                  "json tls_ciphers: all three ciphers stored");
}

static void
test_cipher_json_alias_cipher_overrides_tls_ciphers(void)
{
    /* Keys are parsed in order: tls_ciphers, then ciphers, then cipher.
     * The last key processed wins. */
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"TLS_AES_128_GCM_SHA256\","
                       "\"cipher\":\"TLS_AES_256_GCM_SHA384\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    /* "cipher" is processed after "tls_ciphers" — it wins */
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_AES_256_GCM_SHA384",
                  "json: cipher alias overrides tls_ciphers (processed last)");
}

static void
test_cipher_json_alias_ciphers_overrides_tls_ciphers(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"TLS_AES_128_GCM_SHA256\","
                       "\"ciphers\":\"TLS_CHACHA20_POLY1305_SHA256\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    /* "ciphers" is processed after "tls_ciphers" — it wins */
    ASSERT_EQ_STR(cfg.tls_ciphers, "TLS_CHACHA20_POLY1305_SHA256",
                  "json: ciphers alias overrides tls_ciphers (processed later)");
}

static void
test_cipher_json_not_set_stays_empty(void)
{
    /* No cipher key in JSON — field must remain empty (uses engine default) */
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"auth_key\":\"secret\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "",
                  "json: no cipher key — tls_ciphers stays empty");
}

static void
test_cipher_json_unknown_cipher_stored_as_is(void)
{
    /* The parser does not validate cipher names — unknown values are stored */
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"UNKNOWN_CIPHER_SUITE\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_STR(cfg.tls_ciphers, "UNKNOWN_CIPHER_SUITE",
                  "json: unknown cipher name stored verbatim");
}

static void
test_cipher_json_does_not_affect_fec(void)
{
    const char *json = "{\"mode\":\"client\","
                       "\"server_addr\":\"host:443\","
                       "\"tls_ciphers\":\"TLS_AES_256_GCM_SHA384\"}";
    char *path = write_tmp(json);
    mqvpn_file_config_t cfg;
    mqvpn_config_defaults(&cfg);
    mqvpn_config_load(&cfg, path);
    unlink(path);
    ASSERT_EQ_INT(cfg.fec_enable, 0,
                  "json cipher: fec_enable stays 0");
    ASSERT_EQ_STR(cfg.fec_scheme, "reed_solomon",
                  "json cipher: fec_scheme stays reed_solomon");
}

/* ================================================================
 *  main
 * ================================================================ */

int
main(void)
{
    printf("test_ciphers:\n");

    /* Default value tests */
    test_cipher_default();

    /* INI — Cipher key */
    test_cipher_ini_cipher_key();
    test_cipher_ini_ciphers_key();
    test_cipher_ini_chacha20();
    test_cipher_ini_two_ciphers();
    test_cipher_ini_all_three();

    /* INI — key case insensitivity */
    test_cipher_ini_case_insensitive_key();
    test_cipher_ini_ciphers_uppercase_key();

    /* INI — whitespace and ordering */
    test_cipher_ini_whitespace_trimmed();
    test_cipher_ini_last_value_wins();

    /* INI — isolation */
    test_cipher_not_affected_by_other_ini_sections();
    test_cipher_ini_does_not_affect_fec();

    /* JSON — tls_ciphers / ciphers / cipher keys */
    test_cipher_json_tls_ciphers_key();
    test_cipher_json_ciphers_alias();
    test_cipher_json_cipher_alias();
    test_cipher_json_two_ciphers();
    test_cipher_json_all_three();

    /* JSON — alias ordering */
    test_cipher_json_alias_cipher_overrides_tls_ciphers();
    test_cipher_json_alias_ciphers_overrides_tls_ciphers();

    /* JSON — edge cases */
    test_cipher_json_not_set_stays_empty();
    test_cipher_json_unknown_cipher_stored_as_is();
    test_cipher_json_does_not_affect_fec();

    printf("\n=== test_ciphers: %d passed, %d failed ===\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
