/*
 * test_path_rotation.c — Unit tests for mqvpn_rotate_primary_path()
 *
 * Verifies that on reconnect, the primary path index rotates through
 * non-backup paths (regression test for issue #4257: first WAN path
 * with no internet blocks all reconnection attempts).
 *
 * No xquic dependency — links only path_rotation.c.
 */

#include <stdio.h>
#include <stdlib.h>

#include "path_rotation.h"

/* ── Test infrastructure ── */

static int g_run = 0;
static int g_passed = 0;

#define TEST(name)                 \
    static void test_##name(void); \
    static void run_##name(void)   \
    {                              \
        g_run++;                   \
        printf("  %-60s ", #name); \
        test_##name();             \
        g_passed++;                \
        printf("PASS\n");          \
    }                              \
    static void test_##name(void)

#define ASSERT_EQ(a, b)                                                                    \
    do {                                                                                   \
        int _a = (int)(a), _b = (int)(b);                                                  \
        if (_a != _b) {                                                                    \
            printf("FAIL\n    %s:%d  %s == %d, expected %d\n", __FILE__, __LINE__, #a, _a, \
                   _b);                                                                    \
            exit(1);                                                                       \
        }                                                                                  \
    } while (0)

/* ── Helpers ── */

/* Build a flags array: 'B' marks a backup path, any other char is primary */
static void
make_flags(const char *spec, uint32_t *flags, int n)
{
    for (int i = 0; i < n; i++)
        flags[i] = (spec[i] == 'B') ? MQVPN_PATH_FLAG_BACKUP : 0;
}

/* ── Tests ── */

/* Single primary: nothing to rotate to, index stays 0 */
TEST(single_primary_no_rotation)
{
    uint32_t f[1] = {0};
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 1), 0);
}

/* n_paths == 0: no crash, returns cur_idx */
TEST(zero_paths_no_crash)
{
    ASSERT_EQ(mqvpn_rotate_primary_path(0, NULL, 0), 0);
}

/* Two primaries: 0 → 1 → 0 */
TEST(two_primaries_round_robin)
{
    uint32_t f[2];
    make_flags("PP", f, 2);
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 2), 1);
    ASSERT_EQ(mqvpn_rotate_primary_path(1, f, 2), 0);
}

/* Three primaries: 0 → 1 → 2 → 0 */
TEST(three_primaries_round_robin)
{
    uint32_t f[3];
    make_flags("PPP", f, 3);
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 3), 1);
    ASSERT_EQ(mqvpn_rotate_primary_path(1, f, 3), 2);
    ASSERT_EQ(mqvpn_rotate_primary_path(2, f, 3), 0);
}

/* Backup at index 1: [P, B, P] → 0 skips backup, goes to 2; 2 wraps to 0 */
TEST(backup_in_middle_is_skipped)
{
    uint32_t f[3];
    make_flags("PBP", f, 3);
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 3), 2);
    ASSERT_EQ(mqvpn_rotate_primary_path(2, f, 3), 0);
}

/* Backup first: [B, P, P] → starting at 0, next non-backup wrapping is 1 */
TEST(backup_first_is_skipped)
{
    uint32_t f[3];
    make_flags("BPP", f, 3);
    /* cur=1 → next=2, cur=2 → wraps, skips B at 0, lands on 1 */
    ASSERT_EQ(mqvpn_rotate_primary_path(1, f, 3), 2);
    ASSERT_EQ(mqvpn_rotate_primary_path(2, f, 3), 1);
}

/* Backup last: [P, P, B] → 0→1, 1 wraps, skips B at 2, back to 0 */
TEST(backup_last_is_skipped)
{
    uint32_t f[3];
    make_flags("PPB", f, 3);
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 3), 1);
    ASSERT_EQ(mqvpn_rotate_primary_path(1, f, 3), 0);
}

/* Only one non-backup among backups: no rotation */
TEST(single_primary_among_backups_no_rotation)
{
    uint32_t f[4];
    make_flags("BPBB", f, 4);
    ASSERT_EQ(mqvpn_rotate_primary_path(1, f, 4), 1);
}

/* All backup: cur_idx is unchanged (degenerate config) */
TEST(all_backup_no_rotation)
{
    uint32_t f[3];
    make_flags("BBB", f, 3);
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 3), 0);
    ASSERT_EQ(mqvpn_rotate_primary_path(2, f, 3), 2);
}

/* 4-path config matching issue #4257: [dead-primary, working-primary, B, B]
 * First reconnect should move from 0 to 1. */
TEST(issue_4257_first_dead_rotates_to_second)
{
    uint32_t f[4];
    make_flags("PPBB", f, 4);
    /* Attempt 1 fails on path 0 → rotate to 1 */
    ASSERT_EQ(mqvpn_rotate_primary_path(0, f, 4), 1);
    /* Attempt 2 succeeds on path 1 → no further change expected, but if it
     * were to fail too it would wrap back to 0 */
    ASSERT_EQ(mqvpn_rotate_primary_path(1, f, 4), 0);
}

/* Full rotation cycle with 4 primaries visits every index exactly once */
TEST(four_primaries_full_cycle)
{
    uint32_t f[4];
    make_flags("PPPP", f, 4);
    int idx = 0;
    for (int step = 0; step < 4; step++) {
        int next = mqvpn_rotate_primary_path(idx, f, 4);
        ASSERT_EQ(next, (idx + 1) % 4);
        idx = next;
    }
}

/* ── Runner ── */

int
main(void)
{
    printf("test_path_rotation\n");
    run_single_primary_no_rotation();
    run_zero_paths_no_crash();
    run_two_primaries_round_robin();
    run_three_primaries_round_robin();
    run_backup_in_middle_is_skipped();
    run_backup_first_is_skipped();
    run_backup_last_is_skipped();
    run_single_primary_among_backups_no_rotation();
    run_all_backup_no_rotation();
    run_issue_4257_first_dead_rotates_to_second();
    run_four_primaries_full_cycle();

    printf("\n%d/%d tests passed\n", g_passed, g_run);
    return (g_passed == g_run) ? 0 : 1;
}
