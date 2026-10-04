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

    if (n_primary == 0) return cur_idx; /* nothing to rotate to */
    /* A single primary is used as is when it is cur_idx. When cur_idx is a
     * backup (or a detached path the caller marked as one), the walk below
     * moves to that single primary: with two WANs and the first one removed,
     * returning cur_idx would retry the removed path forever. */
    if (n_primary == 1 && !(path_flags[cur_idx] & MQVPN_PATH_FLAG_BACKUP)) return cur_idx;

    /* Walk forward from cur_idx+1, wrapping, until we find a non-backup path */
    int next = (cur_idx + 1) % n_paths;
    while (path_flags[next] & MQVPN_PATH_FLAG_BACKUP)
        next = (next + 1) % n_paths;

    return next;
}

int
mqvpn_reconnect_path_idx(int cur_idx, const uint32_t *path_flags, const int *attached,
                         int n_paths)
{
    if (n_paths <= 0 || n_paths > MQVPN_MAX_PATHS || !path_flags || !attached)
        return cur_idx;

    /* Detached paths are treated as backup so the rotation skips them. */
    uint32_t view[MQVPN_MAX_PATHS];
    for (int i = 0; i < n_paths; i++)
        view[i] = path_flags[i] | (attached[i] ? 0 : MQVPN_PATH_FLAG_BACKUP);

    int idx = mqvpn_rotate_primary_path(cur_idx, view, n_paths);
    if (idx >= 0 && idx < n_paths && attached[idx] &&
        !(view[idx] & MQVPN_PATH_FLAG_BACKUP))
        return idx;

    /* No attached primary: fall back to the first attached backup path. */
    for (int i = 0; i < n_paths; i++)
        if (attached[i]) return i;
    return cur_idx;
}
