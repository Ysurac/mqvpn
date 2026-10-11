// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * firewall.c — WFP-based kill switch for Windows
 *
 * Uses the Windows Filtering Platform (WFP) to block all outbound traffic
 * except:
 *   - Loopback
 *   - Traffic to the VPN server (UDP from this executable)
 *   - DHCP (v4 68->67, v6 546->547) and ICMPv6 router/neighbor discovery,
 *     so the physical links keep their leases and neighbors
 *   - Traffic on the TUN (Wintun) interface
 *
 * Lifecycle: the base set (everything but the TUN permits) is installed once,
 * before the first connect, and stays through every RECONNECTING; only the
 * TUN permits follow the adapter (win_killswitch_permit_tun() when the TUN
 * comes up, win_killswitch_revoke_tun() when it goes). The whole session is
 * closed only on final shutdown. Tearing the block down on RECONNECTING (the
 * old behavior) leaked all traffic for the whole reconnect window.
 *
 * Block filters carry FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT (a hard block, as
 * wireguard-windows does): without it a permit in a lower-priority sublayer
 * can override them.
 *
 * All objects are added through a DYNAMIC WFP session. BFE deletes everything
 * a dynamic session added once that session ends, including when the process
 * dies without running cleanup. That matters because WFP objects are static by
 * default and "live until they are deleted, BFE stops, or the system is
 * shutdown" (WFP Object Management) — a crash with static filters leaves the
 * block-all rules in place and cuts the host off until the next reboot.
 */

#ifdef _WIN32

#  include "platform_internal_win.h"
#  include "log.h"

#  include <stdio.h>
#  include <stdlib.h>
#  include <string.h>

/* ── Filter helpers ── */

/* Initialize common FWPM_FILTER0 fields. Block filters are hard blocks
 * (FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT): see the file header. Permits keep the
 * action right, so a lower-priority sublayer (Windows Firewall, say) can
 * still block what this kill switch lets through. */
static void
wfp_filter_base(FWPM_FILTER0 *f, const GUID *layer, const GUID *sublayer,
                const wchar_t *name, UINT8 weight, UINT32 action)
{
    memset(f, 0, sizeof(*f));
    f->layerKey = *layer;
    f->subLayerKey = *sublayer;
    f->displayData.name = (wchar_t *)name;
    f->weight.type = FWP_UINT8;
    f->weight.uint8 = weight;
    f->action.type = action;
    if (action == FWP_ACTION_BLOCK) f->flags |= FWPM_FILTER_FLAG_CLEAR_ACTION_RIGHT;
}

/* Add a single WFP filter; *id_out (if non-NULL) receives its runtime id */
static int
add_filter(platform_win_ctx_t *p, const FWPM_FILTER0 *filter, UINT64 *id_out)
{
    UINT64 id = 0;
    DWORD err = FwpmFilterAdd0(p->wfp_engine, filter, NULL, &id);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmFilterAdd0: error %lu", err);
        return -1;
    }

    if (id_out) *id_out = id;
    p->n_wfp_filters++;
    return 0;
}

static void
cond_uint8(FWPM_FILTER_CONDITION0 *c, const GUID *field, UINT8 v)
{
    memset(c, 0, sizeof(*c));
    c->fieldKey = *field;
    c->matchType = FWP_MATCH_EQUAL;
    c->conditionValue.type = FWP_UINT8;
    c->conditionValue.uint8 = v;
}

static void
cond_uint16(FWPM_FILTER_CONDITION0 *c, const GUID *field, UINT16 v)
{
    memset(c, 0, sizeof(*c));
    c->fieldKey = *field;
    c->matchType = FWP_MATCH_EQUAL;
    c->conditionValue.type = FWP_UINT16;
    c->conditionValue.uint16 = v;
}

