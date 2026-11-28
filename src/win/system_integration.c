/* =====================================================================
 *  HopDesk - Windows Autostart (HKCU Run) & System Permission Actions
 * ===================================================================== */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "app.h"
#include "../ui/hopdesk_ui.h"
#include <shellapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const wchar_t k_run_subkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t k_run_value[] = L"HopDesk";
static LONG s_perm_uac_busy;

void win_get_app_path(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    wchar_t path[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    (void)WideCharToMultiByte(CP_UTF8, 0, path, -1, out, (int)cap, NULL, NULL);
}

int win_is_elevated(void) {
    HANDLE token = NULL;
    TOKEN_ELEVATION elev;
    DWORD cb = sizeof(elev);
    int elevated = 0;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        memset(&elev, 0, sizeof(elev));
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &cb))
            elevated = elev.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

int win_autostart_is_enabled(void) {
    HKEY key = NULL;
    wchar_t buf[1024] = {0};
    DWORD type = 0;
    DWORD bytes = sizeof(buf) - sizeof(wchar_t);
    int enabled = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, k_run_subkey, 0, KEY_QUERY_VALUE, &key) == ERROR_SUCCESS) {
        if (RegQueryValueExW(key, k_run_value, NULL, &type, (LPBYTE)buf, &bytes) == ERROR_SUCCESS &&
            (type == REG_SZ || type == REG_EXPAND_SZ) && buf[0] != L'\0') {
            enabled = 1;
        }
        RegCloseKey(key);
    }
    return enabled;
}

int win_autostart_set_enabled(int enabled) {
    HKEY key = NULL;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, k_run_subkey, 0, NULL, 0,
                              KEY_SET_VALUE, NULL, &key, NULL);
    if (rc != ERROR_SUCCESS) {
        hop_log(HOP_LOG_ERROR, "autostart: RegCreateKeyExW failed (%ld)", rc);
        return 0;
    }
    int ok = 0;
    if (enabled) {
        wchar_t exe[MAX_PATH] = {0};
        wchar_t cmd[MAX_PATH + 32] = {0};
        DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            swprintf_s(cmd, ARRAYSIZE(cmd), L"\"%ls\" --agent", exe);
            DWORD bytes = (DWORD)((wcslen(cmd) + 1) * sizeof(wchar_t));
            rc = RegSetValueExW(key, k_run_value, 0, REG_SZ, (const BYTE *)cmd, bytes);
            ok = (rc == ERROR_SUCCESS);
            if (!ok) hop_log(HOP_LOG_ERROR, "autostart: RegSetValueExW failed (%ld)", rc);
            else hop_log(HOP_LOG_INFO, "autostart: enabled HKCU Run entry");
        }
    } else {
        rc = RegDeleteValueW(key, k_run_value);
        ok = (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND);
        if (!ok) hop_log(HOP_LOG_ERROR, "autostart: RegDeleteValueW failed (%ld)", rc);
        else hop_log(HOP_LOG_INFO, "autostart: disabled HKCU Run entry");
    }
    RegCloseKey(key);
    return ok;
}

typedef struct {
    int action;
    DWORD agent_pid;
    HWND hwnd;
    wchar_t exe[MAX_PATH];
    wchar_t cfg[1024];
} win_uac_job;

static DWORD WINAPI uac_action_worker(LPVOID arg) {
    win_uac_job *job = (win_uac_job *)arg;
    if (job) {
        if (job->action == HOPDESK_UI_PERM_ELEVATE) {
            wchar_t args[1200] = {0};
            if (job->cfg[0]) {
                swprintf_s(args, ARRAYSIZE(args), L"--wait-pid %lu --config \"%ls\"",
                           (unsigned long)job->agent_pid, job->cfg);
            } else {
                swprintf_s(args, ARRAYSIZE(args), L"--wait-pid %lu",
                           (unsigned long)job->agent_pid);
            }
            HINSTANCE res = ShellExecuteW(NULL, L"runas", job->exe, args, NULL, SW_SHOWNORMAL);
            if ((INT_PTR)res > 32) {
                hop_log(HOP_LOG_INFO, "permission: elevated relaunch approved; closing standard instance");
                if (job->hwnd) PostMessageW(job->hwnd, WM_CLOSE, 0, 0);
            } else {
                hop_log(HOP_LOG_WARN, "permission: elevated relaunch canceled or failed (%lld)",
                        (long long)(INT_PTR)res);
            }
        } else if (job->action == HOPDESK_UI_PERM_FIREWALL_ALLOW) {
            wchar_t args[MAX_PATH + 192] = {0};
            swprintf_s(args, ARRAYSIZE(args),
                       L"advfirewall firewall add rule name=\"HopDesk\" dir=in action=allow "
                       L"program=\"%ls\" enable=yes profile=private,domain",
                       job->exe);
            HINSTANCE res = ShellExecuteW(NULL, L"runas", L"netsh.exe", args, NULL, SW_HIDE);
            if ((INT_PTR)res > 32) {
                hop_log(HOP_LOG_INFO, "permission: requested Windows Firewall allow rule via UAC");
            } else {
                hop_log(HOP_LOG_WARN, "permission: Windows Firewall rule request canceled or failed (%lld)",
                        (long long)(INT_PTR)res);
            }
        }
        free(job);
    }
    InterlockedExchange(&s_perm_uac_busy, 0);
    return 0;
}

int win_permission_action(int action) {
    wchar_t exe[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;

    if (action == HOPDESK_UI_PERM_ELEVATE || action == HOPDESK_UI_PERM_FIREWALL_ALLOW) {
        if (action == HOPDESK_UI_PERM_ELEVATE && win_is_elevated()) return 1;
        if (InterlockedCompareExchange(&s_perm_uac_busy, 1, 0) != 0) return 0;
        win_uac_job *job = (win_uac_job *)calloc(1, sizeof(*job));
        if (!job) {
            InterlockedExchange(&s_perm_uac_busy, 0);
            return 0;
        }
        job->action = action;
        job->agent_pid = GetCurrentProcessId();
        job->hwnd = g_app.hwnd;
        wcscpy_s(job->exe, ARRAYSIZE(job->exe), exe);
        if (g_app.config_path[0]) {
            (void)MultiByteToWideChar(CP_UTF8, 0, g_app.config_path, -1,
                                      job->cfg, (int)ARRAYSIZE(job->cfg));
        }
        HANDLE t = CreateThread(NULL, 0, uac_action_worker, job, 0, NULL);
        if (!t) {
            free(job);
            InterlockedExchange(&s_perm_uac_busy, 0);
            return 0;
        }
        CloseHandle(t);
        return 1;
    }

    if (action == HOPDESK_UI_PERM_FIREWALL_OPEN) {
        HINSTANCE res = ShellExecuteW(NULL, L"open", L"control.exe",
                                      L"/name Microsoft.WindowsFirewall /page pageConfigureApps",
                                      NULL, SW_SHOWNORMAL);
        return (INT_PTR)res > 32;
    }

    return 0;
}
