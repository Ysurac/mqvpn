/*
 * path_error_policy.h — mqvpn policy helpers for transport path errors
 */

#ifndef MQVPN_PATH_ERROR_POLICY_H
#define MQVPN_PATH_ERROR_POLICY_H

#include <stdbool.h>

/*
 * Returns true when an xqc_conn_create_path() error indicates the transport
 * path budget is exhausted for the current connection and recovery requires
 * reconnecting the connection.
 */
bool mqvpn_path_error_is_create_budget_exhausted(int xqc_ret);

#endif /* MQVPN_PATH_ERROR_POLICY_H */