/* PERMIT loopback (IPv4 + IPv6) */
static int
wfp_add_loopback_permit(platform_win_ctx_t *p)
{
    static const GUID *layers[] = {
        &FWPM_LAYER_ALE_AUTH_CONNECT_V4,
        &FWPM_LAYER_ALE_AUTH_CONNECT_V6,
    };
    static const wchar_t *names[] = {
        L"mqvpn: permit loopback v4",
        L"mqvpn: permit loopback v6",
    };

    for (int i = 0; i < 2; i++) {
        FWPM_FILTER0 f;
        wfp_filter_base(&f, layers[i], &p->wfp_sublayer_key, names[i], 15,
                        FWP_ACTION_PERMIT);

        FWPM_FILTER_CONDITION0 cond;
        memset(&cond, 0, sizeof(cond));
        cond.fieldKey = FWPM_CONDITION_FLAGS;
        cond.matchType = FWP_MATCH_FLAGS_ALL_SET;
        cond.conditionValue.type = FWP_UINT32;
        cond.conditionValue.uint32 = FWP_CONDITION_FLAG_IS_LOOPBACK;

        f.filterCondition = &cond;
        f.numFilterConditions = 1;
        if (add_filter(p, &f, NULL) < 0) return -1;
    }
    return 0;
}

/* PERMIT traffic on the TUN interface (IPv4 + IPv6); ids recorded so the
 * permits can follow the adapter (win_killswitch_revoke_tun). */
static int
wfp_add_iface_permit(platform_win_ctx_t *p)
{
    static const GUID *layers[] = {
        &FWPM_LAYER_ALE_AUTH_CONNECT_V4,
        &FWPM_LAYER_ALE_AUTH_CONNECT_V6,
    };
    static const wchar_t *names[] = {
        L"mqvpn: permit TUN v4",
        L"mqvpn: permit TUN v6",
    };

    /* FWP_UINT64 conditions point at their value: it must outlive the add. */
    UINT64 luid = p->tun.luid.Value;
    for (int i = 0; i < 2; i++) {
        FWPM_FILTER0 f;
        wfp_filter_base(&f, layers[i], &p->wfp_sublayer_key, names[i], 14,
                        FWP_ACTION_PERMIT);

        FWPM_FILTER_CONDITION0 cond;
        memset(&cond, 0, sizeof(cond));
        cond.fieldKey = FWPM_CONDITION_IP_LOCAL_INTERFACE;
        cond.matchType = FWP_MATCH_EQUAL;
        cond.conditionValue.type = FWP_UINT64;
        cond.conditionValue.uint64 = &luid;

        f.filterCondition = &cond;
        f.numFilterConditions = 1;
        if (add_filter(p, &f, &p->wfp_tun_filter_ids[p->n_wfp_tun_filters]) < 0)
            return -1;
        p->n_wfp_tun_filters++;
    }
    return 0;
}

/* AppId of this executable (FWPM_CONDITION_ALE_APP_ID), so only mqvpn itself
 * can reach the server port through the block. NULL on failure (the caller
 * then permits by address/port/protocol only). Free with FwpmFreeMemory0. */
static FWP_BYTE_BLOB *
wfp_own_app_id(void)
{
    const DWORD cap = 32768; /* longest extended-length path, in WCHARs */
    WCHAR *path = (WCHAR *)malloc(cap * sizeof(WCHAR));
    if (!path) return NULL;
    DWORD n = GetModuleFileNameW(NULL, path, cap);
    FWP_BYTE_BLOB *blob = NULL;
    if (n == 0 || n >= cap) {
        LOG_WRN("kill switch: GetModuleFileNameW failed (%lu); server permit not "
                "restricted to this executable",
                GetLastError());
    } else {
        DWORD err = FwpmGetAppIdFromFileName0(path, &blob);
        if (err != ERROR_SUCCESS) {
            LOG_WRN("kill switch: FwpmGetAppIdFromFileName0: error %lu; server permit "
                    "not restricted to this executable",
                    err);
            blob = NULL;
        }
    }
    free(path);
    return blob;
}

/* PERMIT UDP to the VPN server (IPv4 or IPv6, based on server_addr family),
 * from this executable when its AppId could be resolved. */
