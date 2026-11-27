/* HopDesk Windows UI host: native tray and message shell, shared CUI window. */
#include "app.h"
#include "split_ui.h"
#include "main_ui.h"
#include "../ui/hopdesk_ui.h"

#include <cui/cui_app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

static int s_running;
static int s_ui_initialized;
static hop_config s_ui_config;
static HANDLE s_save_thread;
static hop_config s_save_config;
static int s_save_result;
static ULONGLONG s_last_snapshot;
static hop_xfer_task_id s_last_task;
static uint32_t s_last_offset;
static unsigned s_failed_snapshots;

static DWORD WINAPI save_config_worker(LPVOID unused) {
    (void)unused;
    s_save_result = win_split_save(&s_save_config);
    return 0;
}
static void collect_save(void) {
    if (!s_save_thread || WaitForSingleObject(s_save_thread, 0) != WAIT_OBJECT_0) return;
    CloseHandle(s_save_thread); s_save_thread = NULL;
    if (s_save_result) s_ui_config = s_save_config;
    s_last_snapshot = 0;
    hopdesk_ui_save_complete(s_save_result != 0, s_save_result == 1
        ? "设置已保存。声音能力立即应用；网络、角色和认证参数重启后生效。"
        : s_save_result == 2 ? "设置已保存，但音频后台未能启动。请查看日志。"
        : "后台保存未确认；请重新打开界面核对配置。");
}
static void save_config(const hop_config *config) {
    if (!config || s_save_thread) return;
    s_save_config = *config;
    s_save_result = 0;
    s_save_thread = CreateThread(NULL, 0, save_config_worker, NULL, 0, NULL);
    if (!s_save_thread) hopdesk_ui_save_complete(0, "无法启动设置保存线程。");
}
static void snapshot_model(hopdesk_ui_model *model, hop_xfer_task_id selected_task,
                           uint32_t file_offset) {
    ULONGLONG now = GetTickCount64();
    int page = -1;
    collect_save();
    if (!win_split_client_alive()) { main_ui_shutdown(); return; }
    if (s_save_thread) return;
    if (s_last_snapshot && now - s_last_snapshot < 250 &&
        s_last_task == selected_task && s_last_offset == file_offset) return;
    s_last_snapshot = now; s_last_task = selected_task; s_last_offset = file_offset;
    if (win_split_snapshot(model, selected_task, file_offset, &page)) {
        s_ui_config = model->config;
        s_failed_snapshots = 0;
        if (page >= 0 && page < HOPDESK_UI_PAGE_COUNT) hopdesk_ui_show_page(page);
    } else {
        model->connected = model->sharing = model->data_connected = 0;
        if (++s_failed_snapshots >= 3) main_ui_shutdown();
    }
}
static int cancel_task(hop_xfer_task_id id) {
    s_last_snapshot = 0; return win_split_cancel(id);
}
static int retry_file(uint32_t batch, int index) {
    s_last_snapshot = 0; return win_split_retry(batch, index);
}
static void reconnect_peer(void) { (void)win_split_reconnect(); s_last_snapshot = 0; }
static void display_action(int action) { (void)win_split_display(action); }
static void check_updates(void) { (void)win_split_update(); }

static int load_logs(char *out, size_t capacity) {
    FILE *file;
    long size, start;
    size_t got;
    if (!out || capacity < 2) return 0;
    out[0] = '\0';
    file = fopen(g_app.log_path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0) {
        fclose(file);
        return 0;
    }
    start = size > (long)(capacity - 1) ? size - (long)(capacity - 1) : 0;
    if (fseek(file, start, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    got = fread(out, 1, capacity - 1, file);
    fclose(file);
    out[got] = '\0';
    if (start > 0) {
        char *newline = strchr(out, '\n');
        if (newline) memmove(out, newline + 1, strlen(newline + 1) + 1);
    }
    return 1;
}

static int copy_text(const char *utf8) {
    int chars;
    HGLOBAL memory;
    wchar_t *wide;
    size_t bytes;
    if (!utf8) return 0;
    chars = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, NULL, 0);
    if (chars <= 0) return 0;
    bytes = (size_t)chars * sizeof(wchar_t);
    memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) return 0;
    wide = (wchar_t *)GlobalLock(memory);
    if (!wide || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8, -1, wide, chars)) {
        if (wide) GlobalUnlock(memory);
        GlobalFree(memory);
        return 0;
    }
    GlobalUnlock(memory);
    if (!OpenClipboard(GetActiveWindow())) {
        GlobalFree(memory);
        return 0;
    }
    EmptyClipboard();
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        CloseClipboard();
        GlobalFree(memory);
        return 0;
    }
    CloseClipboard();
    return 1;
}

