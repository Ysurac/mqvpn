// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * test_path_label.c — Unit tests for mqvpn_path_label_encode/decode
 * (PATH_LABEL capsule payload). See mqvpn_path_label.h for the why.
 */

#undef NDEBUG
#include <assert.h>

#include <stdio.h>
#include <string.h>

#include "mqvpn_path_label.h"

static int tests_passed = 0;
static int tests_failed = 0;

#define TEST(name)                 \
    do {                           \
        printf("  %-50s ", #name); \
    } while (0)

#define PASS()            \
    do {                  \
        printf("PASS\n"); \
        tests_passed++;   \
    } while (0)

#define FAIL(msg)                  \
    do {                           \
        printf("FAIL: %s\n", msg); \
        tests_failed++;            \
    } while (0)

#define ASSERT_EQ(a, b)                                                             \
    do {                                                                            \
        if ((a) != (b)) {                                                           \
            char _buf[128];                                                        \
            snprintf(_buf, sizeof(_buf), "expected %lld, got %lld", (long long)(b), \
                     (long long)(a));                                               \
            FAIL(_buf);                                                            \
            return;                                                                \
        }                                                                          \
    } while (0)

static void
test_roundtrip_basic(void)
{
    TEST(roundtrip basic iface + weight + dscp_mask);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 5, "wlan0", 10, 1ULL << 46);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id = 0, dscp_mask = 0;
    uint32_t weight = 0;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface, sizeof(iface), &weight,
                                     &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(path_id, 5);
    ASSERT_EQ(weight, 10);
    ASSERT_EQ(dscp_mask, 1ULL << 46);
    if (strcmp(iface, "wlan0") != 0) {
        FAIL("iface mismatch");
        return;
    }

    PASS();
}

static void
test_roundtrip_zero_weight_and_mask(void)
{
    TEST(roundtrip zero weight / dscp_mask(no dedicated value));

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 1, "eth0", 0, 0);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id = 0, dscp_mask = 123; /* pre-dirty */
    uint32_t weight = 456;                 /* pre-dirty */
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface, sizeof(iface), &weight,
                                     &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(weight, 0);
    ASSERT_EQ(dscp_mask, 0);

    PASS();
}

static void
test_roundtrip_max_len_iface(void)
{
    TEST(roundtrip max-length iface(15 chars));

    const char *iface15 = "abcdefghijklmno"; /* 15 chars */
    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 0xdeadbeefULL, iface15, 1, 2);
    ASSERT_EQ(n, (int)(8 + 1 + strlen(iface15) + 4 + 8));

    uint64_t path_id = 0, dscp_mask = 0;
    uint32_t weight = 0;
    char iface_out[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface_out, sizeof(iface_out),
                                     &weight, &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(path_id, 0xdeadbeefULL);
    ASSERT_EQ(weight, 1);
    ASSERT_EQ(dscp_mask, 2);
    if (strcmp(iface_out, iface15) != 0) {
        FAIL("iface mismatch");
        return;
    }

    PASS();
}

static void
test_roundtrip_path_id_zero(void)
{
    TEST(roundtrip path_id 0);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 0, "eth0", 0, 0);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id = 123, dscp_mask = 0; /* pre-dirty */
    uint32_t weight = 0;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface, sizeof(iface), &weight,
                                     &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(path_id, 0);

    PASS();
}

static void
test_roundtrip_max_weight_and_mask(void)
{
    TEST(roundtrip max weight(u32) and dscp_mask(u64));

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    uint32_t max_weight = 0xFFFFFFFFU;
    uint64_t max_mask = 0xFFFFFFFFFFFFFFFFULL;
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 9, "ppp0", max_weight, max_mask);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id = 0, dscp_mask = 0;
    uint32_t weight = 0;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface, sizeof(iface), &weight,
                                     &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(weight, max_weight);
    ASSERT_EQ(dscp_mask, max_mask);

    PASS();
}

static void
test_encode_rejects_empty_iface(void)
{
    TEST(encode rejects empty iface);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    ASSERT_EQ(mqvpn_path_label_encode(buf, sizeof(buf), 1, "", 0, 0), -1);

    PASS();
}

