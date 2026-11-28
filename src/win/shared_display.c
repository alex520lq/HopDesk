/* Shared-monitor switching: one worker owns native topology/DDC operations.
 * The UI thread owns input capture. Network sends and replies bind transaction+epoch. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "app.h"
#include "display_switch.h"
#include "../ui/hopdesk_ui.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static LONG s_busy, s_stopping;
static HANDLE s_worker, s_probe_thread, s_identify_thread;
static SRWLOCK s_reply_lock = SRWLOCK_INIT;
static HANDLE s_reply_event;
static uint32_t s_expected, s_sequence;
static unsigned long s_epoch;
static int s_reply_ok;
static char s_status[192];

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
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
    if (!net || !epoch) return;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg m = {HOP_DISPLAY_COMPLETE, (uint8_t)(ok ? 1 : 0), 0, (uint32_t)target};
    if (!hop_display_encode(wire, &m)) return;
    (void)net_server_send_epoch(net, epoch, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
}

typedef struct {
    hop_config cfg;
    net_server *net;
    unsigned long epoch;
    HWND hwnd;
    int requested, target, missing_mode;
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
static int device_is_attached(display_job *j) {
    DISPLAY_DEVICEW d;
    return current_device(j, &d) && (d.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
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
static int same_peer(display_job *j) {
    return !InterlockedCompareExchange(&s_stopping, 0, 0) &&
        net_server_has_client(j->net) && net_server_epoch(j->net) == j->epoch;
}
static int sleep_interruptible(display_job *j, int require_peer, DWORD ms) {
    DWORD start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < ms) {
        if (InterlockedCompareExchange(&s_stopping, 0, 0)) return 0;
        if (require_peer && !same_peer(j)) return 0;
        DWORD left = ms - (DWORD)(GetTickCount() - start);
        Sleep(left < 50 ? left : 50);
    }
    return !InterlockedCompareExchange(&s_stopping, 0, 0) && (!require_peer || same_peer(j));
}
static int attach_display(void *ctx, int attached) {
    display_job *j = ctx;
    DISPLAY_DEVICEW d;
    if (!attached && !same_peer(j)) return 0;
    if (!current_device(j, &d)) return 0; /* primary/unknown target is never detached */
    int active = (d.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
    if (active == attached) return 1;
    DEVMODEW mode = {0}; mode.dmSize = sizeof(mode);
    if (attached) {
        j->missing_mode = 0;
        if (!load_mode(j, &mode)) {
            if (!EnumDisplaySettingsExW(j->device, ENUM_REGISTRY_SETTINGS, &mode, 0) ||
                mode.dmPelsWidth == 0 || mode.dmPelsHeight == 0) {
                j->missing_mode = 1;
                hop_log(HOP_LOG_ERROR, "display: no saved or registry mode for shared device; extend it in Windows once before setup");
                return 0;
            }
            hop_log(HOP_LOG_WARN, "display: restored %s from Windows registry mode (%lux%lu)",
                j->cfg.disp_device, (unsigned long)mode.dmPelsWidth, (unsigned long)mode.dmPelsHeight);
        }
    } else {
        if (!EnumDisplaySettingsExW(j->device, ENUM_CURRENT_SETTINGS, &mode, 0) ||
            !save_mode(j, &mode)) return 0; /* persist recovery before changing topology */
        mode.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
        mode.dmPelsWidth = mode.dmPelsHeight = 0;
    }
    LONG result = ChangeDisplaySettingsExW(j->device, &mode, NULL, CDS_UPDATEREGISTRY | CDS_NORESET, NULL);
    if (result == DISP_CHANGE_SUCCESSFUL)
        result = ChangeDisplaySettingsExW(NULL, NULL, NULL, 0, NULL);
    hop_log(HOP_LOG_INFO, "display: shared device %s attach=%d native=%ld", j->cfg.disp_device, attached, result);
    if (result != DISP_CHANGE_SUCCESSFUL) return 0;
    DWORD timeout = (DWORD)clamp_int(j->cfg.disp_verify_timeout_ms, 1000, 30000);
    DWORD poll = (DWORD)clamp_int(j->cfg.disp_poll_ms, 50, 2000);
    DWORD start = GetTickCount();
    do {
        if (current_device(j, &d) && !!(d.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == attached) return 1;
        if (!sleep_interruptible(j, 0, poll)) return 0;
    } while ((DWORD)(GetTickCount() - start) < timeout);
    return 0;
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
    if (!hop_display_encode(wire, &m)) {
        AcquireSRWLockExclusive(&s_reply_lock); s_expected = 0; ReleaseSRWLockExclusive(&s_reply_lock);
        return 0;
    }
    int sent = net_server_send_epoch(j->net, j->epoch, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
    DWORD timeout = (DWORD)clamp_int(j->cfg.disp_wake_timeout_ms, 1000, 30000);
    DWORD start = GetTickCount();
    while (sent && same_peer(j) && (DWORD)(GetTickCount() - start) < timeout) {
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
    if (!peer_operation(j, HOP_DISPLAY_WAKE, (uint32_t)clamp_int(j->cfg.disp_wake_sec, 1, 3600))) return 0;
    DWORD settle = (DWORD)clamp_int(j->cfg.disp_wake_settle_ms, 0, 10000);
    return sleep_interruptible(j, 1, settle);
}
static void clear_wake(void *ctx) {
    display_job *j = ctx;
    if (!same_peer(j)) return;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg m = {HOP_DISPLAY_WAKE_CLEAR, 0, 0, 0};
    if (hop_display_encode(wire, &m))
        (void)net_server_send_epoch(j->net, j->epoch, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
}
static uint16_t input_code(display_job *j, int target) {
    return target == HOP_DISPLAY_MAC ? j->cfg.disp_mac_input : j->cfg.disp_win_input;
}
static int local_input(void *ctx, int target) {
    display_job *j = ctx;
    if (target == HOP_DISPLAY_MAC && !same_peer(j)) return 0;
    return disp_input_verified(j->device, input_code(j, target),
        j->cfg.disp_ddc_retries, j->cfg.disp_verify_timeout_ms, j->cfg.disp_poll_ms) &&
        (target != HOP_DISPLAY_MAC || same_peer(j));
}
static int wait_ddc_match(display_job *j, uint16_t want, int require_peer) {
    DWORD timeout = (DWORD)clamp_int(j->cfg.disp_verify_timeout_ms, 1000, 30000);
    DWORD poll = (DWORD)clamp_int(j->cfg.disp_poll_ms, 50, 2000);
    DWORD start = GetTickCount();
    do {
        if (disp_input_matches(j->device, want)) return 1;
        if (!sleep_interruptible(j, require_peer, poll)) return 0;
    } while ((DWORD)(GetTickCount() - start) < timeout);
    return 0;
}
static int peer_input(void *ctx, int target) {
    display_job *j = ctx;
    if (target == HOP_DISPLAY_WINDOWS && j->missing_mode) return 0;
    if (!peer_operation(j, HOP_DISPLAY_INPUT, input_code(j, target))) return 0;
    if (target == HOP_DISPLAY_WINDOWS && !device_is_attached(j)) return 1;
    return wait_ddc_match(j, input_code(j, target), target == HOP_DISPLAY_MAC);
}
static int peer_sleep(void *ctx) {
    display_job *j = ctx;
    if (j->missing_mode) return 0;
    hop_log(HOP_LOG_WARN, "display: trying configured macOS sleep fallback to reclaim Windows screen");
    if (!peer_operation(j, HOP_DISPLAY_SLEEP, 0)) return 0;
    if (!device_is_attached(j)) return 1;
    return wait_ddc_match(j, input_code(j, HOP_DISPLAY_WINDOWS), 0);
}
static DWORD WINAPI switch_worker(LPVOID arg) {
    display_job *j = arg;
    j->target = j->requested;
    if (j->requested == HOP_DISPLAY_AUTO) {
        int attached = device_is_attached(j);
        int cur_input = -1;
        if (attached) {
            uint16_t v = 0;
            int tries = clamp_int(j->cfg.disp_ddc_retries, 1, 10);
            for (int i = 0; i < tries; ++i) {
                if (disp_input_read(j->device, &v)) { cur_input = (int)v; break; }
                if (i + 1 < tries && !sleep_interruptible(j, 0, 200)) break;
            }
        }
        j->target = hop_display_resolve_target(HOP_DISPLAY_AUTO, attached, cur_input, j->cfg.disp_mac_input);
        hop_log(HOP_LOG_INFO, "display: auto resolved device=%s attached=%d input=%d -> target=%s",
            j->cfg.disp_device, attached, cur_input, j->target == HOP_DISPLAY_MAC ? "mac" : "windows");
    }
    hop_display_ops ops = {
        j,
        wake_peer,
        clear_wake,
        local_input,
        peer_input,
        j->cfg.disp_sleep_fallback ? peer_sleep : NULL,
        attach_display
    };
    int guard = hop_watchdog_begin("shared display switch", 60000);
    int result = hop_display_switch_run(j->target, &ops);
    hop_watchdog_end(guard);
    hop_log(result == HOP_DISPLAY_OK ? HOP_LOG_INFO : HOP_LOG_ERROR,
        "display: shared switch target=%s result=%d (0=verified; 5=rollback failed, use Windows display settings/monitor OSD)",
        j->target == HOP_DISPLAY_MAC ? "mac" : "windows", result);
    display_status(result == HOP_DISPLAY_OK ?
        (j->target == HOP_DISPLAY_MAC ? "已验证：共用屏已交给 Mac，Windows 已释放扩展屏" : "已验证：Windows 已恢复扩展屏和输入源") :
        (j->missing_mode ? "未找到共用屏原模式存档：请先在 Windows 按 Win+P 选择「扩展」一次后再试" :
         "共屏切换失败，请检查运行日志、Windows 显示设置与显示器输入源"));
    notify_complete(j->net, j->epoch, j->target, result == HOP_DISPLAY_OK);
    PostMessageW(j->hwnd, WM_APP_DISPLAY_DONE, (WPARAM)j->epoch, (LPARAM)result);
    free(j);
    InterlockedExchange(&s_busy, 0);
    return 0;
}
int shared_display_busy(void) { return InterlockedCompareExchange(&s_busy, 0, 0) != 0; }
int shared_display_start(int target) {
    if (target != HOP_DISPLAY_WINDOWS && target != HOP_DISPLAY_MAC && target != HOP_DISPLAY_AUTO) return 0;
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
    j->requested = target; j->target = target; j->epoch = net_server_epoch(j->net);
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
    unsigned long epoch = net_server_epoch(g_app.net);
    if (m.op == HOP_DISPLAY_REQUEST) {
        if (shared_display_busy()) {
            notify_complete(g_app.net, epoch, (int)m.value, 0);
            return;
        }
        if (!g_app.cfg.disp_shared) {
            if (m.value == HOP_DISPLAY_AUTO) {
                notify_complete(g_app.net, epoch, HOP_DISPLAY_AUTO, 0);
                return;
            }
            uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
            hop_display_msg local = {HOP_DISPLAY_LOCAL, 0, 0, m.value};
            hop_display_encode(wire, &local);
            (void)net_server_send_epoch(g_app.net, epoch, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
            return; /* Ordinary mode preserves the Mac's local DDC target. */
        }
        int action = m.value == HOP_DISPLAY_MAC ? HOPDESK_UI_DISPLAY_TO_MAC :
                     m.value == HOP_DISPLAY_AUTO ? HOPDESK_UI_DISPLAY_TOGGLE :
                     HOPDESK_UI_DISPLAY_TO_WINDOWS;
        win_ui_display_action(action);
        return;
    }
    if (m.op != HOP_DISPLAY_RESULT) return;
    AcquireSRWLockExclusive(&s_reply_lock);
    if (s_expected && m.id == s_expected && epoch == s_epoch) {
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
    if (s_probe_thread) { WaitForSingleObject(s_probe_thread, 2000); CloseHandle(s_probe_thread); s_probe_thread = NULL; }
    if (s_identify_thread) { WaitForSingleObject(s_identify_thread, 1000); CloseHandle(s_identify_thread); s_identify_thread = NULL; }
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

/* ---- Friendly monitor enumeration, async DDC probe, and screen identify HUD ---- */
static SRWLOCK s_inv_lock = SRWLOCK_INIT;
static hop_display_inventory s_inv;
static int s_inv_loaded;
static LONG s_probe_busy;
static LONG s_identify_busy;
static char s_probe_saved_dev[32];

static void query_friendly_monitor_name(const wchar_t *gdi_device, char *out, size_t cap, int *is_builtin) {
    UINT32 path_count = 0, mode_count = 0;
    DISPLAYCONFIG_PATH_INFO *paths = NULL;
    DISPLAYCONFIG_MODE_INFO *modes = NULL;
    int found = 0;

    if (out && cap) out[0] = '\0';
    if (is_builtin) *is_builtin = 0;
    if (!gdi_device || !out || !cap) return;

    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &path_count, &mode_count) == ERROR_SUCCESS &&
        path_count > 0 && mode_count > 0) {
        paths = (DISPLAYCONFIG_PATH_INFO *)calloc(path_count, sizeof(*paths));
        modes = (DISPLAYCONFIG_MODE_INFO *)calloc(mode_count, sizeof(*modes));
        if (paths && modes &&
            QueryDisplayConfig(QDC_ALL_PATHS, &path_count, paths, &mode_count, modes, NULL) == ERROR_SUCCESS) {
            for (int pass = 0; pass < 2 && !found; ++pass) {
                for (UINT32 i = 0; i < path_count; ++i) {
                    if (pass == 0 && !(paths[i].flags & DISPLAYCONFIG_PATH_ACTIVE)) continue;
                    DISPLAYCONFIG_SOURCE_DEVICE_NAME src_name;
                    memset(&src_name, 0, sizeof(src_name));
                    src_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
                    src_name.header.size = sizeof(src_name);
                    src_name.header.adapterId = paths[i].sourceInfo.adapterId;
                    src_name.header.id = paths[i].sourceInfo.id;
                    if (DisplayConfigGetDeviceInfo(&src_name.header) != ERROR_SUCCESS) continue;
                    if (wcscmp(src_name.viewGdiDeviceName, gdi_device) != 0) continue;

                    DISPLAYCONFIG_TARGET_DEVICE_NAME tgt_name;
                    memset(&tgt_name, 0, sizeof(tgt_name));
                    tgt_name.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
                    tgt_name.header.size = sizeof(tgt_name);
                    tgt_name.header.adapterId = paths[i].targetInfo.adapterId;
                    tgt_name.header.id = paths[i].targetInfo.id;
                    if (DisplayConfigGetDeviceInfo(&tgt_name.header) != ERROR_SUCCESS) continue;

                    if (is_builtin &&
                        (tgt_name.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL ||
                         tgt_name.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_DISPLAYPORT_EMBEDDED ||
                         tgt_name.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_UDI_EMBEDDED ||
                         tgt_name.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_LVDS)) {
                        *is_builtin = 1;
                    }
                    if (tgt_name.monitorFriendlyDeviceName[0]) {
                        if (WideCharToMultiByte(CP_UTF8, 0, tgt_name.monitorFriendlyDeviceName, -1,
                                                out, (int)cap, NULL, NULL) > 0) {
                            found = 1;
                            break;
                        }
                    }
                }
            }
        }
        free(paths);
        free(modes);
    }

    if (!found) {
        DISPLAY_DEVICEW mon = {0};
        mon.cb = sizeof(mon);
        if (EnumDisplayDevicesW(gdi_device, 0, &mon, 0) && mon.DeviceString[0]) {
            (void)WideCharToMultiByte(CP_UTF8, 0, mon.DeviceString, -1, out, (int)cap, NULL, NULL);
        }
    }
}

static void enumerate_inventory(hop_display_inventory *dst, int probe_ddc,
                                const hop_display_inventory *prev, const char *saved_dev) {
    memset(dst, 0, sizeof(*dst));
    dst->ddc_tool_ready = 1;
    int idx = 0;
    for (DWORD i = 0; dst->count < HOP_DISPLAY_MAX_DEVICES; ++i) {
        DISPLAY_DEVICEW d = {0};
        d.cb = sizeof(d);
        if (!EnumDisplayDevicesW(NULL, i, &d, 0)) break;
        if (d.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;

        char dev_utf8[32] = {0};
        if (!WideCharToMultiByte(CP_UTF8, 0, d.DeviceName, -1, dev_utf8, sizeof(dev_utf8), NULL, NULL))
            continue;

        DEVMODEW mode = {0};
        mode.dmSize = sizeof(mode);
        int active = EnumDisplaySettingsExW(d.DeviceName, ENUM_CURRENT_SETTINGS, &mode, 0) &&
                     mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0;

        /* Skip completely phantom/unplugged adapter slots that have neither active mode nor registry mode. */
        if (!active) {
            DEVMODEW reg_mode = {0};
            reg_mode.dmSize = sizeof(reg_mode);
            DISPLAY_DEVICEW mon = {0};
            mon.cb = sizeof(mon);
            int has_mon = EnumDisplayDevicesW(d.DeviceName, 0, &mon, 0) != 0;
            int has_reg = EnumDisplaySettingsExW(d.DeviceName, ENUM_REGISTRY_SETTINGS, &reg_mode, 0) &&
                          reg_mode.dmPelsWidth > 0;
            if (!has_mon && !has_reg && (!saved_dev || !saved_dev[0] || strcmp(dev_utf8, saved_dev) != 0))
                continue;
            if (has_reg) mode = reg_mode;
        }

        hop_display_info *item = &dst->items[dst->count];
        item->index = ++idx;
        item->is_primary = (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
        item->attached = active;
        item->width = (int32_t)mode.dmPelsWidth;
        item->height = (int32_t)mode.dmPelsHeight;
        item->pos_x = (int32_t)mode.dmPosition.x;
        item->pos_y = (int32_t)mode.dmPosition.y;
        item->current_input = -1;
        snprintf(item->device_id, sizeof(item->device_id), "%s", dev_utf8);
        int builtin = 0;
        query_friendly_monitor_name(d.DeviceName, item->friendly_name, sizeof(item->friendly_name), &builtin);
        item->is_builtin = builtin;

        if (probe_ddc && active) {
            uint16_t vcp = 0;
            if (disp_input_read(d.DeviceName, &vcp))
                item->current_input = (int32_t)vcp;
        } else if (prev) {
            for (int k = 0; k < prev->count; ++k) {
                if (strcmp(prev->items[k].device_id, item->device_id) == 0) {
                    item->current_input = prev->items[k].current_input;
                    break;
                }
            }
        }
        dst->count++;
    }
}

void shared_display_get_inventory(hop_display_inventory *out) {
    if (!out) return;
    AcquireSRWLockExclusive(&s_inv_lock);
    if (!s_inv_loaded && !InterlockedCompareExchange(&s_probe_busy, 0, 0)) {
        hop_display_inventory fresh;
        char saved_dev[32] = {0};
        snprintf(saved_dev, sizeof(saved_dev), "%s", g_app.cfg.disp_device);
        enumerate_inventory(&fresh, 0, NULL, saved_dev);
        s_inv = fresh;
        s_inv_loaded = 1;
    }
    *out = s_inv;
    ReleaseSRWLockExclusive(&s_inv_lock);
}

static DWORD WINAPI display_probe_worker(LPVOID arg) {
    (void)arg;
    hop_display_inventory fresh;
    char saved_dev[32] = {0};
    int read_ok = 0;

    AcquireSRWLockExclusive(&s_inv_lock);
    s_inv.probing = 1;
    snprintf(saved_dev, sizeof(saved_dev), "%s", s_probe_saved_dev);
    snprintf(s_inv.probe_note, sizeof(s_inv.probe_note), "%s", "正在识别显示器型号并读取 DDC 输入源…");
    ReleaseSRWLockExclusive(&s_inv_lock);

    enumerate_inventory(&fresh, 1, NULL, saved_dev);
    for (int i = 0; i < fresh.count; ++i) {
        if (fresh.items[i].current_input >= 0) ++read_ok;
    }
    fresh.probing = 0;
    if (fresh.count == 0) {
        snprintf(fresh.probe_note, sizeof(fresh.probe_note), "%s", "未检测到显示器，请检查 Windows 显示设置。");
    } else if (read_ok > 0) {
        snprintf(fresh.probe_note, sizeof(fresh.probe_note),
                 "已识别 %d 台显示器，其中 %d 台成功读取当前 DDC 输入源。", fresh.count, read_ok);
    } else {
        snprintf(fresh.probe_note, sizeof(fresh.probe_note),
                 "已识别 %d 台显示器；未能读回 DDC 输入源（请确认显示器 OSD 已开启 DDC/CI）。", fresh.count);
    }

    AcquireSRWLockExclusive(&s_inv_lock);
    s_inv = fresh;
    s_inv_loaded = 1;
    ReleaseSRWLockExclusive(&s_inv_lock);

    InterlockedExchange(&s_probe_busy, 0);
    return 0;
}

int shared_display_probe_async(void) {
    if (InterlockedCompareExchange(&s_stopping, 0, 0)) return 0;
    if (InterlockedCompareExchange(&s_probe_busy, 1, 0) != 0) return 1;
    AcquireSRWLockExclusive(&s_inv_lock);
    snprintf(s_probe_saved_dev, sizeof(s_probe_saved_dev), "%s", g_app.cfg.disp_device);
    ReleaseSRWLockExclusive(&s_inv_lock);
    if (s_probe_thread) { CloseHandle(s_probe_thread); s_probe_thread = NULL; }
    HANDLE t = CreateThread(NULL, 0, display_probe_worker, NULL, 0, NULL);
    if (!t) {
        InterlockedExchange(&s_probe_busy, 0);
        return 0;
    }
    s_probe_thread = t;
    return 1;
}

typedef struct {
    int  index;
    int  is_primary;
    RECT rc;
    wchar_t name[96];
} win_identify_item;

typedef struct {
    hop_display_inventory inv;
    win_identify_item items[HOP_DISPLAY_MAX_DEVICES];
    int count;
} win_identify_ctx;

static BOOL CALLBACK identify_enum_proc(HMONITOR mon, HDC dc, LPRECT rc, LPARAM arg) {
    (void)dc; (void)rc;
    win_identify_ctx *ctx = (win_identify_ctx *)arg;
    if (!ctx || ctx->count >= HOP_DISPLAY_MAX_DEVICES) return FALSE;
    MONITORINFOEXW mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, (MONITORINFO *)&mi)) return TRUE;

    char dev_utf8[32] = {0};
    (void)WideCharToMultiByte(CP_UTF8, 0, mi.szDevice, -1, dev_utf8, sizeof(dev_utf8), NULL, NULL);

    win_identify_item *it = &ctx->items[ctx->count];
    memset(it, 0, sizeof(*it));
    it->rc = mi.rcMonitor;
    it->index = ctx->count + 1;
    it->is_primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;
    wcscpy_s(it->name, ARRAYSIZE(it->name), L"显示器");

    for (int i = 0; i < ctx->inv.count; ++i) {
        if (strcmp(ctx->inv.items[i].device_id, dev_utf8) == 0) {
            it->index = ctx->inv.items[i].index;
            it->is_primary = ctx->inv.items[i].is_primary;
            if (ctx->inv.items[i].friendly_name[0]) {
                (void)MultiByteToWideChar(CP_UTF8, 0, ctx->inv.items[i].friendly_name, -1,
                                          it->name, (int)ARRAYSIZE(it->name));
            }
            break;
        }
    }
    ctx->count++;
    return TRUE;
}

static LRESULT CALLBACK identify_wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc;
        GetClientRect(hwnd, &rc);
        win_identify_item *it = (win_identify_item *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

        HBRUSH bg = CreateSolidBrush(RGB(22, 26, 34));
        FillRect(hdc, &rc, bg);
        DeleteObject(bg);

        HPEN pen = CreatePen(PS_SOLID, 3, RGB(56, 139, 253));
        HGDIOBJ old_pen = SelectObject(hdc, pen);
        HGDIOBJ old_brush = SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, 2, 2, rc.right - 2, rc.bottom - 2);
        SelectObject(hdc, old_brush);
        SelectObject(hdc, old_pen);
        DeleteObject(pen);

        if (it) {
            SetBkMode(hdc, TRANSPARENT);
            HFONT big_font = CreateFontW(76, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
            HFONT sub_font = CreateFontW(22, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
                DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                DEFAULT_PITCH | FF_SWISS, L"Microsoft YaHei UI");
            wchar_t num_buf[16], sub_buf[140];
            swprintf_s(num_buf, ARRAYSIZE(num_buf), L"%d", it->index);
            swprintf_s(sub_buf, ARRAYSIZE(sub_buf), L"%ls · %ls",
                       it->name, it->is_primary ? L"主屏" : L"扩展屏");

            RECT top_rc = { 12, 16, rc.right - 12, 108 };
            RECT bot_rc = { 12, 112, rc.right - 12, rc.bottom - 16 };
            HGDIOBJ old_font = SelectObject(hdc, big_font);
            SetTextColor(hdc, RGB(255, 255, 255));
            DrawTextW(hdc, num_buf, -1, &top_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(hdc, sub_font);
            SetTextColor(hdc, RGB(195, 210, 232));
            DrawTextW(hdc, sub_buf, -1, &bot_rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            SelectObject(hdc, old_font);
            DeleteObject(big_font);
            DeleteObject(sub_font);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI display_identify_worker(LPVOID arg) {
    (void)arg;
    win_identify_ctx ctx;
    memset(&ctx, 0, sizeof(ctx));
    shared_display_get_inventory(&ctx.inv);
    EnumDisplayMonitors(NULL, NULL, identify_enum_proc, (LPARAM)&ctx);
    if (ctx.count <= 0) {
        InterlockedExchange(&s_identify_busy, 0);
        return 0;
    }

    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = identify_wnd_proc;
    wc.hInstance = g_app.inst;
    wc.lpszClassName = L"HopDeskDisplayIdentify";
    RegisterClassW(&wc);

    HWND hwnds[HOP_DISPLAY_MAX_DEVICES] = {0};
    const int w = 360, h = 176;
    for (int i = 0; i < ctx.count; ++i) {
        int x = ctx.items[i].rc.left + ((ctx.items[i].rc.right - ctx.items[i].rc.left) - w) / 2;
        int y = ctx.items[i].rc.top + ((ctx.items[i].rc.bottom - ctx.items[i].rc.top) - h) / 2;
        hwnds[i] = CreateWindowExW(
            WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE | WS_EX_LAYERED,
            wc.lpszClassName, L"", WS_POPUP, x, y, w, h, NULL, NULL, g_app.inst, NULL);
        if (hwnds[i]) {
            SetWindowLongPtrW(hwnds[i], GWLP_USERDATA, (LONG_PTR)&ctx.items[i]);
            SetLayeredWindowAttributes(hwnds[i], 0, 232, LWA_ALPHA);
            ShowWindow(hwnds[i], SW_SHOWNOACTIVATE);
            UpdateWindow(hwnds[i]);
        }
    }

    DWORD start = GetTickCount();
    while ((DWORD)(GetTickCount() - start) < 3000u && !InterlockedCompareExchange(&s_stopping, 0, 0)) {
        MSG m;
        while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
        Sleep(30);
    }
    for (int i = 0; i < ctx.count; ++i) {
        if (hwnds[i]) DestroyWindow(hwnds[i]);
    }
    InterlockedExchange(&s_identify_busy, 0);
    return 0;
}

int shared_display_identify_async(void) {
    if (InterlockedCompareExchange(&s_stopping, 0, 0)) return 0;
    if (InterlockedCompareExchange(&s_identify_busy, 1, 0) != 0) return 1;
    if (s_identify_thread) { CloseHandle(s_identify_thread); s_identify_thread = NULL; }
    HANDLE t = CreateThread(NULL, 0, display_identify_worker, NULL, 0, NULL);
    if (!t) {
        InterlockedExchange(&s_identify_busy, 0);
        return 0;
    }
    s_identify_thread = t;
    return 1;
}
