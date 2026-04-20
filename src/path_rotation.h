/*
 * path_rotation.h — Primary path rotation logic (no xquic dependency)
 */

#ifndef PATH_ROTATION_H
#define PATH_ROTATION_H

#include <stdint.h>

#include "libmqvpn.h" /* MQVPN_PATH_FLAG_BACKUP */

/*
 * mqvpn_rotate_primary_path - pick the next primary path index.
 *
 * Iterates forward (wrapping) from cur_idx+1, skipping any path whose
 * flags have MQVPN_PATH_FLAG_BACKUP set.  Returns the new index, or
 * cur_idx unchanged when there is ≤1 non-backup path (nothing to rotate to).
 *
 * @cur_idx    current primary path index (0-based)
 * @path_flags array of per-path flag words, length n_paths
 * @n_paths    total number of paths (primary + backup)
 */
int mqvpn_rotate_primary_path(int cur_idx, const uint32_t *path_flags, int n_paths);

#endif /* PATH_ROTATION_H */