static void
test_encode_rejects_null_iface(void)
{
    TEST(encode rejects NULL iface);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    ASSERT_EQ(mqvpn_path_label_encode(buf, sizeof(buf), 1, NULL, 0, 0), -1);

    PASS();
}

static void
test_encode_rejects_iface_too_long(void)
{
    TEST(encode rejects iface > 15 chars);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    /* 16 chars — one past MQVPN_PATH_LABEL_IFACE_MAX */
    ASSERT_EQ(mqvpn_path_label_encode(buf, sizeof(buf), 1, "abcdefghijklmnop", 0, 0), -1);

    PASS();
}

static void
test_encode_rejects_buf_too_small(void)
{
    TEST(encode rejects buf too small);

    uint8_t buf[8]; /* smaller than the full fixed-field payload needs */
    ASSERT_EQ(mqvpn_path_label_encode(buf, sizeof(buf), 1, "eth0", 0, 0), -1);

    PASS();
}

static void
test_decode_rejects_null(void)
{
    TEST(decode rejects NULL payload);

    uint64_t path_id, dscp_mask;
    uint32_t weight;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    ASSERT_EQ(mqvpn_path_label_decode(NULL, 21, &path_id, iface, sizeof(iface), &weight,
                                      &dscp_mask),
              -1);

    PASS();
}

static void
test_decode_rejects_truncated(void)
{
    TEST(decode rejects truncated payload);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 1, "wlan0", 5, 6);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id, dscp_mask;
    uint32_t weight;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    /* Truncate right after the iface bytes — missing weight+dscp_mask */
    ASSERT_EQ(mqvpn_path_label_decode(buf, 14, &path_id, iface, sizeof(iface), &weight,
                                      &dscp_mask),
              -1);
    /* Too short even for the fixed 8+1 header */
    ASSERT_EQ(mqvpn_path_label_decode(buf, 5, &path_id, iface, sizeof(iface), &weight,
                                      &dscp_mask),
              -1);
    /* Zero length */
    ASSERT_EQ(mqvpn_path_label_decode(buf, 0, &path_id, iface, sizeof(iface), &weight,
                                      &dscp_mask),
              -1);

    PASS();
}

static void
test_decode_rejects_zero_iface_len(void)
{
    TEST(decode rejects zero-length iface field);

    uint8_t buf[9] = {0};
    buf[8] = 0; /* iface_len = 0 */
    uint64_t path_id, dscp_mask;
    uint32_t weight;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    ASSERT_EQ(
        mqvpn_path_label_decode(buf, sizeof(buf), &path_id, iface, sizeof(iface), &weight,
                                &dscp_mask),
        -1);

    PASS();
}

static void
test_decode_rejects_oversized_iface_len_field(void)
{
    TEST(decode rejects iface_len field > 15);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX] = {0};
    buf[8] = 20; /* claims 20 bytes of iface, exceeding the 15-byte max */
    uint64_t path_id, dscp_mask;
    uint32_t weight;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    ASSERT_EQ(
        mqvpn_path_label_decode(buf, sizeof(buf), &path_id, iface, sizeof(iface), &weight,
                                &dscp_mask),
        -1);

    PASS();
}

static void
test_decode_rejects_small_out_buffer(void)
{
    TEST(decode rejects too-small iface_out buffer);

    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 1, "eth0", 0, 0);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id, dscp_mask;
    uint32_t weight;
    char iface_small[4]; /* smaller than MQVPN_PATH_LABEL_IFACE_MAX+1 */
    ASSERT_EQ(mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface_small,
                                      sizeof(iface_small), &weight, &dscp_mask),
              -1);

    PASS();
}

