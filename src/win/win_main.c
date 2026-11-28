/* =====================================================================
 *  HopDesk - Windows 入口
 *  单实例、隐藏消息窗口、托盘、网络、鼠标钩子。
 * ===================================================================== */
#include "app.h"
#include "xfer.h"
#include "update.h"
#include "main_ui.h"
#include "split_ui.h"
#include "../core/cli.h"
#include "../core/display_switch.h"
#include "../core/update_state.h"
#include "../platform/audio_link.h"
#include "../ui/hopdesk_ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <shellapi.h>
#include <wchar.h>

win_app g_app;
static int s_ui_guard;

/* 声明为 PerMonitorV2 DPI aware：统一使用物理像素，避免坐标虚拟化混乱。
 * 必须在创建任何窗口 / 取坐标之前调用。 */
static void set_dpi_aware(void) {
    HMODULE u = GetModuleHandleW(L"user32.dll");
    if (!u) return;
    typedef BOOL (WINAPI *set_ctx_t)(void *);
    set_ctx_t set_ctx = (set_ctx_t)GetProcAddress(u, "SetProcessDpiAwarenessContext");
    if (set_ctx) {
        if (set_ctx((void *)-4)) return;   /* PER_MONITOR_AWARE_V2 */
        set_ctx((void *)-3);               /* PER_MONITOR_AWARE    */
        return;
    }
    typedef BOOL (WINAPI *set_aware_t)(void);
    set_aware_t set_aware = (set_aware_t)GetProcAddress(u, "SetProcessDPIAware");
    if (set_aware) set_aware();
}

/* exe 同目录下的文件路径 */
static void beside(wchar_t *out, int n, const wchar_t *name) {
    wchar_t *p;
    GetModuleFileNameW(g_app.inst, out, n);
    p = wcsrchr(out, L'\\');
    if (p) wcscpy(p + 1, name);
}

void win_send_capabilities_ex(unsigned takeover_mask, unsigned flags) {
    uint8_t payload[HOP_CAPABILITIES_BYTES];
    if (!g_app.net || !net_server_has_client(g_app.net)) return;
    if (hop_capabilities_encode_ex(payload, g_app.cfg.input_mode, takeover_mask,
                                   g_app.cfg.audio_mode, flags))
        (void)net_server_send(g_app.net, HOP_MSG_CAPABILITIES, payload, sizeof(payload));
}

void win_send_capabilities(unsigned takeover_mask) {
    win_send_capabilities_ex(takeover_mask, 0u);
}

void win_apply_audio_mode(unsigned mode, int sync_peer, int persist) {
    mode &= HOP_AUDIO_MODES;
    g_app.cfg.audio_mode = mode;
    (void)hop_audio_set_mode(&g_app.cfg, mode);
    if (persist) {
        hop_config saved = g_app.cfg;
        (void)hop_config_load(&saved, g_app.config_path);
        saved.audio_mode = mode;
        (void)hop_config_save_atomic(&saved, g_app.config_path);
    }
    win_send_capabilities_ex(0u, sync_peer ? HOP_CAP_FLAG_AUDIO_SYNC : 0u);
    ui_refresh(&g_app);
}

void win_apply_input_mode(unsigned mode, unsigned takeover_mask, int persist) {
    mode &= HOP_INPUT_MODES;
    if (!(mode & HOP_INPUT_MOUSE)) {
        capture_set_sharing(0);
        capture_shutdown();
    }
    if (!(mode & HOP_INPUT_KEYBOARD)) {
        kbd_end_sharing();
        kbd_shutdown();
    }
    g_app.cfg.input_mode = mode;
    if (mode & HOP_INPUT_MOUSE) {
        win_inject_reset();
        capture_init(&g_app);
    }
    if (mode & HOP_INPUT_KEYBOARD) {
        kbd_init(&g_app);
    }
    if (g_app.state == HOP_LINK_SHARING && !(mode & HOP_INPUT_MODES))
        g_app.state = net_server_has_client(g_app.net) ? HOP_LINK_LOCAL : HOP_LINK_DOWN;
    if (persist) {
        hop_config saved = g_app.cfg;
        (void)hop_config_load(&saved, g_app.config_path);
        saved.input_mode = mode;
        (void)hop_config_save_atomic(&saved, g_app.config_path);
    }
    win_send_capabilities(takeover_mask & HOP_INPUT_MODES);
    ui_refresh(&g_app);
}

