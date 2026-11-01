#include "app.h"
#include "update_manifest.h"
#include "../core/update_state.h"
#include "xfer.h"

#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#define UPDATE_MAX_DOWNLOAD (512ull * 1024ull * 1024ull)

static volatile LONG s_update_busy;

static int utf8_wide(const char *src, wchar_t *dst, int cap) {
    int n;
    if (!src || !dst || cap <= 0) return 0;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, cap);
    return n > 0;
}

static int host_allowed(const wchar_t *host, const wchar_t *path, int manifest) {
    if (manifest) {
        return _wcsicmp(host, L"raw.githubusercontent.com") == 0 &&
               wcscmp(path, L"/alex520lq/HopDesk-Releases/main/latest.txt") == 0;
    }
    static const wchar_t prefix[] = L"/alex520lq/HopDesk-Releases/releases/download/v";
    return _wcsicmp(host, L"github.com") == 0 &&
           wcsncmp(path, prefix, wcslen(prefix)) == 0;
}

static HINTERNET http_open_get(const char *url, int manifest,
                               HINTERNET *session_out, HINTERNET *connect_out) {
    wchar_t wide_url[2048], host[256], path[1536], extra[256], object[1800];
    URL_COMPONENTSW parts;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;

    if (!utf8_wide(url, wide_url, (int)(sizeof(wide_url) / sizeof(wide_url[0])))) return NULL;
    memset(&parts, 0, sizeof(parts));
    parts.dwStructSize = sizeof(parts);
    parts.lpszHostName = host; parts.dwHostNameLength = (DWORD)(sizeof(host) / sizeof(host[0]));
    parts.lpszUrlPath = path; parts.dwUrlPathLength = (DWORD)(sizeof(path) / sizeof(path[0]));
    parts.lpszExtraInfo = extra; parts.dwExtraInfoLength = (DWORD)(sizeof(extra) / sizeof(extra[0]));
    if (!WinHttpCrackUrl(wide_url, 0, 0, &parts) || parts.nScheme != INTERNET_SCHEME_HTTPS ||
        parts.dwHostNameLength >= sizeof(host) / sizeof(host[0]) ||
        parts.dwUrlPathLength >= sizeof(path) / sizeof(path[0]) ||
        parts.dwExtraInfoLength >= sizeof(extra) / sizeof(extra[0])) return NULL;
    host[parts.dwHostNameLength] = L'\0';
    path[parts.dwUrlPathLength] = L'\0';
    extra[parts.dwExtraInfoLength] = L'\0';
    if (!host_allowed(host, path, manifest)) return NULL;
    if (swprintf(object, sizeof(object) / sizeof(object[0]), L"%ls%ls", path, extra) < 0) return NULL;

    session = WinHttpOpen(L"HopDesk updater/1.0",
                          WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                          WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) goto fail;
    WinHttpSetTimeouts(session, 10000, 10000, 15000, 30000);
    connect = WinHttpConnect(session, host, parts.nPort, 0);
    if (!connect) goto fail;
    request = WinHttpOpenRequest(connect, L"GET", object, NULL,
                                 WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                                 WINHTTP_FLAG_SECURE);
    if (!request) goto fail;
    (void)WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY,
                           &redirect_policy, sizeof(redirect_policy));
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                            WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) goto fail;
    DWORD status = 0, status_size = sizeof(status);
    if (!WinHttpQueryHeaders(request,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
            WINHTTP_NO_HEADER_INDEX) || status != 200) goto fail;

    *session_out = session;
    *connect_out = connect;
    return request;

fail:
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
    return NULL;
}

static void http_close(HINTERNET request, HINTERNET connect, HINTERNET session) {
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    if (session) WinHttpCloseHandle(session);
}

static int http_get_manifest(char *out, size_t cap) {
    HINTERNET session = NULL, connect = NULL;
    HINTERNET request = http_open_get(HOP_UPDATE_MANIFEST_URL, 1, &session, &connect);
    size_t used = 0;
    int ok = 0;
    if (!request) return 0;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) break;
        if (!available) { ok = used > 0; break; }
        if ((size_t)available >= cap - used) break;
        DWORD read = 0;
        if (!WinHttpReadData(request, out + used, available, &read) || !read) break;
        used += read;
    }
    if (ok) out[used] = '\0';
    http_close(request, connect, session);
    return ok;
}