static void
test_decode_ignores_trailing_bytes(void)
{
    TEST(decode ignores trailing bytes after the payload);

    /* Capsule framing already gives us an exact length; decode should not
     * require payload_len to be an exact match — extra trailing bytes
     * (e.g. from a future wire format extension) must not break parsing
     * of the fields that ARE present. */
    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX + 4];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 42, "ppp0", 7, 8);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }
    buf[n] = 0xAA;
    buf[n + 1] = 0xBB;

    uint64_t path_id = 0, dscp_mask = 0;
    uint32_t weight = 0;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n + 2, &path_id, iface, sizeof(iface),
                                     &weight, &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(path_id, 42);
    ASSERT_EQ(weight, 7);
    ASSERT_EQ(dscp_mask, 8);
    if (strcmp(iface, "ppp0") != 0) {
        FAIL("iface mismatch");
        return;
    }

    PASS();
}

static void
test_capsule_type_distinct(void)
{
    TEST(capsule type constant is distinct from IETF-defined types);

    /* DATAGRAM/ADDRESS_ASSIGN/ADDRESS_REQUEST/ROUTE_ADVERTISEMENT = 0x00-0x03 */
    assert(MQVPN_CAPSULE_PATH_LABEL > 0x03);

    PASS();
}

static void
test_capsule_push_type_distinct(void)
{
    TEST(push capsule type constant is distinct from IETF types and from PATH_LABEL);

    /* DATAGRAM/ADDRESS_ASSIGN/ADDRESS_REQUEST/ROUTE_ADVERTISEMENT = 0x00-0x03 */
    assert(MQVPN_CAPSULE_PATH_LABEL_PUSH > 0x03);
    assert(MQVPN_CAPSULE_PATH_LABEL_PUSH != MQVPN_CAPSULE_PATH_LABEL);

    PASS();
}

static void
test_push_direction_roundtrip_via_shared_codec(void)
{
    TEST(push direction (path_id=0) round-trips through the same encode/decode);

    /* The server -> client push direction reuses mqvpn_path_label_encode/
     * decode verbatim with path_id=0 (see mqvpn_path_label.h's "Problem 3"
     * doc) — this pins that contract: path_id survives as exactly 0, and
     * iface/weight/dscp_mask round-trip normally. */
    uint8_t buf[MQVPN_PATH_LABEL_PAYLOAD_MAX];
    int n = mqvpn_path_label_encode(buf, sizeof(buf), 0, "wan1", 50, 1ULL << 46);
    if (n <= 0) {
        FAIL("encode failed");
        return;
    }

    uint64_t path_id = 999, dscp_mask = 0; /* pre-dirty path_id */
    uint32_t weight = 0;
    char iface[MQVPN_PATH_LABEL_IFACE_MAX + 1];
    int rc = mqvpn_path_label_decode(buf, (size_t)n, &path_id, iface, sizeof(iface), &weight,
                                     &dscp_mask);
    ASSERT_EQ(rc, 0);
    ASSERT_EQ(path_id, 0); /* meaningless in this direction; always 0 on the wire */
    ASSERT_EQ(weight, 50);
    ASSERT_EQ(dscp_mask, 1ULL << 46);
    if (strcmp(iface, "wan1") != 0) {
        FAIL("iface mismatch");
        return;
    }

    PASS();
}

int
main(void)
{
    printf("=== mqvpn_path_label unit tests ===\n\n");

    test_roundtrip_basic();
    test_roundtrip_zero_weight_and_mask();
    test_roundtrip_max_len_iface();
    test_roundtrip_path_id_zero();
    test_roundtrip_max_weight_and_mask();
    test_encode_rejects_empty_iface();
    test_encode_rejects_null_iface();
    test_encode_rejects_iface_too_long();
    test_encode_rejects_buf_too_small();
    test_decode_rejects_null();
    test_decode_rejects_truncated();
    test_decode_rejects_zero_iface_len();
    test_decode_rejects_oversized_iface_len_field();
    test_decode_rejects_small_out_buffer();
    test_decode_ignores_trailing_bytes();
    test_capsule_type_distinct();
    test_capsule_push_type_distinct();
    test_push_direction_roundtrip_via_shared_codec();

    printf("\n%d passed, %d failed\n", tests_passed, tests_failed);
    return tests_failed > 0 ? 1 : 0;
}