int win_apply_hot_config(const hop_config *next) {
    if (!next) return 0;
    unsigned prev_input = g_app.cfg.input_mode;
    unsigned prev_audio = g_app.cfg.audio_mode;
    unsigned next_input = next->input_mode & HOP_INPUT_MODES;
    unsigned next_audio = next->audio_mode & HOP_AUDIO_MODES;
    unsigned takeover = next_input & ~prev_input;

    /* Preserve transport-binding fields that require restart to rebind safely;
     * copy all hot-applicable parameters into live g_app.cfg. */
    hop_role live_role = g_app.cfg.role;
    uint16_t live_port = g_app.cfg.port;
    uint16_t live_data_port = g_app.cfg.data_port;
    uint16_t live_audio_port = g_app.cfg.audio_port;
    int live_w = g_app.cfg.screen_w;
    int live_h = g_app.cfg.screen_h;
    int live_rx_slots = g_app.cfg.xfer_rx_queue_slots;
    int live_rx_mb = g_app.cfg.xfer_rx_queue_mb;

    g_app.cfg = *next;
    g_app.cfg.role = live_role;
    g_app.cfg.port = live_port;
    g_app.cfg.data_port = live_data_port;
    g_app.cfg.audio_port = live_audio_port;
    g_app.cfg.screen_w = live_w;
    g_app.cfg.screen_h = live_h;
    g_app.cfg.xfer_rx_queue_slots = live_rx_slots;
    g_app.cfg.xfer_rx_queue_mb = live_rx_mb;
    g_app.cfg.input_mode = prev_input;
    g_app.cfg.audio_mode = next_audio;

    hop_runtime_apply_config(&g_app.cfg);
    if (next_input != prev_input)
        win_apply_input_mode(next_input, takeover, 0);
    if (next_audio != prev_audio) {
        (void)hop_audio_set_mode(&g_app.cfg, next_audio);
        win_send_capabilities_ex(0u, HOP_CAP_FLAG_AUDIO_SYNC);
    } else if (next_input == prev_input) {
        win_send_capabilities(0);
    }
    ui_refresh(&g_app);
    return hop_audio_set_mode(&g_app.cfg, g_app.cfg.audio_mode);
}

