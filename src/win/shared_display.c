/* Shared-monitor switching: one worker owns native topology/DDC operations.
 * The UI thread owns input capture. Network replies carry transaction+epoch. */
#include "app.h"
#include "display_switch.h"
#include "../ui/hopdesk_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static LONG s_busy, s_stopping;
static HANDLE s_worker;
static SRWLOCK s_reply_lock = SRWLOCK_INIT;
static HANDLE s_reply_event;
static uint32_t s_expected, s_sequence;
static unsigned long s_epoch;
static int s_reply_ok;
static char s_status[192];
void shared_display_status(char *out, size_t cap) {
    AcquireSRWLockShared(&s_reply_lock);
    snprintf(out, cap, "%s", s_status);
    ReleaseSRWLockShared(&s_reply_lock);
}
static void display_status(const char *text) {
    AcquireSRWLockExclusive(&s_reply_lock);
    snprintf(s_status, sizeof(s_status), "%s", text);
    ReleaseSRWLockExclusive(&s_reply_lock);
}
static void notify_complete(net_server *net, unsigned long epoch, int target, int ok) {
    if (!net_server_has_client(net) || net_server_epoch(net) != epoch) return;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg m = {HOP_DISPLAY_COMPLETE, (uint8_t)ok, 0, (uint32_t)target};
    hop_display_encode(wire, &m);
    net_server_send(net, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
}

typedef struct {
    hop_config cfg;
    net_server *net;
    unsigned long epoch;
    HWND hwnd;
    int target;
    wchar_t device[32], state_path[1100];
} display_job;
typedef struct { DWORD version; wchar_t device[32]; DEVMODEW mode; } display_saved;

static int current_device(display_job *j, DISPLAY_DEVICEW *out) {
    for (DWORD i = 0; ; ++i) {
        DISPLAY_DEVICEW d = {0}; d.cb = sizeof(d);
        if (!EnumDisplayDevicesW(NULL, i, &d, 0)) return 0;
        if (!wcscmp(d.DeviceName, j->device)) {
            *out = d;
            return !(d.StateFlags & (DISPLAY_DEVICE_PRIMARY_DEVICE | DISPLAY_DEVICE_MIRRORING_DRIVER));
        }
    }
}
static int save_mode(display_job *j, DEVMODEW *mode) {
    display_saved s = {0}; s.version = 1; s.mode = *mode;
    wcscpy_s(s.device, 32, j->device);
    wchar_t temp[1120]; swprintf_s(temp, 1120, L"%ls.tmp", j->state_path);
    HANDLE f = CreateFileW(temp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    DWORD n = 0;
    int ok = WriteFile(f, &s, sizeof(s), &n, NULL) && n == sizeof(s) && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(temp, j->state_path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp);
    return ok;
}
static int load_mode(display_job *j, DEVMODEW *mode) {
    display_saved s = {0}; DWORD n = 0;
    HANDLE f = CreateFileW(j->state_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
    if (f == INVALID_HANDLE_VALUE) return 0;
    int ok = GetFileSize(f, NULL) == sizeof(s) && ReadFile(f, &s, sizeof(s), &n, NULL) && n == sizeof(s);
    CloseHandle(f);
    if (!ok || s.version != 1 || wcsncmp(s.device, j->device, 32) ||
        s.mode.dmSize != sizeof(DEVMODEW) || s.mode.dmDriverExtra ||
        s.mode.dmPelsWidth == 0 || s.mode.dmPelsHeight == 0) return 0;
    *mode = s.mode;
    return 1;
}
static int same_peer(display_job *j);
static int attach_display(void *ctx, int attached) {
    display_job *j = ctx;
    DISPLAY_DEVICEW d;
    if (!attached && !same_peer(j)) return 0;
    if (!current_device(j, &d)) return 0; /* primary/unknown target is never detached */
    int active = (d.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
    if (active == attached) return 1;
    DEVMODEW mode = {0}; mode.dmSize = sizeof(mode);
    if (attached) {
        if (!load_mode(j, &mode)) {
            hop_log(HOP_LOG_ERROR, "display: no saved mode for shared device; extend it in Windows once before setup");
            return 0;
        }
    } else {
        if (!EnumDisplaySettingsExW(j->device, ENUM_CURRENT_SETTINGS, &mode, 0) ||
            !save_mode(j, &mode)) return 0; /* persist recovery before changing topology */
        mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        mode.dmPelsWidth = mode.dmPelsHeight = 0;
    }
    LONG result = ChangeDisplaySettingsExW(j->device, &mode, NULL, CDS_UPDATEREGISTRY, NULL);
    hop_log(HOP_LOG_INFO, "display: shared device %s attach=%d native=%ld", j->cfg.disp_device, attached, result);
    if (result != DISP_CHANGE_SUCCESSFUL) return 0;
    DWORD start = GetTickCount();
    do {
        if (current_device(j, &d) && !!(d.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == attached) return 1;
        Sleep(200);
    } while ((DWORD)(GetTickCount() - start) < 6000);
    return 0;
}
static int same_peer(display_job *j) {
    return !InterlockedCompareExchange(&s_stopping, 0, 0) &&
        net_server_has_client(j->net) && net_server_epoch(j->net) == j->epoch;
}
static int peer_operation(display_job *j, int op, uint32_t value) {
    if (!same_peer(j)) return 0;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    AcquireSRWLockExclusive(&s_reply_lock);
    uint32_t id = ++s_sequence; if (!id) id = ++s_sequence;
    s_expected = id; s_epoch = j->epoch; s_reply_ok = 0;
    ResetEvent(s_reply_event);
    ReleaseSRWLockExclusive(&s_reply_lock);
    hop_display_msg m = {(uint8_t)op, 0, id, value};
    hop_display_encode(wire, &m);
    int sent = net_server_send(j->net, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
    DWORD start = GetTickCount();
    while (sent && same_peer(j) && (DWORD)(GetTickCount() - start) < 10000) {
        if (WaitForSingleObject(s_reply_event, 100) == WAIT_OBJECT_0) break;
    }
    AcquireSRWLockExclusive(&s_reply_lock);
    int ok = sent && s_reply_ok && same_peer(j);
    s_expected = 0;
    ReleaseSRWLockExclusive(&s_reply_lock);
    return ok;
}
static int wake_peer(void *ctx) {
    display_job *j = ctx;
    if (!peer_operation(j, HOP_DISPLAY_WAKE, (uint32_t)j->cfg.disp_wake_sec)) return 0;
    Sleep((DWORD)j->cfg.disp_wake_delay_ms);
    return same_peer(j);
}
static uint16_t input_code(display_job *j, int target) {
    return target == HOP_DISPLAY_MAC ? j->cfg.disp_mac_input : j->cfg.disp_win_input;
}
static int local_input(void *ctx, int target) {
    display_job *j = ctx;
    if (target == HOP_DISPLAY_MAC && !same_peer(j)) return 0;
    return disp_input_verified(j->device, input_code(j, target)) &&
        (target != HOP_DISPLAY_MAC || same_peer(j));
}
static int peer_input(void *ctx, int target) {
    display_job *j = ctx;
    if (!peer_operation(j, HOP_DISPLAY_INPUT, input_code(j, target))) return 0;
    DWORD start = GetTickCount();
    do {
        if (disp_input_matches(j->device, input_code(j, target))) return 1;
        Sleep(200);
    } while (same_peer(j) && (DWORD)(GetTickCount() - start) < 6000);
    return 0;
}
static DWORD WINAPI switch_worker(LPVOID arg) {
    display_job *j = arg;
    hop_display_ops ops = {j, wake_peer, local_input, peer_input, attach_display};
    int guard = hop_watchdog_begin("shared display switch", 60000);
    int result = hop_display_switch_run(j->target, &ops);
    hop_watchdog_end(guard);
    hop_log(result == HOP_DISPLAY_OK ? HOP_LOG_INFO : HOP_LOG_ERROR,
        "display: shared switch target=%s result=%d (0=verified; 5=rollback failed, use Windows display settings/monitor OSD)",
        j->target == HOP_DISPLAY_MAC ? "mac" : "windows", result);
    display_status(result == HOP_DISPLAY_OK ?
        (j->target == HOP_DISPLAY_MAC ? "已验证：共用屏已交给 Mac，Windows 已释放扩展屏" : "已验证：Windows 已恢复扩展屏和输入源") :
        "共屏切换失败，请检查运行日志、Windows 显示设置与显示器输入源");
    notify_complete(j->net, j->epoch, j->target, result == HOP_DISPLAY_OK);
    PostMessageW(j->hwnd, WM_APP_DISPLAY_DONE, (WPARAM)j->epoch, (LPARAM)result);
    free(j);
    InterlockedExchange(&s_busy, 0);
    return 0;
}
int shared_display_busy(void) { return InterlockedCompareExchange(&s_busy, 0, 0) != 0; }
int shared_display_start(int target) {
    if (disp_input_busy()) {
        display_status("已有输入源切换正在执行，请稍后重试共屏切换");
        notify_complete(g_app.net, net_server_epoch(g_app.net), target, 0);
        return 0;
    }
    if (InterlockedCompareExchange(&s_busy, 1, 0)) {
        hop_log(HOP_LOG_WARN, "display: switch already in progress"); return 0;
    }
    if (s_worker) { WaitForSingleObject(s_worker, INFINITE); CloseHandle(s_worker); s_worker = NULL; }
    display_job *j = calloc(1, sizeof(*j));
    if (!j) goto fail;
    j->cfg = g_app.cfg; j->net = g_app.net; j->hwnd = g_app.hwnd;
    j->target = target; j->epoch = net_server_epoch(j->net);
    char state[1100]; snprintf(state, sizeof(state), "%s.display", g_app.config_path);
    if (!j->cfg.disp_device[0] ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, j->cfg.disp_device, -1, j->device, 32) ||
        !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, state, -1, j->state_path, 1100)) goto fail_job;
    DISPLAY_DEVICEW d;
    if (!current_device(j, &d)) goto fail_job;
    if (!s_reply_event) s_reply_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!s_reply_event) goto fail_job;
    display_status("正在切换共用显示器，请稍候…");
    KillTimer(g_app.hwnd, IDT_DISP);
    capture_set_sharing(0);
    capture_topology_changed();
    s_worker = CreateThread(NULL, 0, switch_worker, j, 0, NULL);
    if (s_worker) return 1;
fail_job:
    free(j);
fail:
    InterlockedExchange(&s_busy, 0);
    display_status("无法切换：请在设置中指定有效的 Windows 共用扩展屏设备名");
    notify_complete(g_app.net, net_server_epoch(g_app.net), target, 0);
    hop_log(HOP_LOG_ERROR, "display: configure a valid secondary disp_device before shared switching");
    return 0;
}
void shared_display_message(const uint8_t *p, uint32_t len) {
    hop_display_msg m;
    if (!hop_display_decode(&m, p, len)) return;
    if (m.op == HOP_DISPLAY_REQUEST) {
        if (shared_display_busy()) {
            notify_complete(g_app.net, net_server_epoch(g_app.net), (int)m.value, 0);
            return;
        }
        if (!g_app.cfg.disp_shared) {
            uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
            hop_display_msg local = {HOP_DISPLAY_LOCAL, 0, 0, m.value};
            hop_display_encode(wire, &local);
            net_server_send(g_app.net, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
            return; /* Ordinary mode preserves the Mac's local DDC target. */
        }
        win_ui_display_action(m.value == HOP_DISPLAY_MAC ? HOPDESK_UI_DISPLAY_TO_MAC : HOPDESK_UI_DISPLAY_TO_WINDOWS);
        return;
    }
    if (m.op != HOP_DISPLAY_RESULT) return;
    AcquireSRWLockExclusive(&s_reply_lock);
    if (s_expected && m.id == s_expected && net_server_epoch(g_app.net) == s_epoch) {
        s_reply_ok = m.ok; SetEvent(s_reply_event);
    }
    ReleaseSRWLockExclusive(&s_reply_lock);
}
void shared_display_shutdown(void) {
    InterlockedExchange(&s_stopping, 1);
    AcquireSRWLockExclusive(&s_reply_lock);
    s_reply_ok = 0; s_expected = 0;
    if (s_reply_event) SetEvent(s_reply_event);
    ReleaseSRWLockExclusive(&s_reply_lock);
    if (s_worker) { WaitForSingleObject(s_worker, INFINITE); CloseHandle(s_worker); s_worker = NULL; }
    if (s_reply_event) { CloseHandle(s_reply_event); s_reply_event = NULL; }
}

void shared_display_devices(char *out, size_t cap) {
    size_t used = 0;
    if (!cap) return;
    out[0] = 0;
    for (DWORD i = 0; used + 1 < cap; ++i) {
        DISPLAY_DEVICEW d = {0}; d.cb = sizeof(d);
        if (!EnumDisplayDevicesW(NULL, i, &d, 0)) break;
        if (d.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;
        char name[64];
        if (!WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, name, sizeof(name), NULL, NULL)) continue;
        DEVMODEW mode = {0}; mode.dmSize = sizeof(mode);
        int active = EnumDisplaySettingsExW(d.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0) && mode.dmPelsWidth > 0;
        int n = snprintf(out + used, cap - used, "%s%s · %s · (%ld,%ld)\n", name,
            (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) ? " [主屏]" : "",
            active ? "已扩展" : "未启用", mode.dmPosition.x, mode.dmPosition.y);
        if (n < 0 || (size_t)n >= cap - used) break;
        used += (size_t)n;
    }
}
