// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * dns.c — DNS configuration for Windows
 *
 * Uses SetInterfaceDnsSettings() (Windows 10 1607+) to set DNS servers
 * on the TUN interface. Falls back to netsh if the API is unavailable.
 */

#ifdef _WIN32

#  include "platform_internal_win.h"
#  include "log.h"

#  include <stdio.h>
#  include <string.h>

/*
 * SetInterfaceDnsSettings is available from Windows 10 1607+.
 * We try to load it dynamically to allow compilation on older SDKs.
 */

/* DNS_INTERFACE_SETTINGS is available in newer Windows SDKs (netioapi.h).
 * Only define it ourselves when compiling with older SDKs. */
#  ifndef DNS_INTERFACE_SETTINGS_VERSION1
typedef struct _DNS_INTERFACE_SETTINGS {
    ULONG Version; /* DNS_INTERFACE_SETTINGS_VERSION1 = 1 */
    ULONG64 Flags;
    PWSTR Domain;
    PWSTR NameServer;
    PWSTR SearchList;
    ULONG RegistrationEnabled;
    ULONG RegisterAdapterName;
    ULONG EnableLLMNR;
    ULONG QueryAdapterName;
    PWSTR ProfileNameServer;
} DNS_INTERFACE_SETTINGS;

#    define DNS_INTERFACE_SETTINGS_VERSION1 1
#    define DNS_SETTING_NAMESERVER          0x0004
#  endif

typedef DWORD(WINAPI *SetInterfaceDnsSettings_fn)(REFGUID InterfaceGuid,
                                                  const DNS_INTERFACE_SETTINGS *Settings);

typedef DWORD(WINAPI *GetInterfaceDnsSettings_fn)(REFGUID InterfaceGuid,
                                                  DNS_INTERFACE_SETTINGS *Settings);

static SetInterfaceDnsSettings_fn pSetInterfaceDnsSettings = NULL;
static int dns_api_resolved = 0;

static void
resolve_dns_api(void)
{
    if (dns_api_resolved) return;
    dns_api_resolved = 1;

    HMODULE h = LoadLibraryA("iphlpapi.dll");
    if (h) {
        pSetInterfaceDnsSettings =
            (SetInterfaceDnsSettings_fn)GetProcAddress(h, "SetInterfaceDnsSettings");
    }
}

/* Helper: run netsh.exe directly via CreateProcessW (no shell) */
static int
run_netsh(const WCHAR *args)
{
    WCHAR cmd[1024];
    _snwprintf(cmd, sizeof(cmd) / sizeof(cmd[0]), L"netsh.exe %s", args);
    cmd[(sizeof(cmd) / sizeof(cmd[0])) - 1] = L'\0';

    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi;
    memset(&pi, 0, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si,
                        &pi)) {
        LOG_ERR("CreateProcessW(netsh): error %lu", GetLastError());
        return -1;
    }

    WaitForSingleObject(pi.hProcess, 10000);

    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return (exit_code == 0) ? 0 : -1;
}

/* Fallback: use netsh to set DNS (via CreateProcessW, no shell) */
static int
set_dns_netsh(const char *adapter_name, const char **servers, int n)
{
    WCHAR wname[256], waddr[64], args[512];
    MultiByteToWideChar(CP_ACP, 0, adapter_name, -1, wname, 256);

    /* Set primary DNS */
    MultiByteToWideChar(CP_ACP, 0, servers[0], -1, waddr, 64);
    _snwprintf(args, 512, L"interface ip set dnsservers \"%s\" static %s primary", wname,
               waddr);
    if (run_netsh(args) != 0) {
        LOG_ERR("netsh set primary DNS failed");
        return -1;
    }

    /* Add secondary DNS servers */
    for (int i = 1; i < n; i++) {
        MultiByteToWideChar(CP_ACP, 0, servers[i], -1, waddr, 64);
        _snwprintf(args, 512, L"interface ip add dnsservers \"%s\" %s index=%d", wname,
                   waddr, i + 1);
        run_netsh(args);
    }

    return 0;
}

/* Fallback: clear DNS via netsh (via CreateProcessW, no shell) */
static void
clear_dns_netsh(const char *adapter_name)
{
    WCHAR wname[256], args[512];
    MultiByteToWideChar(CP_ACP, 0, adapter_name, -1, wname, 256);
    _snwprintf(args, 512, L"interface ip set dnsservers \"%s\" dhcp", wname);
    run_netsh(args);
}

/* Only IP literals reach SetInterfaceDnsSettings and the netsh command line
 * (an unquoted argument there): anything else is dropped with a warning. */
