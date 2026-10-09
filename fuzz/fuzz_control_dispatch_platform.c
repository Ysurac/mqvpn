// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/* Link-only definitions for fuzz_control_dispatch.
 *
 * control_socket.c's client-mode commands (add_path, remove_path,
 * list_paths, set_path_weight, set_path_dscp_mask) call these
 * platform_linux.c functions, which are not part of mqvpn_lib, so the fuzz
 * target did not link. The harness drives dispatch() with cli_ctx == NULL
 * (server mode), where those commands return before reaching the platform
 * layer: these are never called, they only have to resolve. They abort, so
 * a change that makes one reachable shows up as a fuzzer crash instead of a
 * silent "failed" reply. */

#include <stdlib.h>

#include "platform_internal.h"

int
platform_add_path(platform_ctx_t *p, const char *iface, int backup)
{
    (void)p;
    (void)iface;
    (void)backup;
    abort();
}

int
platform_remove_path(platform_ctx_t *p, const char *iface)
{
    (void)p;
    (void)iface;
    abort();
}

int
platform_list_paths(platform_ctx_t *p, char names[][IFNAMSIZ], int max)
{
    (void)p;
    (void)names;
    (void)max;
    abort();
}

int
platform_set_path_weight(platform_ctx_t *p, const char *iface, uint32_t weight)
{
    (void)p;
    (void)iface;
    (void)weight;
    abort();
}

int
platform_set_path_dscp_mask(platform_ctx_t *p, const char *iface, uint64_t dscp_mask)
{
    (void)p;
    (void)iface;
    (void)dscp_mask;
    abort();
}
