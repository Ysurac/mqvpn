// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 mp0rta and mqvpn contributors

/*
 * dns.c — DNS configuration for Windows
 *
 * Uses SetInterfaceDnsSettings() (Windows 10 1607+) to set DNS servers
 * on the TUN interface. Falls back to netsh if the API is unavailable.
 * An NRPT rule for "." then sends every query to those servers, so Windows
 * does not also ask other adapters' servers (DNS leak).
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
 * keeps queries on the tunnel's servers even where the NRPT rule below
 * cannot be installed or is overridden by group policy. Lives and dies with
 * the adapter: nothing to restore. */
#  define WIN_DNS_TUN_METRIC 0

/* ─── NRPT rule ───
 *
 * A Name Resolution Policy Table rule for the namespace "." with generic DNS
 * servers makes the DNS client send every query to those servers, whatever
 * the interface metrics. It is a registry key under the local (non group
 * policy) NRPT store, which the DNS client service reads.
 *
 * Crash safety: the key is created volatile (gone at reboot), and its name
 * carries the owning pid: a GUID whose leading groups are fixed for mqvpn
 * ("mqvp" "n" in hex) and whose last 8 hex digits are the pid. At startup
 * win_dns_flush_stale_nrpt() removes the mqvpn rules whose process no longer
 * runs, the Windows twin of the Linux kill switch's stale-rule cleanup.
 * Rules of other software never match the prefix and are left alone. */
#  define NRPT_LOCAL_KEY \
      L"SYSTEM\\CurrentControlSet\\Services\\Dnscache\\Parameters\\DnsPolicyConfig"
#  define NRPT_GPO_KEY \
      L"SOFTWARE\\Policies\\Microsoft\\Windows NT\\DNSClient\\DnsPolicyConfig"
#  define NRPT_NAME_PREFIX L"{6d717670-6e00-4000-8000-0000"
#  define NRPT_NAME_FMT    NRPT_NAME_PREFIX L"%08lx}"
#  define NRPT_STALE_MAX   32

typedef BOOL(WINAPI *DnsFlushResolverCache_fn)(void);

/* Drop cached answers obtained under the previous rules (dnsapi.dll's
 * undocumented but long-standing export, what `ipconfig /flushdns` calls). */
