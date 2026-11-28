#include "app.h"
#include "update.h"
#include "update_manifest.h"
#include "../core/update_controller.h"
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

typedef struct {
    HWND owner;
    int  action;
    int  channel;
} win_update_req;

static SRWLOCK s_state_lock = SRWLOCK_INIT;
static hop_update_snapshot s_snap;
static hop_update_manifest s_manifest;
static int s_state_inited;
static int s_has_manifest;
static int s_staged_ready;
static int s_applying_handoff;
static wchar_t s_staged_archive[MAX_PATH];
static volatile LONG s_update_busy;

static int current_build_channel(void) {
    return hop_update_channel_from_name(HOP_CHANNEL) == HOP_UPDATE_CHANNEL_DEV
        ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
}

static void ensure_state_inited_locked(void) {
    if (!s_state_inited) {
        hop_update_snapshot_init(&s_snap, g_app.cfg.update_channel);
        s_state_inited = 1;
    }
}

static void clear_staged_locked(void) {
    if (s_staged_archive[0] && !s_applying_handoff)
        DeleteFileW(s_staged_archive);
    s_staged_archive[0] = L'\0';
    s_staged_ready = 0;
}

void update_get_snapshot(hop_update_snapshot *out) {
    if (!out) return;
    AcquireSRWLockShared(&s_state_lock);
    if (!s_state_inited) {
        hop_update_snapshot_init(out, g_app.cfg.update_channel);
    } else {
        *out = s_snap;
    }
    ReleaseSRWLockShared(&s_state_lock);
}

void update_shutdown(void) {
    AcquireSRWLockExclusive(&s_state_lock);
    clear_staged_locked();
    ReleaseSRWLockExclusive(&s_state_lock);
}

static int utf8_wide(const char *src, wchar_t *dst, int cap) {
    int n;
    if (!src || !dst || cap <= 0) return 0;
    n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, cap);
    return n > 0;
}

static int host_allowed(const wchar_t *host, const wchar_t *path, int manifest) {
    if (manifest) {
        return _wcsicmp(host, L"raw.githubusercontent.com") == 0 &&
               (wcscmp(path, L"/alex520lq/HopDesk/updates/stable.txt") == 0 ||
                wcscmp(path, L"/alex520lq/HopDesk/updates/dev.txt") == 0 ||
                wcscmp(path, L"/alex520lq/HopDesk/updates/latest.txt") == 0);
    }
    static const wchar_t prefix[] = L"/alex520lq/HopDesk/releases/download/v";
    return _wcsicmp(host, L"github.com") == 0 &&
           wcsncmp(path, prefix, wcslen(prefix)) == 0;
}

static HINTERNET http_open_get(const char *url, int manifest,
                               HINTERNET *session_out, HINTERNET *connect_out,
                               uint64_t *content_length_out) {
    wchar_t wide_url[2048], host[256], path[1536], extra[256], object[1800];
    URL_COMPONENTSW parts;
    HINTERNET session = NULL, connect = NULL, request = NULL;
    DWORD redirect_policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;

    if (content_length_out) *content_length_out = 0;
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

    session = WinHttpOpen(L"HopDesk updater/2.0",
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
    if (content_length_out) {
        wchar_t cl_buf[64] = {0};
        DWORD cl_size = sizeof(cl_buf);
        if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CONTENT_LENGTH,
                                WINHTTP_HEADER_NAME_BY_INDEX, cl_buf, &cl_size,
                                WINHTTP_NO_HEADER_INDEX)) {
            uint64_t parsed = _wcstoui64(cl_buf, NULL, 10);
            if (parsed <= UPDATE_MAX_DOWNLOAD) *content_length_out = parsed;
        }
    }

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

