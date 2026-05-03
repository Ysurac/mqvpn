/*
 * path_error_policy.c — mqvpn policy helpers for transport path errors
 */

#include "path_error_policy.h"

#include <xquic/xqc_errno.h>

bool
mqvpn_path_error_is_create_budget_exhausted(int xqc_ret)
{
    return xqc_ret == -XQC_EMP_CREATE_PATH;
}