static void default_config(hop_config *config) {
    char name[HOP_NAME_MAX];
    if (!config) return;
    snprintf(name, sizeof(name), "%s",
        s_ui_config.device_name[0] ? s_ui_config.device_name : "Windows");
    hop_config_defaults(config);
    snprintf(config->device_name, sizeof(config->device_name), "%s", name);
}

static HANDLE s_cjk_font_file = INVALID_HANDLE_VALUE;
static HANDLE s_cjk_font_mapping;
static const void *s_cjk_font_view;

static void release_cjk_font(void) {
    if (s_cjk_font_view) UnmapViewOfFile(s_cjk_font_view);
    if (s_cjk_font_mapping) CloseHandle(s_cjk_font_mapping);
    if (s_cjk_font_file != INVALID_HANDLE_VALUE) CloseHandle(s_cjk_font_file);
    s_cjk_font_view = NULL;
    s_cjk_font_mapping = NULL;
    s_cjk_font_file = INVALID_HANDLE_VALUE;
}

static cui_font load_cjk_font(cui_ctx *ui) {
    static const wchar_t *const candidates[] = {
        L"C:\\Windows\\Fonts\\msyh.ttc",
        L"C:\\Windows\\Fonts\\simhei.ttf",
        L"C:\\Windows\\Fonts\\simsun.ttc"
    };
    size_t i;
    for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); ++i) {
        HANDLE file = CreateFileW(candidates[i], GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
        LARGE_INTEGER size;
        HANDLE mapping;
        const void *view;
        cui_font font;
        if (file == INVALID_HANDLE_VALUE) continue;
        if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 ||
            (unsigned long long)size.QuadPart > (unsigned long long)SIZE_MAX) {
            CloseHandle(file);
            continue;
        }
        mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
        if (!mapping) {
            CloseHandle(file);
            continue;
        }
        view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
        if (!view) {
            CloseHandle(mapping);
            CloseHandle(file);
            continue;
        }
        s_cjk_font_file = file;
        s_cjk_font_mapping = mapping;
        s_cjk_font_view = view;
        font = cui_font_load_memory(ui, view, (size_t)size.QuadPart, false);
        if (font >= 0) return font;
        release_cjk_font();
    }
    return -1;
}

static const hopdesk_ui_ops k_ui_ops = {
    snapshot_model,
    save_config,
    cancel_task,
    retry_file,
    reconnect_peer,
    display_action,
    check_updates,
    NULL,
    load_logs,
    copy_text,
    default_config,
    load_cjk_font,
    release_cjk_font
};

static void draw_frame(cui_ctx *ui, void *user) {
    (void)user;
    if (!s_running) return;
    if (!s_ui_initialized) {
        hopdesk_ui_init(ui);
        s_ui_initialized = 1;
    }
    hopdesk_ui_frame(ui);
}

void main_ui_show_page(int page) {
    cui_app_desc desc;
    if (s_running) {
        hopdesk_ui_show_page(page);
        return;
    }
    if (!win_split_client_active()) return;
    hop_config_defaults(&s_ui_config);
    s_last_snapshot = 0;
    s_failed_snapshots = 0;
    hopdesk_ui_set_ops(&k_ui_ops);
    hopdesk_ui_show_page(page);
    memset(&desc, 0, sizeof(desc));
    desc.title = "HopDesk";
    desc.width = 1180;
    desc.height = 820;
    desc.min_width = 960;
    desc.min_height = 640;
    desc.renderer = CUI_RENDERER_GPU;
    desc.follow_system_theme = true;
    desc.accent = CUI_ACCENT_BLUE;
    s_running = 1;
    (void)cui_app_run(&desc, draw_frame, NULL);
    s_running = 0;
    s_ui_initialized = 0;
    /* The context is gone: settle the save without calling UI callbacks. */
    if (s_save_thread) {
        WaitForSingleObject(s_save_thread, INFINITE);
        CloseHandle(s_save_thread); s_save_thread = NULL;
    }
    hopdesk_ui_shutdown();
}

void main_ui_refresh(void) {
    if (s_running) hopdesk_ui_refresh();
}

void main_ui_shutdown(void) {
    if (s_running) cui_app_quit(NULL);
}