static int write_all(HANDLE file, const BYTE *data, DWORD len) {
    DWORD offset = 0;
    while (offset < len) {
        DWORD written = 0;
        if (!WriteFile(file, data + offset, len - offset, &written, NULL) || !written)
            return 0;
        offset += written;
    }
    return 1;
}

static int http_download_sha256(const char *url, const wchar_t *path,
                                const char *expected_hex) {
    HINTERNET session = NULL, connect = NULL;
    HINTERNET request = http_open_get(url, 0, &session, &connect);
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR hash_object = NULL;
    BYTE digest[32], buffer[64 * 1024];
    ULONG object_len = 0, hash_len = 0, got = 0;
    ULONGLONG total = 0;
    char actual[65];
    int ok = 0;

    if (!request) return 0;
    file = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_NEW,
                       FILE_ATTRIBUTE_TEMPORARY, NULL);
    if (file == INVALID_HANDLE_VALUE) goto done;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH,
                                         (PUCHAR)&object_len, sizeof(object_len), &got, 0)) ||
        !BCRYPT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH,
                                         (PUCHAR)&hash_len, sizeof(hash_len), &got, 0)) ||
        hash_len != sizeof(digest)) goto done;
    hash_object = (PUCHAR)HeapAlloc(GetProcessHeap(), 0, object_len);
    if (!hash_object || !BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, hash_object,
                                                         object_len, NULL, 0, 0))) goto done;

    for (;;) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request, &available)) goto done;
        if (!available) break;
        while (available) {
            DWORD want = available > sizeof(buffer) ? (DWORD)sizeof(buffer) : available;
            DWORD read = 0;
            if (!WinHttpReadData(request, buffer, want, &read) || !read) goto done;
            total += read;
            if (total > UPDATE_MAX_DOWNLOAD ||
                !write_all(file, buffer, read) ||
                !BCRYPT_SUCCESS(BCryptHashData(hash, buffer, read, 0))) goto done;
            available -= read;
        }
    }
    if (!total || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) goto done;
    for (int i = 0; i < 32; ++i)
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    actual[64] = '\0';
    if (_stricmp(actual, expected_hex) != 0) goto done;
    if (!FlushFileBuffers(file)) goto done;
    ok = 1;

done:
    if (hash) BCryptDestroyHash(hash);
    if (hash_object) HeapFree(GetProcessHeap(), 0, hash_object);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    http_close(request, connect, session);
    if (!ok) DeleteFileW(path);
    return ok;
}