static void on_link(int up) {
    clip_on_link(up != 0);
    if (up) {
        g_app.peer_capabilities_known = 0;
        g_app.peer_input_mode = 0;
        g_app.peer_audio_mode = 0;
        if (up == 2 && g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
        if (g_app.state == HOP_LINK_DOWN) g_app.state = HOP_LINK_LOCAL;
        win_send_capabilities(0);
    } else {
        g_app.peer_capabilities_known = 0;
        g_app.peer_input_mode = 0;
        g_app.peer_audio_mode = 0;
        g_app.peer_name[0] = '\0';
        win_inject_reset();
        if (g_app.state == HOP_LINK_SHARING) capture_set_sharing(0);
        g_app.state = HOP_LINK_DOWN;
        xfer_kick();
    }
    ui_refresh(&g_app);
}

/* HELLO: {tlen, token, nlen, name, w(4), h(4)} */
static void on_hello(const uint8_t *p, uint32_t len) {
    hop_hello_msg hello;
    /* The network thread validates the token before publishing a session. */
    if (!hop_hello_decode(p, len, &hello)) return;
    g_app.peer_w = (int)hello.width;
    g_app.peer_h = (int)hello.height;
    if (hello.name && hello.name_len) {
        size_t copy = hello.name_len < sizeof(g_app.peer_name) - 1
            ? hello.name_len : sizeof(g_app.peer_name) - 1;
        memcpy(g_app.peer_name, hello.name, copy);
        g_app.peer_name[copy] = '\0';
    }

    hop_log(HOP_LOG_INFO, "peer screen %dx%d", g_app.peer_w, g_app.peer_h);
}

static void on_capabilities(const uint8_t *p, uint32_t len) {
    hop_capabilities_msg msg;
    unsigned next_input, next_audio;
    int changed = 0;
    if (!hop_capabilities_decode(p, len, &msg)) return;
    g_app.peer_input_mode = msg.input_mode;
    g_app.peer_audio_mode = msg.audio_mode;
    g_app.peer_capabilities_known = 1;
    next_input = hop_input_reconcile(g_app.cfg.input_mode, msg.input_mode,
                                     msg.takeover_mask, g_app.cfg.role);
    if (next_input != g_app.cfg.input_mode) {
        hop_log(HOP_LOG_INFO, "input: peer capability sync adjusted local input_mode %u -> %u",
                g_app.cfg.input_mode, next_input);
        win_apply_input_mode(next_input, 0, 1);
        changed = 1;
    }
    next_audio = hop_audio_reconcile(g_app.cfg.audio_mode, msg.audio_mode,
                                     msg.flags, g_app.cfg.role);
    if (next_audio != g_app.cfg.audio_mode) {
        hop_log(HOP_LOG_INFO, "audio: peer capability sync adjusted local audio_mode %u -> %u",
                g_app.cfg.audio_mode, next_audio);
        win_apply_audio_mode(next_audio, 0, 1);
        changed = 1;
    }
    if (!changed) ui_refresh(&g_app);
}

static void on_recv(hop_recv *r) {
    if (r->type == HOP_MSG_HELLO_ACK) {
        int ok; uint32_t w, h; hop_session_id id;
        if (hop_hello_ack_decode(r->data, r->len, &ok, &w, &h, &id)) {
            g_app.peer_w = (int)w;
            g_app.peer_h = (int)h;
            if (r->len >= 2) {
                size_t nl = r->data[1];
                if (nl > 0 && 2u + nl <= r->len) {
                    size_t copy = nl < sizeof(g_app.peer_name) - 1 ? nl : sizeof(g_app.peer_name) - 1;
                    memcpy(g_app.peer_name, r->data + 2, copy);
                    g_app.peer_name[copy] = '\0';
                }
            }
        }
    }
    win_inject_message(r->type, r->data, r->len);
    switch (r->type) {
        case HOP_MSG_HELLO:
            on_hello(r->data, r->len);
            break;
        case HOP_MSG_DISP_SWITCH:
            shared_display_message(r->data, r->len);
            break;
        case HOP_MSG_CAPABILITIES:
            on_capabilities(r->data, r->len);
            break;
        case HOP_MSG_EDGE_LEAVE:
            if (r->len >= 1 && (g_app.cfg.input_mode & HOP_INPUT_MOUSE) &&
                g_app.state == HOP_LINK_SHARING)
                capture_set_sharing(0);
            break;
        case HOP_MSG_CLIP_TEXT:
            clip_apply_remote(r->data, r->len);
            break;
        case HOP_MSG_CLIP_FILE_OFFER:
            clip_on_remote_file_offer(r->data, r->len);
            break;
        case HOP_MSG_CLIP_FILE_OFFER_CLEAR:
            clip_on_remote_file_offer_clear(r->data, r->len);
            break;
        case HOP_MSG_CLIP_PASTE_REQUEST:
            clip_on_paste_request(r->data, r->len);
            break;
        case HOP_MSG_CLIP_PASTE_FAILED:
            clip_on_paste_failed(r->data, r->len);
            break;
        default:
            break;
    }
}

/* ---- 阶段 4：显示切换 ---- */

/* 切到 Mac 显示：先让 Mac 醒，再切显示器输入源。
 * 两步之间必须留时间 —— 输入源一切走，Windows 就看不到画面了。 */
static int disp_switch_to_mac(void) {
    if (g_app.cfg.disp_shared) return shared_display_start(HOP_DISPLAY_MAC);
    uint8_t p[4];
    if (!net_server_has_client(g_app.net)) {
        hop_log(HOP_LOG_WARN, "no client, cannot switch display");
        return 0;
    }
    hop_put_be32(p, (uint32_t)g_app.cfg.disp_wake_sec);
    net_server_send(g_app.net, HOP_MSG_DISP_WAKE, p, 4);
    capture_set_sharing(0);          /* 画面即将走掉，键鼠先收回本机 */
    UINT wake_delay = g_app.cfg.disp_wake_delay_ms >= 0
        ? (UINT)g_app.cfg.disp_wake_delay_ms : (UINT)DISP_WAKE_DELAY_MS;
    SetTimer(g_app.hwnd, IDT_DISP, wake_delay, NULL);
    return 1;
}

/* 切回 Windows 显示：让 Mac 把输入源切回来；Mac 不可达就本机 DDC 硬切 */
static int disp_switch_to_win(void) {
    if (g_app.cfg.disp_shared) return shared_display_start(HOP_DISPLAY_WINDOWS);
    uint8_t p[2];
    hop_put_be16(p, g_app.cfg.disp_win_input);
    if (!net_server_send(g_app.net, HOP_MSG_DISP_INPUT, p, 2)) {
        hop_log(HOP_LOG_WARN, "client unreachable, falling back to local DDC");
        disp_set_input(g_app.cfg.disp_win_input);
    }
    capture_set_sharing(0);
    return 1;
}

static int disp_switch_toggle(void) {
    if (!g_app.cfg.disp_shared) {
        hop_log(HOP_LOG_WARN, "display: one-key toggle requires disp_shared=true");
        return 0;
    }
    return shared_display_start(HOP_DISPLAY_AUTO);
}

static int disp_wake_mac(void) {
    uint8_t p[4];
    if (!net_server_has_client(g_app.net)) {
        hop_log(HOP_LOG_WARN, "no client, cannot wake display");
        return 0;
    }
    hop_put_be32(p, (uint32_t)g_app.cfg.disp_wake_sec);
    return net_server_send(g_app.net, HOP_MSG_DISP_WAKE, p, 4);
}

static int disp_sleep_mac(void) {
    int ok = net_server_send(g_app.net, HOP_MSG_DISP_SLEEP, NULL, 0);
    capture_set_sharing(0);
    return ok;
}

int win_ui_display_action(int action) {
    if (action == HOPDESK_UI_DISPLAY_IDENTIFY)
        return shared_display_identify_async();
    if (shared_display_busy()) {
        hop_log(HOP_LOG_WARN, "display: switch in progress; retry after completion");
        return 0;
    }
    switch (action) {
        case HOPDESK_UI_DISPLAY_TO_MAC: return disp_switch_to_mac();
        case HOPDESK_UI_DISPLAY_TO_WINDOWS: return disp_switch_to_win();
        case HOPDESK_UI_DISPLAY_TOGGLE: return disp_switch_toggle();
        case HOPDESK_UI_DISPLAY_WAKE: return disp_wake_mac();
        case HOPDESK_UI_DISPLAY_SLEEP: return disp_sleep_mac();
        case HOPDESK_UI_DISPLAY_PROBE: return shared_display_probe_async();
        default: return 0;
    }
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_APP_LINK:
            if ((unsigned long)lp == net_server_epoch(g_app.net)) on_link((int)wp);
            return 0;
        case WM_APP_RECV: {
            hop_recv *r = (hop_recv *)lp;
            if (r->epoch == net_server_epoch(g_app.net)) on_recv(r);
            free(r);
            return 0;
        }
        case WM_APP_CLIP:
            clip_on_worker_message((UINT)wp, (HGLOBAL)lp);
            return 0;
        case WM_APP_CLIP_PASTE:
            clip_on_paste_message((HGLOBAL)lp, (DWORD)wp);
            return 0;
        case WM_APP_SPLIT_REQUEST:
            win_split_agent_dispatch();
            return 0;
        case WM_DISPLAYCHANGE:
            capture_topology_changed();
            return 0;
        case WM_APP_DISPLAY_DONE:
            capture_topology_changed();
            ui_refresh(&g_app);
            return 0;
        case WM_APP_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == NIN_SELECT ||
                LOWORD(lp) == NIN_KEYSELECT) {
                ui_show_main(&g_app);
            } else if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) {
                ui_show_menu(&g_app);
            }
            return 0;
        case WM_CLIPBOARDUPDATE:
            clip_on_update();
            return 0;
        case WM_TIMER:
            if (wp == IDT_HOOK) { hop_update_report_ready(HOP_VERSION); hop_watchdog_ping(s_ui_guard); win_inject_retry_pending(); if(g_app.cfg.input_mode & HOP_INPUT_KEYBOARD)kbd_retry_pending(); if(g_app.cfg.input_mode & HOP_INPUT_MOUSE)capture_check_hooks(); ui_refresh(&g_app); return 0; }
            if (wp == IDT_XFER_NOTIFY) { win_split_agent_reap(); ui_transfer_poll(); return 0; }
            if (wp == IDT_CLIP_RETRY) {
                clip_retry_pending();
                clip_on_update(); /* retry the latest local clipboard snapshot too */
                return 0;
            }
            if (wp == IDT_DISP) {
                KillTimer(h, IDT_DISP);
                disp_set_input(g_app.cfg.disp_mac_input);
                return 0;
            }
            if (wp == IDT_EDGE) {
                KillTimer(h, IDT_EDGE);
                capture_on_edge_timer();
                return 0;
            }
            break;
        case WM_COMMAND:
            switch (LOWORD(wp)) {
                case 1090: ui_show_main(&g_app); return 0;
                case 1091: DestroyWindow(h); return 0;
                case 1001: capture_force_toggle(); return 0;
                case 1002: DestroyWindow(h); return 0;
                case 1003: (void)win_ui_display_action(HOPDESK_UI_DISPLAY_TO_MAC); return 0;
                case 1004: (void)win_ui_display_action(HOPDESK_UI_DISPLAY_TO_WINDOWS); return 0;
                case 1005: (void)win_ui_display_action(HOPDESK_UI_DISPLAY_SLEEP); return 0;
                case 1006: xfer_cancel(); return 0;
                case 1007: ui_show_transfers(&g_app); return 0;
                case 1008: update_check_async(g_app.hwnd, g_app.cfg.update_channel); return 0;
                case 1009: (void)win_ui_display_action(HOPDESK_UI_DISPLAY_WAKE); return 0;
                case 1010: (void)win_ui_display_action(HOPDESK_UI_DISPLAY_TOGGLE); return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static int cmdline_has_flag(const wchar_t *cmd, const wchar_t *flag) {
    if (!cmd || !flag || !flag[0]) return 0;
    size_t len = wcslen(flag);
    const wchar_t *p = cmd;
    while (*p) {
        while (*p == L' ' || *p == L'\t') ++p;
        if (!*p) break;
        const wchar_t *start = p;
        if (*p == L'"') {
            ++p;
            while (*p && *p != L'"') ++p;
            if (*p == L'"') ++p;
        } else {
            while (*p && *p != L' ' && *p != L'\t') ++p;
        }
        if ((size_t)(p - start) == len && wcsncmp(start, flag, len) == 0)
            return 1;
    }
    return 0;
}

static void win_cli_write_handle(DWORD std_id, const char *data, size_t len) {
    HANDLE h = GetStdHandle(std_id);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    while (len > 0) {
        DWORD chunk = len > 65536u ? 65536u : (DWORD)len;
        DWORD written = 0;
        if (!WriteFile(h, data, chunk, &written, NULL) || !written) break;
        data += written;
        len -= written;
    }
}
static void win_cli_write_out(void *ctx, const char *data, size_t len) {
    (void)ctx;
    win_cli_write_handle(STD_OUTPUT_HANDLE, data, len);
}
static void win_cli_write_err(void *ctx, const char *data, size_t len) {
    (void)ctx;
    win_cli_write_handle(STD_ERROR_HANDLE, data, len);
}

static int run_win_cli_if_requested(HINSTANCE inst, int *exit_code) {
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv || argc < 2) {
        if (wargv) LocalFree(wargv);
        return 0;
    }
    char **argv = (char **)calloc((size_t)argc, sizeof(char *));
    if (!argv) { LocalFree(wargv); return 0; }
    for (int i = 0; i < argc; ++i) {
        int need = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
        if (need <= 0) { argv[i] = _strdup(""); continue; }
        argv[i] = (char *)malloc((size_t)need);
        if (argv[i]) WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], need, NULL, NULL);
    }
    LocalFree(wargv);

    int is_cli = hop_cli_is_invocation(argc, (const char *const *)argv);
    if (is_cli) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE hErr = GetStdHandle(STD_ERROR_HANDLE);
        int attached = 0;
        if ((!hOut || hOut == INVALID_HANDLE_VALUE || GetFileType(hOut) == FILE_TYPE_UNKNOWN) &&
            (!hErr || hErr == INVALID_HANDLE_VALUE || GetFileType(hErr) == FILE_TYPE_UNKNOWN)) {
            attached = AttachConsole(ATTACH_PARENT_PROCESS) != 0;
        }
        g_app.inst = inst;
        wchar_t conf_w[MAX_PATH], log_w[MAX_PATH];
        char conf_u[MAX_PATH * 3] = {0}, log_u[MAX_PATH * 3] = {0};
        beside(conf_w, MAX_PATH, L"hopdesk.conf");
        beside(log_w, MAX_PATH, L"hopdesk.log");
        WideCharToMultiByte(CP_UTF8, 0, conf_w, -1, conf_u, sizeof(conf_u), NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, log_w, -1, log_u, sizeof(log_u), NULL, NULL);

        hop_cli_ops ops;
        memset(&ops, 0, sizeof(ops));
        ops.default_config_path = conf_u;
        ops.default_log_path = log_u;
        ops.default_device_name = "Alex";
        ops.write_out = win_cli_write_out;
        ops.write_err = win_cli_write_err;
        ops.agent_get_status = win_cli_agent_get_status;
        ops.agent_get_config = win_cli_agent_get_config;
        ops.agent_save_config = win_cli_agent_save_config;
        ops.agent_action = win_cli_agent_action;

        *exit_code = hop_cli_run(argc, (const char *const *)argv, &ops);
        if (attached) FreeConsole();
    }
    for (int i = 0; i < argc; ++i) free(argv[i]);
    free(argv);
    return is_cli;
}

