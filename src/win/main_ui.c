/* HopDesk Windows UI host: native tray and message shell, shared CUI window. */
#include "app.h"
#include "xfer.h"
#include "update.h"
#include "main_ui.h"
#include "../ui/hopdesk_ui.h"
#include "../platform/audio_link.h"

#include <cui/cui_app.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <io.h>

static int s_running;
static int s_ui_initialized;
static int s_config_loaded;
static hop_config s_ui_config;

static const hop_xfer_task_snapshot *find_task(const hopdesk_ui_model *model,
                                                hop_xfer_task_id id) {
    int i;
    if (!model || !id) return NULL;
    for (i = 0; i < model->task_count; ++i)
        if (model->tasks[i].id == id) return &model->tasks[i];
    return NULL;
}

static void snapshot_model(hopdesk_ui_model *model,
                           hop_xfer_task_id selected_task,
                           uint32_t file_offset) {
    uint64_t task_revision = 0, history_revision = 0, revision = 0;
    int total = 0, n = 0;
    const hop_xfer_task_snapshot *selected;

    if (!model) return;
    if (!s_config_loaded) {
        s_ui_config = g_app.cfg;
        (void)hop_config_load(&s_ui_config, g_app.config_path);
        s_config_loaded = 1;
    }
    model->config = s_ui_config;
    local_addresses(model->local_addresses, sizeof(model->local_addresses));

    model->connected = g_app.state != HOP_LINK_DOWN;
    model->sharing = g_app.state == HOP_LINK_SHARING;
    model->data_connected = xfer_connected();
    model->permissions_known = 0;
    if (model->config.role == HOP_ROLE_CLIENT)
        snprintf(model->peer_label, sizeof(model->peer_label), "%s",
            model->config.peer_host[0] ? model->config.peer_host : "服务器地址未配置");
    else
        snprintf(model->peer_label, sizeof(model->peer_label), "%s",
            model->connected ? "Mac" : "等待 Mac 连接");

    {
        hop_audio_status audio = hop_audio_get_status();
        model->audio_requested = audio.requested;
        model->audio_active = audio.active;
        model->audio_unavailable = audio.unavailable;
    }
    xfer_get_status_pair(&model->transfer);

    hop_xfer_task_get_revisions(&task_revision, &history_revision);
    if (!model->task_snapshot_valid || task_revision != model->task_revision) {
        n = hop_xfer_task_list_revisioned(model->tasks, HOPDESK_UI_TASK_CAP,
                                          &total, &revision);
        model->task_count = n > 0 ? n : 0;
        model->task_total = total;
        model->task_revision = revision;
        model->task_snapshot_valid = 1;
    }
    if (!model->history_snapshot_valid || history_revision != model->history_revision) {
        n = hop_xfer_task_history_store_list(model->history, HOPDESK_UI_HISTORY_CAP,
            HOP_XFER_DIR_NONE, HOP_XFER_TASK_EMPTY, 1, &total);
        model->history_count = n > 0 ? n : 0;
        model->history_total = total;
        model->history_revision = history_revision;
        model->history_snapshot_valid = 1;
    }

    selected = find_task(model, selected_task);
    if (!selected || !selected->transfer_id ||
        (selected->direction != HOP_XFER_DIR_SEND &&
         selected->direction != HOP_XFER_DIR_RECEIVE)) {
        model->file_count = 0;
        model->file_total = 0;
        model->file_batch = 0;
        model->file_offset = file_offset;
        model->file_revision = 0;
        model->file_direction = HOP_XFER_DIR_NONE;
        return;
    }

    {
        int file_total = 0;
        uint64_t file_revision = 0;
        (void)xfer_get_files_page(selected->direction, selected->transfer_id,
            file_offset, NULL, 0, &file_total, &file_revision);
        if (model->file_batch != selected->transfer_id ||
            model->file_direction != selected->direction ||
            model->file_offset != file_offset ||
            model->file_revision != file_revision) {
            n = xfer_get_files_page(selected->direction, selected->transfer_id,
                file_offset, model->files, HOPDESK_UI_FILE_PAGE,
                &file_total, &file_revision);
            model->file_count = n > 0 ? n : 0;
        }
        model->file_total = file_total;
        model->file_batch = selected->transfer_id;
        model->file_direction = selected->direction;
        model->file_offset = file_offset;
        model->file_revision = file_revision;
    }
}