static void
nrpt_flush_resolver_cache(void)
{
    static DnsFlushResolverCache_fn fn = NULL;
    static int resolved = 0;
    if (!resolved) {
        resolved = 1;
        HMODULE h = LoadLibraryExW(L"dnsapi.dll", NULL, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (h)
            fn = (DnsFlushResolverCache_fn)(void (*)(void))GetProcAddress(
                h, "DnsFlushResolverCache");
    }
    if (!fn || !fn()) LOG_WRN("could not flush the DNS resolver cache");
}

/* Full path of rule `name` under the local NRPT store. */
static void
nrpt_key_path(const WCHAR *name, WCHAR *out, size_t cap)
{
    _snwprintf(out, cap, L"%ls\\%ls", NRPT_LOCAL_KEY, name);
    out[cap - 1] = L'\0';
}

static int
nrpt_set_sz(HKEY k, const WCHAR *name, const WCHAR *val, DWORD type)
{
    /* REG_MULTI_SZ needs a second terminator: callers pass it in val. */
    size_t len = wcslen(val) + 1;
    if (type == REG_MULTI_SZ) len++;
    return RegSetValueExW(k, name, 0, type, (const BYTE *)val,
                          (DWORD)(len * sizeof(WCHAR))) == ERROR_SUCCESS
               ? 0
               : -1;
}

static int
nrpt_set_dword(HKEY k, const WCHAR *name, DWORD val)
{
    return RegSetValueExW(k, name, 0, REG_DWORD, (const BYTE *)&val, sizeof(val)) ==
                   ERROR_SUCCESS
               ? 0
               : -1;
}

/* Group policy NRPT rules make Windows ignore the local store entirely. */
static void
nrpt_warn_if_gpo(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NRPT_GPO_KEY, 0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    DWORD n = 0;
    if (RegQueryInfoKeyW(k, NULL, NULL, NULL, &n, NULL, NULL, NULL, NULL, NULL, NULL,
                         NULL) == ERROR_SUCCESS &&
        n > 0)
        LOG_WRN("group policy NRPT rules are set: Windows ignores mqvpn's NRPT rule, "
                "other adapters' DNS servers may still be asked");
    RegCloseKey(k);
}

/* Install the "." rule for servers[0..n). 0 on success. */
static int
nrpt_add(platform_win_ctx_t *p, const char **servers, int n)
{
    WCHAR list[512] = {0};
    int off = 0;
    for (int i = 0; i < n; i++) {
        WCHAR addr[64];
        MultiByteToWideChar(CP_ACP, 0, servers[i], -1, addr, 64);
        int len = (int)wcslen(addr);
        if (off + len + 2 >= (int)(sizeof(list) / sizeof(list[0]))) break;
        if (i > 0) list[off++] = L';';
        memcpy(list + off, addr, len * sizeof(WCHAR));
        off += len;
    }
    list[off] = 0;

    /* The store's own key stays non-volatile: a volatile parent would stop
     * Windows tools from adding persistent rules under it until reboot. */
    HKEY parent;
    if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, NRPT_LOCAL_KEY, 0, NULL,
                        REG_OPTION_NON_VOLATILE, KEY_CREATE_SUB_KEY, NULL, &parent,
                        NULL) != ERROR_SUCCESS) {
        LOG_WRN("NRPT: cannot open the policy store");
        return -1;
    }
    WCHAR name[64];
    _snwprintf(name, 64, NRPT_NAME_FMT, (unsigned long)GetCurrentProcessId());
    name[63] = L'\0';
    HKEY k;
    LONG err = RegCreateKeyExW(parent, name, 0, NULL, REG_OPTION_VOLATILE, KEY_SET_VALUE,
                               NULL, &k, NULL);
    RegCloseKey(parent);
    if (err != ERROR_SUCCESS) {
        LOG_WRN("NRPT: cannot create the rule (error %ld)", err);
        return -1;
    }
    WCHAR comment[64];
    _snwprintf(comment, 64, L"mqvpn pid %lu", (unsigned long)GetCurrentProcessId());
    comment[63] = L'\0';
    /* The values PowerShell's Add-DnsClientNrptRule -Namespace "."
     * -NameServers writes: Version 2, ConfigOptions 8 = generic DNS
     * servers. */
    int rc = nrpt_set_dword(k, L"Version", 2) | nrpt_set_dword(k, L"ConfigOptions", 8) |
             nrpt_set_sz(k, L"Name", L".\0", REG_MULTI_SZ) |
             nrpt_set_sz(k, L"GenericDNSServers", list, REG_SZ) |
             nrpt_set_sz(k, L"IPSECCARestriction", L"", REG_SZ) |
             nrpt_set_sz(k, L"Comment", comment, REG_SZ);
    RegCloseKey(k);

    WCHAR path[256];
    nrpt_key_path(name, path, 256);
    if (rc != 0) {
        RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
        LOG_WRN("NRPT: cannot write the rule");
        return -1;
    }
    memcpy(p->nrpt_key, name, sizeof(p->nrpt_key));
    p->nrpt_active = 1;
    nrpt_warn_if_gpo();
    nrpt_flush_resolver_cache();
    return 0;
}

static void
nrpt_remove(platform_win_ctx_t *p)
{
    if (!p->nrpt_active) return;
    WCHAR path[256];
    nrpt_key_path(p->nrpt_key, path, 256);
    LONG err = RegDeleteKeyW(HKEY_LOCAL_MACHINE, path);
    if (err != ERROR_SUCCESS && err != ERROR_FILE_NOT_FOUND)
        LOG_WRN("NRPT: cannot remove the rule (error %ld)", err);
    p->nrpt_active = 0;
    nrpt_flush_resolver_cache();
}