static void extract_win_config_override(char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = '\0';
    int argc = 0;
    LPWSTR *wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!wargv) return;
    for (int i = 1; i < argc; ++i) {
        const wchar_t *val = NULL;
        if (wcscmp(wargv[i], L"--config") == 0 && i + 1 < argc) val = wargv[i + 1];
        else if (wcsncmp(wargv[i], L"--config=", 9) == 0 && wargv[i][9]) val = wargv[i] + 9;
        if (val) {
            WideCharToMultiByte(CP_UTF8, 0, val, -1, out, (int)cap, NULL, NULL);
            break;
        }
    }
    LocalFree(wargv);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmd, int show) {
    (void)prev; (void)show;

    {
        int cli_exit = 0;
        if (run_win_cli_if_requested(inst, &cli_exit)) return cli_exit;
    }

    set_dpi_aware();
    /* UI returns before agent singleton, tray, hooks, network, transfer,
     * clipboard listener and audio startup. */
    if (cmd && wcsncmp(cmd, L"--ui", 4) == 0) {
        unsigned long parent = 0;
        int page = 0;
        wchar_t extra = 0;
        wchar_t log_path[MAX_PATH];
        if (swscanf(cmd, L"--ui %lu %d %lc", &parent, &page, &extra) != 2 ||
            page < 0 || page >= HOPDESK_UI_PAGE_COUNT ||
            !win_split_client_init((DWORD)parent)) return 1;
        g_app.inst = inst;
        beside(log_path, MAX_PATH, L"hopdesk.log");
        WideCharToMultiByte(CP_UTF8, 0, log_path, -1, g_app.log_path,
                            sizeof(g_app.log_path), NULL, NULL);
        main_ui_show_page(page);
        win_split_client_close();
        return 0;
    }

    const int silent_agent = cmdline_has_flag(cmd, L"--agent") ||
        GetEnvironmentVariableW(L"HOPDESK_UPDATE_READY", NULL, 0) > 0;

    /* ---- 单实例 ----
     * 两个实例会同时 bind 24817/24818，而 Windows 的 SO_REUSEADDR 允许
     * 后来者绑上同一个端口并把连接抢走 —— 表现是"对端连上了，但键鼠时灵时不灵"，
     * 极难排查。用命名互斥量在最早的时刻拦掉第二个实例。
     *
     * 句柄故意不关：互斥量要活到进程结束，进程一退内核自动释放，
     * 所以崩溃也不会留下"需要手工清理"的僵尸锁。 */
    static HANDLE s_instance_mtx = NULL;
    s_instance_mtx = CreateMutexW(NULL, TRUE, L"Local\\HopDesk.SingleInstance");
    if (s_instance_mtx && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (!silent_agent) {
            HWND existing = NULL;
            for (int i = 0; i < 10 && !existing; ++i) {
                existing = FindWindowExW(HWND_MESSAGE, NULL, L"HopDeskMsg", L"HopDesk");
                if (!existing) Sleep(50);
            }
            if (existing) {
                DWORD agent_pid = 0;
                GetWindowThreadProcessId(existing, &agent_pid);
                if (agent_pid) AllowSetForegroundWindow(agent_pid);
                PostMessageW(existing, WM_COMMAND, 1090, 0);
            } else {
                MessageBoxW(NULL,
                            L"HopDesk 已经在运行。\n\n"
                            L"请到任务栏通知区域（托盘）里操作它的图标。",
                            L"HopDesk", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            }
        }
        CloseHandle(s_instance_mtx);
        s_instance_mtx = NULL;
        return 0;
    }

    int startup_guard = hop_watchdog_begin("Windows startup", 30000);
    g_app.inst = inst;
    g_app.state = HOP_LINK_DOWN;

    hop_config_defaults(&g_app.cfg);
    snprintf(g_app.cfg.device_name, HOP_NAME_MAX, "%s", "Alex");
    g_app.cfg.screen_w = GetSystemMetrics(SM_CXSCREEN);
    g_app.cfg.screen_h = GetSystemMetrics(SM_CYSCREEN);

    char conf_utf8[MAX_PATH * 3];
    extract_win_config_override(conf_utf8, sizeof(conf_utf8));
    if (!conf_utf8[0]) {
        wchar_t conf_path[MAX_PATH];
        beside(conf_path, MAX_PATH, L"hopdesk.conf");
        WideCharToMultiByte(CP_UTF8, 0, conf_path, -1, conf_utf8, sizeof(conf_utf8), NULL, NULL);
    }
    snprintf(g_app.config_path, sizeof(g_app.config_path), "%s", conf_utf8);
    hop_config_load(&g_app.cfg, conf_utf8);
    hop_runtime_apply_config(&g_app.cfg);

    wchar_t log_path[MAX_PATH];
    beside(log_path, MAX_PATH, L"hopdesk.log");
    char log_utf8[MAX_PATH * 3];
    WideCharToMultiByte(CP_UTF8, 0, log_path, -1, log_utf8, sizeof(log_utf8), NULL, NULL);
    snprintf(g_app.log_path, sizeof(g_app.log_path), "%s", log_utf8);
    hop_log_open(log_utf8);

    hop_log(HOP_LOG_INFO, "HopDesk %s starting", HOP_VERSION);

    /* 隐藏消息窗口 */
    WNDCLASSW wc;
    memset(&wc, 0, sizeof(wc));
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = inst;
    wc.lpszClassName = L"HopDeskMsg";
    RegisterClassW(&wc);
    g_app.hwnd = CreateWindowExW(0, wc.lpszClassName, L"HopDesk",
                                 0, 0, 0, 0, 0, HWND_MESSAGE, NULL, inst, NULL);

    ui_init(&g_app);
    /* Start the data channel before the control server: every control callback
     * (including disconnect -> xfer_kick) must see a live transfer subsystem. */
    if (!xfer_init(g_app.hwnd, &g_app.cfg)) {
        hop_log(HOP_LOG_ERROR, "transfer worker initialization failed");
        while (!xfer_shutdown()) Sleep(100); /* fail closed; retain owners until settled */
        ui_shutdown(&g_app);
        DestroyWindow(g_app.hwnd);
        return 1;
    }
    g_app.net = net_server_start(g_app.hwnd, &g_app.cfg);
    if (!g_app.net) {
        hop_log(HOP_LOG_ERROR, "control network worker initialization failed");
        while (!xfer_shutdown()) Sleep(100);
        ui_shutdown(&g_app);
        DestroyWindow(g_app.hwnd);
        return 1;
    }
    clip_init(g_app.hwnd);
    if(g_app.cfg.input_mode & HOP_INPUT_MOUSE) capture_init(&g_app);
    if(g_app.cfg.input_mode & HOP_INPUT_KEYBOARD) kbd_init(&g_app);
    if(!hop_audio_set_mode(&g_app.cfg,g_app.cfg.audio_mode))
        hop_log(HOP_LOG_WARN,"audio: unable to start configured audio mode %u",g_app.cfg.audio_mode);
    int split_started = win_split_agent_start(g_app.hwnd);
    if (!split_started) {
        hop_log(HOP_LOG_ERROR, "unable to start local UI IPC");
        MessageBoxW(NULL, L"Unable to initialize HopDesk UI IPC.", L"HopDesk", MB_OK | MB_ICONERROR);
        DestroyWindow(g_app.hwnd);
    }
    hop_watchdog_end(startup_guard);
    s_ui_guard = hop_watchdog_begin("Windows message loop", 30000);
    SetTimer(g_app.hwnd, IDT_HOOK, 3000, NULL);
    /* Manual launch opens the main window once; closing it leaves the agent in the tray. */
    if (split_started && !silent_agent) {
        PostMessageW(g_app.hwnd, WM_COMMAND, 1090, 0);
    }

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    win_split_agent_stop();
    update_shutdown();
    hop_watchdog_end(s_ui_guard);
    KillTimer(g_app.hwnd, IDT_HOOK);
    win_inject_reset();
    kbd_shutdown();
    shared_display_shutdown();
    capture_shutdown();
    hop_audio_shutdown();
    while (!clip_shutdown(g_app.hwnd)) Sleep(100); /* retain unresolved image Task owners */
    /* Stop/join control callbacks before destroying transfer locks: on_link()
     * can call xfer_kick after a disconnect. */
    if (g_app.net) { net_server_stop(g_app.net); g_app.net = NULL; }
    while (!xfer_shutdown()) Sleep(100); /* do not exit with a live owner/Reservation */
    ui_shutdown(&g_app);
    hop_log(HOP_LOG_INFO, "stopped");
    /* 正常退出时也释放互斥量，别指望"反正进程要退了" —— 万一有别的
     * 退出路径（比如宿主进程 fork 出来的子进程）还活着，锁就悬着了 */
    if (s_instance_mtx) CloseHandle(s_instance_mtx);
    return 0;
}
