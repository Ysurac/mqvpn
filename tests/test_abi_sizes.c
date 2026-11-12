// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * The getters below write sizeof(*out) without reading the caller's
 * struct_size, and mqvpn_client_get_paths() fills an array. Growing one of
 * these structs therefore breaks binaries built against the old header,
 * unless MQVPN_SOVERSION in CMakeLists.txt is bumped with it.
 *
 * Sizes are recorded per soname. Never edit a released row: bump
 * MQVPN_SOVERSION and add a row. 64-bit only; 32-bit targets lay the
 * structs out differently.
 */
#include "libmqvpn.h"

#if MQVPN_SOVERSION == 4
/* dev's .so.4 layout, not upstream's: mqvpn_path_info_t carries dev's
 * `flags` (56 -> 64 upstream), and mqvpn_client_info_t embeds
 * paths[MQVPN_MAX_PATHS], which is 16 on Linux x86_64 (168 + 16 * 96). */
enum {
    SZ_STATS = 144,
    SZ_PATH_INFO = 64,
    SZ_PATH_STATS = 96,
#  if defined(__linux__) && defined(__x86_64__) && !defined(__ANDROID__)
    SZ_CLIENT_INFO = 1704,
#  else
    SZ_CLIENT_INFO = 936,
#  endif
    SZ_INTEREST = 16,
};
#else
#  error "MQVPN_SOVERSION changed: add a size row for it"
#endif

#define PIN_SIZE(type, size)                                      \
    _Static_assert(sizeof(void *) != 8 || sizeof(type) == (size), \
                   #type " changed size: bump MQVPN_SOVERSION and add a row")

PIN_SIZE(mqvpn_stats_t, SZ_STATS);             /* client/server_get_stats */
PIN_SIZE(mqvpn_path_info_t, SZ_PATH_INFO);     /* client_get_paths (array) */
PIN_SIZE(mqvpn_path_stats_t, SZ_PATH_STATS);   /* client_info_t.paths[] */
PIN_SIZE(mqvpn_client_info_t, SZ_CLIENT_INFO); /* server_get_client_info */
PIN_SIZE(mqvpn_interest_t, SZ_INTEREST);       /* client/server_get_interest */

int
main(void)
{
    return 0;
}
