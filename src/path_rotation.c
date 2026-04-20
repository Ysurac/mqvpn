/*
 * path_rotation.c — Primary path rotation logic (no xquic dependency)
 */

#include "path_rotation.h"

int
mqvpn_rotate_primary_path(int cur_idx, const uint32_t *path_flags, int n_paths)
{
    if (n_paths <= 1 || !path_flags) return cur_idx;

    /* Count non-backup (primary) paths */
    int n_primary = 0;
    for (int i = 0; i < n_paths; i++)
        if (!(path_flags[i] & MQVPN_PATH_FLAG_BACKUP)) n_primary++;

    if (n_primary <= 1) return cur_idx; /* nothing to rotate to */

    /* Walk forward from cur_idx+1, wrapping, until we find a non-backup path */
    int next = (cur_idx + 1) % n_paths;
    while (path_flags[next] & MQVPN_PATH_FLAG_BACKUP)
        next = (next + 1) % n_paths;

    return next;
}