static int
dns_server_valid(const char *s)
{
    unsigned char scratch[sizeof(struct in6_addr)];
    return inet_pton(AF_INET, s, scratch) == 1 || inet_pton(AF_INET6, s, scratch) == 1;
}

/* Interface metric pinned on the TUN when it carries the DNS override.
 * Windows asks the DNS servers of the lowest-metric interface first, so this
 * keeps queries on the tunnel's servers instead of racing the physical
 * adapters'. It does not stop every query from leaving elsewhere (Windows
 * can still fall back to other interfaces' servers); KillSwitch blocks
 * those. An NRPT "." rule would, but it is a registry policy that outlives a
 * crashed process, so it is not used here. Lives and dies with the adapter:
 * nothing to restore. */
#  define WIN_DNS_TUN_METRIC 0

int
win_setup_dns(platform_win_ctx_t *p)
{
    if (p->dns_configured || p->n_dns <= 0) return 0;

    const char *servers[4];
    int n = 0;
    for (int i = 0; i < p->n_dns && i < 4; i++) {
        if (!dns_server_valid(p->dns_servers[i])) {
            LOG_WRN("DNS server '%s' is not an IP address, ignored", p->dns_servers[i]);
            continue;
        }
        servers[n++] = p->dns_servers[i];
    }
    if (n == 0) {
        LOG_ERR("no valid DNS server address");
        return -1;
    }

    if (mqvpn_tun_win_set_metric(&p->tun, WIN_DNS_TUN_METRIC) < 0)
        LOG_WRN("could not lower the TUN interface metric; other adapters' DNS "
                "servers may be preferred");

    resolve_dns_api();

    if (pSetInterfaceDnsSettings) {
        /* Build "addr1,addr2,..." nameserver string */
        WCHAR ns[512] = {0};
        int off = 0;
        for (int i = 0; i < n; i++) {
            WCHAR addr[64];
            MultiByteToWideChar(CP_ACP, 0, servers[i], -1, addr, 64);
            if (i > 0) ns[off++] = L',';
            int len = (int)wcslen(addr);
            memcpy(ns + off, addr, len * sizeof(WCHAR));
            off += len;
        }
        ns[off] = 0;

        /* Get interface GUID from LUID */
        GUID if_guid;
        DWORD err = ConvertInterfaceLuidToGuid(&p->tun.luid, &if_guid);
        if (err != NO_ERROR) {
            LOG_ERR("ConvertInterfaceLuidToGuid: error %lu", err);
            goto fallback;
        }

        DNS_INTERFACE_SETTINGS settings;
        memset(&settings, 0, sizeof(settings));
        settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
        settings.Flags = DNS_SETTING_NAMESERVER;
        settings.NameServer = ns;

        err = pSetInterfaceDnsSettings(&if_guid, &settings);
        if (err != NO_ERROR) {
            LOG_WRN("SetInterfaceDnsSettings: error %lu, trying netsh", err);
            goto fallback;
        }

        p->dns_configured = 1;
        p->dns_method = WIN_DNS_METHOD_API;
        LOG_INF("DNS configured via SetInterfaceDnsSettings (%d servers)", n);
        return 0;
    }

fallback:
    if (set_dns_netsh(p->tun.name, servers, n) < 0) return -1;

    p->dns_configured = 1;
    p->dns_method = WIN_DNS_METHOD_NETSH;
    LOG_INF("DNS configured via netsh (%d servers)", n);
    return 0;
}

void
win_cleanup_dns(platform_win_ctx_t *p)
{
    if (!p->dns_configured) return;

    /* Undo with the method that applied it: the API's presence says nothing
     * about which one did (the API path falls back to netsh on error). */
    if (p->dns_method == WIN_DNS_METHOD_API && pSetInterfaceDnsSettings) {
        GUID if_guid;
        if (ConvertInterfaceLuidToGuid(&p->tun.luid, &if_guid) == NO_ERROR) {
            DNS_INTERFACE_SETTINGS settings;
            memset(&settings, 0, sizeof(settings));
            settings.Version = DNS_INTERFACE_SETTINGS_VERSION1;
            settings.Flags = DNS_SETTING_NAMESERVER;
            settings.NameServer = L"";

            pSetInterfaceDnsSettings(&if_guid, &settings);
        }
    } else if (p->dns_method == WIN_DNS_METHOD_NETSH) {
        clear_dns_netsh(p->tun.name);
    }

    p->dns_configured = 0;
    p->dns_method = WIN_DNS_METHOD_NONE;
    LOG_INF("DNS restored");
}

#endif /* _WIN32 */
