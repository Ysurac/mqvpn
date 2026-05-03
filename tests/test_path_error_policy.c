/*
 * test_path_error_policy.c — unit tests for path error policy helpers
 */

#include <stdio.h>
#include <stdlib.h>

#include <xquic/xqc_errno.h>

#include "path_error_policy.h"

static int g_run = 0;
static int g_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void)   \
    {                              \
        g_run++;                   \
        printf("  %-52s ", #name); \
        test_##name();             \
        g_passed++;                \
        printf("PASS\n");          \
    }                              \
    static void test_##name(void)

#define ASSERT_TRUE(v)                                                                     \
    do {                                                                                   \
        if (!(v)) {                                                                        \
            printf("FAIL\n    %s:%d  expected true: %s\n", __FILE__, __LINE__, #v);      \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

#define ASSERT_FALSE(v)                                                                    \
    do {                                                                                   \
        if ((v)) {                                                                         \
            printf("FAIL\n    %s:%d  expected false: %s\n", __FILE__, __LINE__, #v);     \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

TEST(create_path_error_triggers_budget_exhausted_policy)
{
    ASSERT_TRUE(mqvpn_path_error_is_create_budget_exhausted(-XQC_EMP_CREATE_PATH));
}

TEST(other_multipath_errors_do_not_trigger_budget_exhausted_policy)
{
    ASSERT_FALSE(mqvpn_path_error_is_create_budget_exhausted(-XQC_EMP_NO_AVAIL_PATH_ID));
    ASSERT_FALSE(mqvpn_path_error_is_create_budget_exhausted(-XQC_EMP_PATH_NOT_FOUND));
}

TEST(non_negative_or_unrelated_codes_do_not_trigger)
{
    ASSERT_FALSE(mqvpn_path_error_is_create_budget_exhausted(XQC_EMP_CREATE_PATH));
    ASSERT_FALSE(mqvpn_path_error_is_create_budget_exhausted(0));
    ASSERT_FALSE(mqvpn_path_error_is_create_budget_exhausted(-1));
}

int
main(void)
{
    printf("\n[test_path_error_policy] Running tests...\n");

    run_create_path_error_triggers_budget_exhausted_policy();
    run_other_multipath_errors_do_not_trigger_budget_exhausted_policy();
    run_non_negative_or_unrelated_codes_do_not_trigger();

    printf("[test_path_error_policy] Passed %d/%d tests\n", g_passed, g_run);
    return (g_passed == g_run) ? 0 : 1;
}