static int
wfp_add_server_permit(platform_win_ctx_t *p)
{
    const GUID *layer;
    const wchar_t *name;
    FWPM_FILTER_CONDITION0 conds[4];
    memset(conds, 0, sizeof(conds));

    conds[0].fieldKey = FWPM_CONDITION_IP_REMOTE_ADDRESS;
    conds[0].matchType = FWP_MATCH_EQUAL;
    if (p->server_addr.ss_family == AF_INET) {
        layer = &FWPM_LAYER_ALE_AUTH_CONNECT_V4;
        name = L"mqvpn: permit server UDP v4";
        conds[0].conditionValue.type = FWP_UINT32;
        conds[0].conditionValue.uint32 =
            ntohl(((struct sockaddr_in *)&p->server_addr)->sin_addr.s_addr);
    } else if (p->server_addr.ss_family == AF_INET6) {
        layer = &FWPM_LAYER_ALE_AUTH_CONNECT_V6;
        name = L"mqvpn: permit server UDP v6";
        conds[0].conditionValue.type = FWP_BYTE_ARRAY16_TYPE;
        conds[0].conditionValue.byteArray16 =
            (FWP_BYTE_ARRAY16 *)&((struct sockaddr_in6 *)&p->server_addr)->sin6_addr;
    } else {
        return 0; /* unknown family — skip */
    }
    cond_uint16(&conds[1], &FWPM_CONDITION_IP_REMOTE_PORT, (UINT16)p->server_port);
    cond_uint8(&conds[2], &FWPM_CONDITION_IP_PROTOCOL, IPPROTO_UDP);

    UINT32 n = 3;
    FWP_BYTE_BLOB *app_id = wfp_own_app_id();
    if (app_id) {
        conds[3].fieldKey = FWPM_CONDITION_ALE_APP_ID;
        conds[3].matchType = FWP_MATCH_EQUAL;
        conds[3].conditionValue.type = FWP_BYTE_BLOB_TYPE;
        conds[3].conditionValue.byteBlob = app_id;
        n = 4;
    }

    FWPM_FILTER0 f;
    wfp_filter_base(&f, layer, &p->wfp_sublayer_key, name, 13, FWP_ACTION_PERMIT);
    f.filterCondition = conds;
    f.numFilterConditions = n;
    int rc = add_filter(p, &f, NULL);
    if (app_id) FwpmFreeMemory0((void **)&app_id);
    return rc;
}

/* PERMIT DHCP and ICMPv6 router/neighbor discovery, so a long reconnect (the
 * block now stays up through it) does not lose the physical links' leases and
 * neighbors. Outbound only, like every rule here: the kill switch never
 * filters inbound. */
static int
wfp_add_link_maintenance_permit(platform_win_ctx_t *p)
{
    /* DHCPv4: client 68 -> server 67 (broadcast DISCOVER/REQUEST, unicast
     * renew). */
    {
        FWPM_FILTER_CONDITION0 c[3];
        cond_uint8(&c[0], &FWPM_CONDITION_IP_PROTOCOL, IPPROTO_UDP);
        cond_uint16(&c[1], &FWPM_CONDITION_IP_LOCAL_PORT, 68);
        cond_uint16(&c[2], &FWPM_CONDITION_IP_REMOTE_PORT, 67);
        FWPM_FILTER0 f;
        wfp_filter_base(&f, &FWPM_LAYER_ALE_AUTH_CONNECT_V4, &p->wfp_sublayer_key,
                        L"mqvpn: permit DHCP v4", 12, FWP_ACTION_PERMIT);
        f.filterCondition = c;
        f.numFilterConditions = 3;
        if (add_filter(p, &f, NULL) < 0) return -1;
    }
    /* DHCPv6: client 546 -> server/relay 547. */
    {
        FWPM_FILTER_CONDITION0 c[3];
        cond_uint8(&c[0], &FWPM_CONDITION_IP_PROTOCOL, IPPROTO_UDP);
        cond_uint16(&c[1], &FWPM_CONDITION_IP_LOCAL_PORT, 546);
        cond_uint16(&c[2], &FWPM_CONDITION_IP_REMOTE_PORT, 547);
        FWPM_FILTER0 f;
        wfp_filter_base(&f, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, &p->wfp_sublayer_key,
                        L"mqvpn: permit DHCP v6", 12, FWP_ACTION_PERMIT);
        f.filterCondition = c;
        f.numFilterConditions = 3;
        if (add_filter(p, &f, NULL) < 0) return -1;
    }
    /* ICMPv6 RS/RA/NS/NA (133-136). WFP ORs conditions that share a field
     * key and ANDs the groups, so this is proto == 58 AND type in 133..136.
     * At the ALE layers the ICMP type is carried in the local-port field
     * (FWPM_CONDITION_ICMP_TYPE). */
    {
        FWPM_FILTER_CONDITION0 c[5];
        cond_uint8(&c[0], &FWPM_CONDITION_IP_PROTOCOL, IPPROTO_ICMPV6);
        for (int i = 0; i < 4; i++)
            cond_uint16(&c[1 + i], &FWPM_CONDITION_ICMP_TYPE, (UINT16)(133 + i));
        FWPM_FILTER0 f;
        wfp_filter_base(&f, &FWPM_LAYER_ALE_AUTH_CONNECT_V6, &p->wfp_sublayer_key,
                        L"mqvpn: permit ICMPv6 ND", 12, FWP_ACTION_PERMIT);
        f.filterCondition = c;
        f.numFilterConditions = 5;
        if (add_filter(p, &f, NULL) < 0) return -1;
    }
    return 0;
}