static int http_get_manifest_url(const char *url, char *out, size_t cap) {
    HINTERNET session = NULL, connect = NULL;
    HINTERNET request = http_open_get(url, 1, &session, &connect, NULL);
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

static int http_get_manifest(int channel, char *out, size_t cap) {
    const char *primary = hop_update_manifest_url(channel);
    if (http_get_manifest_url(primary, out, cap)) return 1;
    /* Backward compatibility: stable channel may fall back to legacy latest.txt. */
    if (channel == HOP_UPDATE_CHANNEL_STABLE)
        return http_get_manifest_url(HOP_UPDATE_MANIFEST_URL, out, cap);
    return 0;
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
                                const char *expected_hex, uint64_t *bytes_out) {
    HINTERNET session = NULL, connect = NULL;
    uint64_t content_len = 0;
    HINTERNET request = http_open_get(url, 0, &session, &connect, &content_len);
    HANDLE file = INVALID_HANDLE_VALUE;
    BCRYPT_ALG_HANDLE alg = NULL;
    BCRYPT_HASH_HANDLE hash = NULL;
    PUCHAR hash_object = NULL;
    BYTE digest[32], buffer[64 * 1024];
    ULONG object_len = 0, hash_len = 0, got = 0;
    ULONGLONG total = 0, last_reported = 0;
    char actual[65];
    int ok = 0;

    if (bytes_out) *bytes_out = 0;
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

    AcquireSRWLockExclusive(&s_state_lock);
    hop_update_snapshot_set_downloading(&s_snap, 0, content_len);
    ReleaseSRWLockExclusive(&s_state_lock);

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
            if (total - last_reported >= 65536ull || (content_len && total >= content_len)) {
                last_reported = total;
                AcquireSRWLockExclusive(&s_state_lock);
                hop_update_snapshot_set_downloading(&s_snap, total, content_len);
                ReleaseSRWLockExclusive(&s_state_lock);
            }
        }
    }
    if (!total || !BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0))) goto done;
    for (int i = 0; i < 32; ++i)
        snprintf(actual + i * 2, 3, "%02x", digest[i]);
    actual[64] = '\0';
    if (_stricmp(actual, expected_hex) != 0) goto done;
    if (!FlushFileBuffers(file)) goto done;
    if (bytes_out) *bytes_out = total;
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