/* The owner of a rule named for `pid` is gone: no such process, a process
 * that has exited, or the pid now belongs to another program. Our own pid
 * counts as stale: this runs before the process installs its rule, so one
 * with our pid was left by an earlier process. Anything undecidable (access
 * denied, image name unavailable) keeps the rule. */
static int
nrpt_pid_stale(DWORD pid)
{
    if (pid == GetCurrentProcessId()) return 1;
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return GetLastError() == ERROR_INVALID_PARAMETER;
    int stale = 0;
    DWORD code = 0;
    if (GetExitCodeProcess(h, &code) && code != STILL_ACTIVE) {
        stale = 1;
    } else {
        WCHAR theirs[MAX_PATH], ours[MAX_PATH];
        DWORD tlen = MAX_PATH;
        DWORD olen = GetModuleFileNameW(NULL, ours, MAX_PATH);
        if (QueryFullProcessImageNameW(h, 0, theirs, &tlen) && olen > 0 &&
            olen < MAX_PATH) {
            const WCHAR *tb = wcsrchr(theirs, L'\\');
            const WCHAR *ob = wcsrchr(ours, L'\\');
            stale = _wcsicmp(tb ? tb + 1 : theirs, ob ? ob + 1 : ours) != 0;
        }
    }
    CloseHandle(h);
    return stale;
}

void
win_dns_flush_stale_nrpt(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, NRPT_LOCAL_KEY, 0, KEY_READ, &k) !=
        ERROR_SUCCESS)
        return; /* no store, no rules */

    /* Collect first: deleting while enumerating shifts the indexes. */
    WCHAR stale[NRPT_STALE_MAX][64];
    int n_stale = 0;
    const size_t plen = wcslen(NRPT_NAME_PREFIX);
    for (DWORD i = 0; n_stale < NRPT_STALE_MAX; i++) {
        WCHAR name[256];
        DWORD nlen = 256;
        LONG err = RegEnumKeyExW(k, i, name, &nlen, NULL, NULL, NULL, NULL);
        if (err == ERROR_NO_MORE_ITEMS) break;
        if (err != ERROR_SUCCESS) continue;
        /* Exactly the shape nrpt_add writes: prefix, 8 hex digits, '}'. */
        if (nlen != plen + 9 || _wcsnicmp(name, NRPT_NAME_PREFIX, plen) != 0 ||
            name[plen + 8] != L'}')
            continue;
        WCHAR hex[9];
        memcpy(hex, name + plen, 8 * sizeof(WCHAR));
        hex[8] = 0;
        WCHAR *end = NULL;
        unsigned long pid = wcstoul(hex, &end, 16);
        if (!end || *end != 0) continue;
        if (nrpt_pid_stale((DWORD)pid)) memcpy(stale[n_stale++], name, sizeof(stale[0]));
    }
    RegCloseKey(k);

    for (int i = 0; i < n_stale; i++) {
        WCHAR path[256];
        nrpt_key_path(stale[i], path, 256);
        if (RegDeleteKeyW(HKEY_LOCAL_MACHINE, path) == ERROR_SUCCESS)
            LOG_INF("removed a stale mqvpn NRPT rule (%ls)", stale[i]);
    }
    if (n_stale > 0) nrpt_flush_resolver_cache();
}

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
        goto nrpt;
    }

fallback:
    if (set_dns_netsh(p->tun.name, servers, n) < 0) return -1;

    p->dns_configured = 1;
    p->dns_method = WIN_DNS_METHOD_NETSH;
    LOG_INF("DNS configured via netsh (%d servers)", n);

nrpt:
    if (nrpt_add(p, servers, n) == 0)
        LOG_INF("DNS: NRPT rule set, all queries go to the tunnel's servers");
    else
        LOG_WRN("DNS: no NRPT rule; other adapters' DNS servers may still be asked");
    return 0;
}

void
win_cleanup_dns(platform_win_ctx_t *p)
{
    nrpt_remove(p);
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