typedef struct {
    hop_config config;
    int ok;
} win_ui_save_result;

static DWORD WINAPI save_config_worker(LPVOID argument) {
    win_ui_save_result *result = (win_ui_save_result *)argument;
    result->ok = hop_config_save_atomic(&result->config, g_app.config_path);
    if (!PostMessageW(g_app.hwnd, WM_APP_UI_SAVE_RESULT, 0, (LPARAM)result))
        free(result);
    return 0;
}

static void save_config(const hop_config *config) {
    win_ui_save_result *result;
    HANDLE thread;
    if (!config) {
        hopdesk_ui_save_complete(0, "没有可保存的设置。");
        return;
    }
    result = (win_ui_save_result *)calloc(1, sizeof(*result));
    if (!result) {
        hopdesk_ui_save_complete(0, "内存不足，无法保存设置。");
        return;
    }
    result->config = *config;
    thread = CreateThread(NULL, 0, save_config_worker, result, 0, NULL);
    if (!thread) {
        free(result);
        hopdesk_ui_save_complete(0, "无法启动设置保存线程。");
        return;
    }
    CloseHandle(thread);
}

void main_ui_save_complete(void *opaque) {
    win_ui_save_result *result = (win_ui_save_result *)opaque;
    int audio_ok;
    if (!result) return;
    if (!result->ok) {
        free(result);
        hopdesk_ui_save_complete(0, "保存失败；原配置未改变。请检查文件权限和磁盘空间。");
        return;
    }
    s_ui_config = result->config;
    /* Audio is hot-applied; role, ports, peer and authentication take effect on restart. */
    g_app.cfg.audio_mode = result->config.audio_mode;
    audio_ok = hop_audio_set_mode(&g_app.cfg, g_app.cfg.audio_mode);
    free(result);
    hopdesk_ui_save_complete(1, audio_ok
        ? "设置已保存。声音能力立即应用；网络、角色和认证参数重启后生效。"
        : "设置已保存，但音频后台未能启动。请查看日志；重启后会按已保存配置重试。");
}

static int cancel_task(hop_xfer_task_id id) {
    return xfer_cancel_task(id);
}

static int retry_file(uint32_t batch, int index) {
    return xfer_retry_file(batch, index);
}

static void reconnect_peer(void) {
    if (g_app.net) net_server_drop(g_app.net);
    xfer_kick();
}

static void display_action(int action) {
    win_ui_display_action(action);
}

static void check_updates(void) {
    update_check_async(g_app.hwnd);
}

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
    if (!OpenClipboard(g_app.hwnd)) {
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
    if (!s_config_loaded) {
        s_ui_config = g_app.cfg;
        (void)hop_config_load(&s_ui_config, g_app.config_path);
        s_config_loaded = 1;
    }
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
    hopdesk_ui_shutdown();
    /* The CUI loop pumps the thread queue. If the hidden shell window posted
     * WM_QUIT while the main window was open, it consumed that message; hand it
     * back to the outer application loop after the CUI window has been destroyed. */
    if (g_app.exit_requested) PostQuitMessage(0);
    hopdesk_ui_shutdown();
}

void main_ui_refresh(void) {
    if (s_running) hopdesk_ui_refresh();
}

void main_ui_shutdown(void) {
    if (s_running) cui_app_quit(NULL);
}

int main_ui_translate(MSG *message) {
    (void)message;
    return 0;
}