static int apply_staged_installer(HWND owner, const wchar_t *archive, const char *version,
                                  char *err_out, size_t err_cap) {
    wchar_t temp[MAX_PATH], exe[MAX_PATH], helper[MAX_PATH], helper_run[MAX_PATH];
    wchar_t command[8192], target_dir[MAX_PATH], transaction[MAX_PATH], prepared[MAX_PATH], wversion[64];
    char nonce[33]; hop_session_id id;
    DWORD n;
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    wchar_t *slash;

    if (!archive || !archive[0] || GetFileAttributesW(archive) == INVALID_FILE_ATTRIBUTES) {
        snprintf(err_out, err_cap, "%s", "已下载的安装包已失效，请重新下载更新。");
        return 0;
    }

    n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp)) {
        snprintf(err_out, err_cap, "%s", "无法获取系统临时目录。");
        return 0;
    }

    n = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!n || n >= ARRAYSIZE(exe)) return 0;
    slash = wcsrchr(exe, L'\\');
    if (!slash) return 0;
    *slash = L'\0';
    wcscpy_s(target_dir, ARRAYSIZE(target_dir), exe);
    if (swprintf(helper, ARRAYSIZE(helper), L"%ls\\update-windows.ps1", target_dir) < 0 ||
        GetFileAttributesW(helper) == INVALID_FILE_ATTRIBUTES) {
        snprintf(err_out, err_cap, "%s", "找不到安全更新辅助脚本 update-windows.ps1，请重新安装当前版本。");
        return 0;
    }

    /* Close admission only at install time, after download and user confirmation.
     * Producer/receiver reservations make the busy check race-free. */
    if (!hop_update_install_begin(xfer_busy)) {
        snprintf(err_out, err_cap, "%s", "当前仍有文件传输任务，请等待传输完成后再点击重启安装。");
        return 0;
    }
    if (!hop_session_generate(&id) || !utf8_wide(version, wversion, ARRAYSIZE(wversion))) goto prepare_fail;
    hop_session_hex(&id, nonce);
    wchar_t wnonce[33];
    if (!utf8_wide(nonce, wnonce, ARRAYSIZE(wnonce)) ||
        swprintf(transaction, ARRAYSIZE(transaction), L"%lsHopDesk-update-%ls", temp, wnonce) < 0 ||
        !CreateDirectoryW(transaction, NULL)) goto prepare_fail;
    if (swprintf(helper_run, ARRAYSIZE(helper_run), L"%ls\\installer.ps1", transaction) < 0 ||
        swprintf(prepared, ARRAYSIZE(prepared), L"%ls\\prepared", transaction) < 0 ||
        !CopyFileW(helper, helper_run, TRUE)) {
        RemoveDirectoryW(transaction); goto prepare_fail;
    }
    /* powershell.exe is Windows PowerShell 5.1. Add a UTF-8 BOM to its temporary
     * script so non-ASCII diagnostics parse consistently on non-UTF8 systems. */
    {
        HANDLE f = CreateFileW(helper_run, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, 0, NULL);
        DWORD size = f == INVALID_HANDLE_VALUE ? 0 : GetFileSize(f, NULL), read = 0;
        BYTE *bytes = size && size < 1024 * 1024 ? (BYTE *)malloc(size) : NULL;
        int ok = bytes && ReadFile(f, bytes, size, &read, NULL) && read == size;
        if (ok) {
            int has_bom = size >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf;
            if (!has_bom) {
                SetFilePointer(f, 0, NULL, FILE_BEGIN);
                BYTE bom[3] = {0xef, 0xbb, 0xbf};
                ok = write_all(f, bom, 3) && write_all(f, bytes, size) && SetEndOfFile(f);
            }
        }
        free(bytes); if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        if (!ok) { DeleteFileW(helper_run); RemoveDirectoryW(transaction); goto prepare_fail; }
    }

    if (swprintf(command, ARRAYSIZE(command),
        L"powershell.exe -NoProfile -NonInteractive -ExecutionPolicy Bypass "
        L"-File \"%ls\" -Archive \"%ls\" -TargetDir \"%ls\" -ProcessId %lu -Transaction \"%ls\" -Nonce %hs -ExpectedVersion %ls",
        helper_run, archive, target_dir, GetCurrentProcessId(), transaction, nonce, wversion) < 0) {
        DeleteFileW(helper_run);
        hop_update_install_end();
        RemoveDirectoryW(transaction);
        snprintf(err_out, err_cap, "%s", "无法构造更新安装命令。");
        return 0;
    }
    memset(&si, 0, sizeof(si)); si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, command, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, target_dir, &si, &pi)) {
        DeleteFileW(helper_run);
        hop_update_install_end();
        RemoveDirectoryW(transaction);
        snprintf(err_out, err_cap, "%s", "无法启动更新辅助程序，旧版本未被修改。");
        return 0;
    }
    CloseHandle(pi.hThread);
    /* No user-facing modal exists after the helper starts its exit deadline.
     * Keep the old app alive unless preparation actually succeeded. */
    int prepared_ok = 0;
    for (int i = 0; i < 240; i++) {
        if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) break;
        HANDLE ack = CreateFileW(prepared, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
        if (ack != INVALID_HANDLE_VALUE) {
            char value[64] = {0}; DWORD got = 0;
            prepared_ok = ReadFile(ack, value, 63, &got, NULL) && !memcmp(value, nonce, 32) &&
                ((got == 33 && value[32] == '\n') || (got == 34 && value[32] == '\r' && value[33] == '\n'));
            CloseHandle(ack);
            if (prepared_ok) break;
        }
        Sleep(250);
    }
    if (!prepared_ok) {
        /* It cannot have replaced a live old executable: safe to cancel here. */
        TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        hop_update_install_end();
        DeleteFileW(helper_run); DeleteFileW(prepared); RemoveDirectoryW(transaction);
        snprintf(err_out, err_cap, "%s", "更新准备失败，当前版本继续运行。");
        return 0;
    }
    CloseHandle(pi.hProcess);
    AcquireSRWLockExclusive(&s_state_lock);
    s_applying_handoff = 1;
    ReleaseSRWLockExclusive(&s_state_lock);
    if (!PostMessageW(owner, WM_CLOSE, 0, 0)) {
        AcquireSRWLockExclusive(&s_state_lock);
        s_applying_handoff = 0;
        ReleaseSRWLockExclusive(&s_state_lock);
        hop_update_install_end();
        snprintf(err_out, err_cap, "%s", "无法通知主进程退出，当前版本继续运行。");
        return 0; /* helper times out safely without replacing the live app */
    }
    return 1; /* admission stays closed until this process exits */