static int launch_installer(HWND owner, const char *url, const char *sha256, const char *version) {
    wchar_t temp[MAX_PATH], archive[MAX_PATH], exe[MAX_PATH], helper[MAX_PATH], helper_run[MAX_PATH];
    wchar_t command[8192], target_dir[MAX_PATH], transaction[MAX_PATH], prepared[MAX_PATH], wversion[64];
    char nonce[33]; hop_session_id id;
    DWORD n;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *slash, *dot;

    n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp) || !GetTempFileNameW(temp, L"hop", 0, archive))
        return 0;
    DeleteFileW(archive); /* GetTempFileName creates a .tmp: do not leak it. */
    dot = wcsrchr(archive, L'.');
    if (dot) wcscpy_s(dot, 5, L".zip");
    DeleteFileW(archive);

    if (!http_download_sha256(url, archive, sha256)) {
        MessageBoxW(owner, L"更新包下载失败或 SHA-256 校验不匹配。旧版本未被修改。",
                    L"HopDesk 更新", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 0;
    }

    n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) { DeleteFileW(archive); return 0; }
    slash = wcsrchr(exe, L'\\');
    if (!slash) { DeleteFileW(archive); return 0; }
    *slash = L'\0';
    wcscpy_s(target_dir, ARRAYSIZE(target_dir), exe);
    if (swprintf(helper, ARRAYSIZE(helper), L"%ls\\update-windows.ps1", target_dir) < 0 ||
        GetFileAttributesW(helper) == INVALID_FILE_ATTRIBUTES) {
        DeleteFileW(archive);
        MessageBoxW(owner, L"找不到安全更新辅助脚本。请重新安装当前版本后再更新。",
                    L"HopDesk 更新", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return 0;
    }

    /* Close admission only at install time, after download and user confirmation.
     * Producer/receiver reservations make the busy check race-free. */
    if (!hop_update_install_begin(xfer_busy)) {
        DeleteFileW(archive);
        MessageBoxW(owner, L"传输已开始或仍在扫描。请等待完成后重试更新。",
                    L"HopDesk 更新", MB_OK | MB_ICONWARNING);
        return 0;
    }
    if (!hop_session_generate(&id) || !utf8_wide(version,wversion,ARRAYSIZE(wversion))) goto prepare_fail;
    hop_session_hex(&id,nonce);
    wchar_t wnonce[33];
    if (!utf8_wide(nonce,wnonce,ARRAYSIZE(wnonce)) ||
        swprintf(transaction,ARRAYSIZE(transaction),L"%lsHopDesk-update-%ls",temp,wnonce)<0 ||
        !CreateDirectoryW(transaction,NULL)) goto prepare_fail;
    if (swprintf(helper_run,ARRAYSIZE(helper_run),L"%ls\\installer.ps1",transaction)<0 ||
        swprintf(prepared,ARRAYSIZE(prepared),L"%ls\\prepared",transaction)<0 ||
        !CopyFileW(helper,helper_run,TRUE)) {
        RemoveDirectoryW(transaction); goto prepare_fail;
    }
    /* powershell.exe is Windows PowerShell 5.1. Add a UTF-8 BOM to its temporary
     * script so non-ASCII diagnostics parse consistently on non-UTF8 systems. */
    {
        HANDLE f=CreateFileW(helper_run,GENERIC_READ|GENERIC_WRITE,0,NULL,OPEN_EXISTING,0,NULL);
        DWORD size=f==INVALID_HANDLE_VALUE?0:GetFileSize(f,NULL),read=0;
        BYTE *bytes=size && size<1024*1024?(BYTE *)malloc(size):NULL;
        int ok=bytes && ReadFile(f,bytes,size,&read,NULL) && read==size;
        if (ok) {
            SetFilePointer(f,0,NULL,FILE_BEGIN);
            BYTE bom[3]={0xef,0xbb,0xbf};
            ok=write_all(f,bom,3) && write_all(f,bytes,size) && SetEndOfFile(f);
        }
        free(bytes); if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);
        if(!ok) { DeleteFileW(helper_run);RemoveDirectoryW(transaction);goto prepare_fail; }
    }

    if (swprintf(command, ARRAYSIZE(command),
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
        L"-File \"%ls\" -Archive \"%ls\" -TargetDir \"%ls\" -ProcessId %lu -Transaction \"%ls\" -Nonce %hs -ExpectedVersion %ls",
        helper_run, archive, target_dir, GetCurrentProcessId(), transaction, nonce, wversion) < 0) {
        DeleteFileW(archive);
        DeleteFileW(helper_run);
        hop_update_install_end();
        RemoveDirectoryW(transaction);
        return 0;
    }
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, target_dir, &si, &pi)) {
        DeleteFileW(archive);
        DeleteFileW(helper_run);
        MessageBoxW(owner, L"无法启动更新辅助程序。旧版本未被修改。",
                    L"HopDesk 更新", MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        hop_update_install_end();
        RemoveDirectoryW(transaction);
        return 0;
    }
    CloseHandle(pi.hThread);
    /* No user-facing modal exists after the helper starts its exit deadline.
     * Keep the old app alive unless preparation actually succeeded. */
    int prepared_ok=0;
    for (int i=0;i<240;i++) {
        if (WaitForSingleObject(pi.hProcess,0)==WAIT_OBJECT_0) break;
        HANDLE ack=CreateFileW(prepared,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(ack!=INVALID_HANDLE_VALUE) {
            char value[64]={0};DWORD got=0;
            prepared_ok=ReadFile(ack,value,63,&got,NULL) && !memcmp(value,nonce,32) &&
                ((got==33 && value[32]=='\n') || (got==34 && value[32]=='\r' && value[33]=='\n'));
            CloseHandle(ack);
            if(prepared_ok)break;
        }
        Sleep(250);
    }
    if (!prepared_ok) {
        /* It cannot have replaced a live old executable: safe to cancel here. */
        TerminateProcess(pi.hProcess,1); WaitForSingleObject(pi.hProcess,5000);
        CloseHandle(pi.hProcess);
        hop_update_install_end();
        DeleteFileW(helper_run);DeleteFileW(archive);DeleteFileW(prepared);RemoveDirectoryW(transaction);
        MessageBoxW(owner,L"更新准备失败，当前版本继续运行。",L"HopDesk 更新",MB_OK|MB_ICONERROR);
        return 0;
    }
    CloseHandle(pi.hProcess);
    if (!PostMessageW(owner, WM_CLOSE, 0, 0)) {
        hop_update_install_end();
        return 0; /* helper times out safely without replacing the live app */
    }
    return 1; /* admission stays closed until this process exits */

prepare_fail:
    DeleteFileW(archive);
    hop_update_install_end();
    MessageBoxW(owner,L"无法准备更新事务，当前版本继续运行。",L"HopDesk 更新",MB_OK|MB_ICONERROR);
    return 0;
}

static DWORD WINAPI update_worker(void *arg) {
    HWND owner = (HWND)arg;
    char text[HOP_UPDATE_MANIFEST_MAX + 1];
    hop_update_manifest manifest;
    int valid = 0, cmp;
    wchar_t message[1024], version[64], current[64];

    if (xfer_busy()) {
        MessageBoxW(owner, L"当前仍有文件传输任务。请等待传输结束后再更新，避免中断文件传输。",
                    L"HopDesk 更新", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        goto done;
    }
    if (!http_get_manifest(text, sizeof(text)) ||
        !hop_update_manifest_parse(text, strlen(text), &manifest)) {
        MessageBoxW(owner, L"无法读取有效的更新清单。请检查网络，或稍后重试。",
                    L"HopDesk 更新", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        goto done;
    }
    cmp = hop_version_compare(HOP_VERSION, manifest.version, &valid);
    if (!valid) {
        MessageBoxW(owner, L"当前构建版本不是正式版本，无法执行自动更新。",
                    L"HopDesk 更新", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        goto done;
    }
    if (cmp >= 0) {
        if (!utf8_wide(HOP_VERSION, version, ARRAYSIZE(version))) {
            MessageBoxW(owner, L"当前构建版本信息无效。", L"HopDesk 更新",
                        MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
            goto done;
        }
        swprintf(message, ARRAYSIZE(message), L"当前已经是最新版本（v%ls）。", version);
        MessageBoxW(owner, message, L"HopDesk 更新", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        goto done;
    }

    if (!utf8_wide(manifest.version, version, ARRAYSIZE(version)) ||
        !utf8_wide(HOP_VERSION, current, ARRAYSIZE(current))) {
        MessageBoxW(owner, L"版本信息无效，已取消更新。", L"HopDesk 更新",
                    MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
        goto done;
    }
    swprintf(message, ARRAYSIZE(message),
        L"发现新版本 v%ls（当前 v%ls）。\n\n是否下载并自动安装？",
        version, current);
    if (MessageBoxW(owner, message, L"HopDesk 更新",
                    MB_YESNO | MB_ICONINFORMATION | MB_SETFOREGROUND) == IDYES) {
        (void)launch_installer(owner, manifest.windows_url, manifest.windows_sha256, manifest.version);
    }

done:
    InterlockedExchange(&s_update_busy, 0);
    return 0;
}

void update_check_async(HWND owner) {
    HANDLE thread;
    if (InterlockedCompareExchange(&s_update_busy, 1, 0) != 0) return;
    thread = CreateThread(NULL, 0, update_worker, owner, 0, NULL);
    if (!thread) {
        InterlockedExchange(&s_update_busy, 0);
        MessageBoxW(owner, L"无法启动更新检查任务。", L"HopDesk 更新",
                    MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
        return;
    }
    CloseHandle(thread);
}
