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
#include <string.h>
#include <wchar.h>

static const wchar_t k_run_subkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t k_run_value[] = L"HopDesk";

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

int win_permission_action(int action) {
    wchar_t exe[MAX_PATH] = {0};
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;

    if (action == HOPDESK_UI_PERM_ELEVATE) {
        if (win_is_elevated()) return 1;
        wchar_t args[64] = {0};
        swprintf_s(args, ARRAYSIZE(args), L"--wait-pid %lu", (unsigned long)GetCurrentProcessId());
        HINSTANCE res = ShellExecuteW(NULL, L"runas", exe, args, NULL, SW_SHOWNORMAL);
        if ((INT_PTR)res > 32) {
            hop_log(HOP_LOG_INFO, "permission: elevated relaunch approved; closing standard instance");
            if (g_app.hwnd) PostMessageW(g_app.hwnd, WM_CLOSE, 0, 0);
            return 1;
        }
        hop_log(HOP_LOG_WARN, "permission: elevated relaunch canceled or failed (%lld)", (long long)(INT_PTR)res);
        return 0;
    }

    if (action == HOPDESK_UI_PERM_FIREWALL_ALLOW) {
        wchar_t args[MAX_PATH + 192] = {0};
        swprintf_s(args, ARRAYSIZE(args),
                   L"advfirewall firewall add rule name=\"HopDesk\" dir=in action=allow "
                   L"program=\"%ls\" enable=yes profile=private,domain",
                   exe);
        HINSTANCE res = ShellExecuteW(NULL, L"runas", L"netsh.exe", args, NULL, SW_HIDE);
        if ((INT_PTR)res > 32) {
            hop_log(HOP_LOG_INFO, "permission: requested Windows Firewall allow rule via UAC");
            return 1;
        }
        hop_log(HOP_LOG_WARN, "permission: Windows Firewall rule request canceled or failed (%lld)", (long long)(INT_PTR)res);
        return 0;
    }

    if (action == HOPDESK_UI_PERM_FIREWALL_OPEN) {
        HINSTANCE res = ShellExecuteW(NULL, L"open", L"control.exe",
                                      L"/name Microsoft.WindowsFirewall /page pageConfigureApps",
                                      NULL, SW_SHOWNORMAL);
        return (INT_PTR)res > 32;
    }

    return 0;
}