prepare_fail:
    hop_update_install_end();
    snprintf(err_out, err_cap, "%s", "无法准备更新事务，当前版本继续运行。");
    return 0;
}

static void run_check(int target_channel) {
    char text[HOP_UPDATE_MANIFEST_MAX + 1];
    hop_update_manifest manifest;

    AcquireSRWLockExclusive(&s_state_lock);
    ensure_state_inited_locked();
    clear_staged_locked();
    s_has_manifest = 0;
    hop_update_snapshot_set_checking(&s_snap, target_channel);
    ReleaseSRWLockExclusive(&s_state_lock);

    if (!g_app.cfg.update_check_enabled) {
        AcquireSRWLockExclusive(&s_state_lock);
        hop_update_snapshot_set_error(&s_snap, target_channel,
            "已在配置中禁用更新检查（update_check_enabled=0）。");
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }

    if (!http_get_manifest(target_channel, text, sizeof(text)) ||
        !hop_update_manifest_parse(text, strlen(text), &manifest)) {
        AcquireSRWLockExclusive(&s_state_lock);
        hop_update_snapshot_set_error(&s_snap, target_channel,
            "无法读取有效的更新清单。请检查网络或确认该通道已有发布版本。");
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }

    AcquireSRWLockExclusive(&s_state_lock);
    s_manifest = manifest;
    s_has_manifest = 1;
    hop_update_snapshot_set_manifest_result(&s_snap, HOP_VERSION, current_build_channel(),
                                            HOP_CTRL_PROTO_VER, HOP_XFER_PROTO_VER,
                                            target_channel, &manifest);
    int result_phase = s_snap.phase;
    ReleaseSRWLockExclusive(&s_state_lock);
    hop_log(HOP_LOG_INFO, "update: checked channel=%s result_phase=%d target=v%s",
            hop_update_channel_name(target_channel), result_phase, manifest.version);
}

static void run_download(void) {
    hop_update_manifest manifest;
    wchar_t temp[MAX_PATH], archive[MAX_PATH];
    wchar_t *dot;
    DWORD n;
    uint64_t bytes = 0;

    AcquireSRWLockExclusive(&s_state_lock);
    ensure_state_inited_locked();
    if (!s_has_manifest || (s_snap.phase != HOP_UPDATE_AVAILABLE && s_snap.phase != HOP_UPDATE_ERROR)) {
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }
    clear_staged_locked();
    manifest = s_manifest;
    hop_update_snapshot_set_downloading(&s_snap, 0, 0);
    ReleaseSRWLockExclusive(&s_state_lock);

    n = GetTempPathW(ARRAYSIZE(temp), temp);
    if (!n || n >= ARRAYSIZE(temp) || !GetTempFileNameW(temp, L"hop", 0, archive)) {
        AcquireSRWLockExclusive(&s_state_lock);
        hop_update_snapshot_set_error(&s_snap, manifest.channel, "无法创建临时下载文件。");
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }
    DeleteFileW(archive);
    dot = wcsrchr(archive, L'.');
    if (dot) wcscpy_s(dot, 5, L".zip");
    DeleteFileW(archive);

    if (!http_download_sha256(manifest.windows_url, archive, manifest.windows_sha256, &bytes)) {
        AcquireSRWLockExclusive(&s_state_lock);
        hop_update_snapshot_set_error(&s_snap, manifest.channel,
            "更新包下载失败或 SHA-256 校验不匹配。旧版本未被修改。");
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }

    AcquireSRWLockExclusive(&s_state_lock);
    wcscpy_s(s_staged_archive, ARRAYSIZE(s_staged_archive), archive);
    s_staged_ready = 1;
    hop_update_snapshot_set_ready(&s_snap, bytes);
    ReleaseSRWLockExclusive(&s_state_lock);
    hop_log(HOP_LOG_INFO, "update: downloaded and verified v%s (%llu bytes)",
            manifest.version, (unsigned long long)bytes);
}