/* BLOCK all other outbound (IPv4 + IPv6) */
static int
wfp_add_block_all(platform_win_ctx_t *p)
{
    static const GUID *layers[] = {
        &FWPM_LAYER_ALE_AUTH_CONNECT_V4,
        &FWPM_LAYER_ALE_AUTH_CONNECT_V6,
    };
    static const wchar_t *names[] = {
        L"mqvpn: block all v4",
        L"mqvpn: block all v6",
    };

    for (int i = 0; i < 2; i++) {
        FWPM_FILTER0 f;
        wfp_filter_base(&f, layers[i], &p->wfp_sublayer_key, names[i], 1,
                        FWP_ACTION_BLOCK);
        f.numFilterConditions = 0;
        if (add_filter(p, &f, NULL) < 0) return -1;
    }
    return 0;
}

/* ── Public API ── */

int
win_setup_killswitch(platform_win_ctx_t *p)
{
    if (!p->killswitch_enabled) return 0;

    /* Checked before killswitch_active, which a failed close deliberately
     * leaves set: the early return below would otherwise report success for a
     * kill switch that is really the stale, unreachable session. */
    if (p->wfp_close_failed) {
        LOG_ERR("kill switch: the previous WFP session could not be closed; "
                "refusing to open a second one on top of it");
        return -1;
    }

    if (p->killswitch_active) return 0;

    DWORD err;

    /* Open WFP engine on a dynamic session — see the file header for why. */
    FWPM_SESSION0 session;
    memset(&session, 0, sizeof(session));
    session.displayData.name = L"mqvpn kill switch";
    session.flags = FWPM_SESSION_FLAG_DYNAMIC;

    err = FwpmEngineOpen0(NULL, RPC_C_AUTHN_DEFAULT, NULL, &session, &p->wfp_engine);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmEngineOpen0: error %lu", err);
        return -1;
    }

    /* Begin transaction */
    err = FwpmTransactionBegin0(p->wfp_engine, 0);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmTransactionBegin0: error %lu", err);
        FwpmEngineClose0(p->wfp_engine);
        p->wfp_engine = NULL;
        return -1;
    }

    /* Create sublayer */
    CoCreateGuid(&p->wfp_sublayer_key);

    FWPM_SUBLAYER0 sublayer;
    memset(&sublayer, 0, sizeof(sublayer));
    sublayer.subLayerKey = p->wfp_sublayer_key;
    sublayer.displayData.name = L"mqvpn kill switch";
    sublayer.weight = 0xFFFF; /* highest priority */

    err = FwpmSubLayerAdd0(p->wfp_engine, &sublayer, NULL);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmSubLayerAdd0: error %lu", err);
        FwpmTransactionAbort0(p->wfp_engine);
        FwpmEngineClose0(p->wfp_engine);
        p->wfp_engine = NULL;
        return -1;
    }

    p->n_wfp_filters = 0;
    p->n_wfp_tun_filters = 0;

    /* Base set: permit loopback, server, link maintenance → block all. The
     * TUN permits come and go with the adapter (win_killswitch_permit_tun). */
    if (wfp_add_loopback_permit(p) < 0 || wfp_add_server_permit(p) < 0 ||
        wfp_add_link_maintenance_permit(p) < 0 || wfp_add_block_all(p) < 0) {
        FwpmTransactionAbort0(p->wfp_engine);
        FwpmEngineClose0(p->wfp_engine);
        p->wfp_engine = NULL;
        p->n_wfp_filters = 0;
        return -1;
    }

    /* Commit transaction */
    err = FwpmTransactionCommit0(p->wfp_engine);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmTransactionCommit0: error %lu", err);
        FwpmTransactionAbort0(p->wfp_engine);
        FwpmEngineClose0(p->wfp_engine);
        p->wfp_engine = NULL;
        p->n_wfp_filters = 0;
        return -1;
    }

    p->killswitch_active = 1;
    LOG_INF("kill switch active (%d WFP filters)", p->n_wfp_filters);
    return 0;
}

