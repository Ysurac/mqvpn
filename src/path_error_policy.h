/*
 * path_error_policy.h — mqvpn policy helpers for transport path errors
 */

#ifndef MQVPN_PATH_ERROR_POLICY_H
#define MQVPN_PATH_ERROR_POLICY_H

#include <stdbool.h>

/*
 * Returns true when an xqc_conn_create_path() error means xquic could not
 * create the path object at all (-XQC_EMP_CREATE_PATH). It is not a path ID
 * shortage: under draft-21 path IDs are dynamic and running out of them is
 * the transient -XQC_EMP_NO_AVAIL_PATH_ID. The client treats this one as a
 * permanent activation failure for the slot.
 */
bool mqvpn_path_error_is_create_budget_exhausted(int xqc_ret);

#endif /* MQVPN_PATH_ERROR_POLICY_H */