static void run_apply(HWND owner) {
    wchar_t archive[MAX_PATH];
    hop_update_manifest manifest;
    char err[256] = {0};

    AcquireSRWLockExclusive(&s_state_lock);
    ensure_state_inited_locked();
    if (!s_has_manifest || !s_staged_ready || !s_staged_archive[0]) {
        hop_update_snapshot_set_error(&s_snap, s_snap.target_channel,
            "尚未下载就绪的更新包，请先点击“下载更新”。");
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }
    wcscpy_s(archive, ARRAYSIZE(archive), s_staged_archive);
    manifest = s_manifest;
    hop_update_snapshot_set_applying(&s_snap);
    ReleaseSRWLockExclusive(&s_state_lock);

    if (!apply_staged_installer(owner, archive, manifest.version, err, sizeof(err))) {
        AcquireSRWLockExclusive(&s_state_lock);
        /* Keep staged archive valid if blocked by active transfer so user can retry apply. */
        if (GetFileAttributesW(archive) != INVALID_FILE_ATTRIBUTES) {
            s_snap.phase = HOP_UPDATE_READY;
            snprintf(s_snap.message, sizeof(s_snap.message), "%s", err[0] ? err : "安装准备失败，可稍后重试。");
        } else {
            clear_staged_locked();
            hop_update_snapshot_set_error(&s_snap, manifest.channel, err);
        }
        ReleaseSRWLockExclusive(&s_state_lock);
    }
}

static DWORD WINAPI update_worker(LPVOID arg) {
    win_update_req *req = (win_update_req *)arg;
    HWND owner = req ? req->owner : NULL;
    int action = req ? req->action : 0;
    int channel = req ? req->channel : HOP_UPDATE_CHANNEL_STABLE;
    free(req);

    switch (action) {
        case HOP_UPDATE_ACTION_CHECK:    run_check(channel); break;
        case HOP_UPDATE_ACTION_DOWNLOAD: run_download(); break;
        case HOP_UPDATE_ACTION_APPLY:    run_apply(owner); break;
        default: break;
    }

    InterlockedExchange(&s_update_busy, 0);
    return 0;
}

static void dispatch_update_action(HWND owner, int action, int channel) {
    win_update_req *req;
    HANDLE thread;
    if (InterlockedCompareExchange(&s_update_busy, 1, 0) != 0) return;
    req = (win_update_req *)malloc(sizeof(*req));
    if (!req) {
        InterlockedExchange(&s_update_busy, 0);
        return;
    }
    req->owner = owner;
    req->action = action;
    req->channel = channel == HOP_UPDATE_CHANNEL_DEV ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
    thread = CreateThread(NULL, 0, update_worker, req, 0, NULL);
    if (!thread) {
        free(req);
        InterlockedExchange(&s_update_busy, 0);
        AcquireSRWLockExclusive(&s_state_lock);
        ensure_state_inited_locked();
        hop_update_snapshot_set_error(&s_snap, channel, "无法启动后台更新线程。");
        ReleaseSRWLockExclusive(&s_state_lock);
        return;
    }
    CloseHandle(thread);
}

void update_check_async(HWND owner, int target_channel) {
    dispatch_update_action(owner, HOP_UPDATE_ACTION_CHECK, target_channel);
}

void update_download_async(HWND owner) {
    dispatch_update_action(owner, HOP_UPDATE_ACTION_DOWNLOAD, g_app.cfg.update_channel);
}

void update_apply_async(HWND owner) {
    dispatch_update_action(owner, HOP_UPDATE_ACTION_APPLY, g_app.cfg.update_channel);
}