int
win_killswitch_permit_tun(platform_win_ctx_t *p)
{
    if (!p->killswitch_active || !p->wfp_engine || p->wfp_close_failed) {
        /* Enabled but not active: the base set failed, or was never set up. */
        if (p->killswitch_enabled) {
            LOG_ERR("kill switch: base filters are not installed");
            return -1;
        }
        return 0;
    }
    if (p->n_wfp_tun_filters > 0) win_killswitch_revoke_tun(p); /* stale adapter */

    DWORD err = FwpmTransactionBegin0(p->wfp_engine, 0);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmTransactionBegin0: error %lu", err);
        return -1;
    }
    int before = p->n_wfp_filters;
    if (wfp_add_iface_permit(p) < 0) {
        FwpmTransactionAbort0(p->wfp_engine);
        p->n_wfp_filters = before;
        p->n_wfp_tun_filters = 0;
        return -1;
    }
    err = FwpmTransactionCommit0(p->wfp_engine);
    if (err != ERROR_SUCCESS) {
        LOG_ERR("FwpmTransactionCommit0: error %lu", err);
        FwpmTransactionAbort0(p->wfp_engine);
        p->n_wfp_filters = before;
        p->n_wfp_tun_filters = 0;
        return -1;
    }
    LOG_INF("kill switch: TUN %s permitted (%d WFP filters)", p->tun.name,
            p->n_wfp_filters);
    return 0;
}

void
win_killswitch_revoke_tun(platform_win_ctx_t *p)
{
    if (p->n_wfp_tun_filters == 0) return;
    if (p->wfp_engine && !p->wfp_close_failed) {
        for (int i = 0; i < p->n_wfp_tun_filters; i++) {
            DWORD err = FwpmFilterDeleteById0(p->wfp_engine, p->wfp_tun_filter_ids[i]);
            if (err != ERROR_SUCCESS && err != (DWORD)FWP_E_FILTER_NOT_FOUND)
                /* A leftover permit names the old adapter's LUID: it matches
                 * nothing once that adapter is gone, and the session close
                 * on shutdown removes it with everything else. */
                LOG_WRN("kill switch: FwpmFilterDeleteById0: error %lu", err);
            else
                p->n_wfp_filters--;
        }
    }
    p->n_wfp_tun_filters = 0;
}

void
win_cleanup_killswitch(platform_win_ctx_t *p)
{
    if (!p->killswitch_active || !p->wfp_engine) return;

    /* Never close a handle whose close already failed: a WFP engine handle is
     * an RPC context handle, and its state after a failed close is undefined. */
    if (p->wfp_close_failed) return;

    /* Closing the engine ends the dynamic session, which deletes the sublayer
     * and every filter in it.
     *
     * WFP has no cascade delete: "An object cannot be deleted until all
     * objects that reference it have first been deleted" (WFP Object
     * Management), and every filter references the sublayer it was added to.
     * Deleting the sublayer on its own therefore leaves the filters — and the
     * block-all rules among them — in place. */
    DWORD err = FwpmEngineClose0(p->wfp_engine);
    if (err != ERROR_SUCCESS) {
        /* The session may still be blocking everything, and nothing can reach
         * it any more: the handle is the only reference to it, and the next
         * setup overwrites wfp_sublayer_key with a fresh GUID. Keep
         * wfp_engine and killswitch_active set so no second session is stacked
         * on the stale one, and let the caller end the process — the one
         * remedy that always works, because BFE runs down a dynamic session
         * when its owner dies. That is why the session is dynamic at all. */
        p->wfp_close_failed = 1;
        LOG_ERR("FwpmEngineClose0: error %lu; kill switch filters are still "
                "live and can no longer be addressed — shutting down so BFE "
                "removes them",
                err);
        return;
    }

    p->wfp_engine = NULL;
    p->killswitch_active = 0;
    p->n_wfp_filters = 0;
    p->n_wfp_tun_filters = 0;
    LOG_INF("kill switch deactivated");
}

#endif /* _WIN32 */
