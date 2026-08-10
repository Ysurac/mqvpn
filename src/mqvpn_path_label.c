// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * mqvpn_path_label.c — see mqvpn_path_label.h for the why.
 */
#include "mqvpn_path_label.h"

#include <string.h>

int
mqvpn_path_label_encode(uint8_t *buf, size_t buf_len, uint64_t path_id, const char *iface,
                        uint32_t weight, uint64_t dscp_mask)
{
    if (!buf || !iface) return -1;

    size_t iface_len = strlen(iface);
    if (iface_len == 0 || iface_len > MQVPN_PATH_LABEL_IFACE_MAX) return -1;

    size_t total = 8 + 1 + iface_len + 4 + 8;
    if (buf_len < total) return -1;

    size_t off = 0;
    for (int i = 0; i < 8; i++) {
        buf[off++] = (uint8_t)(path_id >> (8 * (7 - i)));
    }
    buf[off++] = (uint8_t)iface_len;
    memcpy(buf + off, iface, iface_len);
    off += iface_len;
    for (int i = 0; i < 4; i++) {
        buf[off++] = (uint8_t)(weight >> (8 * (3 - i)));
    }
    for (int i = 0; i < 8; i++) {
        buf[off++] = (uint8_t)(dscp_mask >> (8 * (7 - i)));
    }

    return (int)total;
}

int
mqvpn_path_label_decode(const uint8_t *payload, size_t payload_len, uint64_t *path_id_out,
                        char *iface_out, size_t iface_out_cap, uint32_t *weight_out,
                        uint64_t *dscp_mask_out)
{
    if (!payload || !path_id_out || !iface_out || !weight_out || !dscp_mask_out) return -1;
    if (iface_out_cap < MQVPN_PATH_LABEL_IFACE_MAX + 1) return -1;
    if (payload_len < 9) return -1;

    size_t off = 0;
    uint64_t path_id = 0;
    for (int i = 0; i < 8; i++) {
        path_id = (path_id << 8) | payload[off++];
    }

    uint8_t iface_len = payload[off++];
    if (iface_len == 0 || iface_len > MQVPN_PATH_LABEL_IFACE_MAX) return -1;
    if (payload_len < off + (size_t)iface_len + 4 + 8) return -1;

    memcpy(iface_out, payload + off, iface_len);
    iface_out[iface_len] = '\0';
    off += iface_len;

    uint32_t weight = 0;
    for (int i = 0; i < 4; i++) {
        weight = (weight << 8) | payload[off++];
    }

    uint64_t dscp_mask = 0;
    for (int i = 0; i < 8; i++) {
        dscp_mask = (dscp_mask << 8) | payload[off++];
    }

    *path_id_out = path_id;
    *weight_out = weight;
    *dscp_mask_out = dscp_mask;

    return 0;
}
