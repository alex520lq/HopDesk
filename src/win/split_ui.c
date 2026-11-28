/* Local Windows UI/agent transport. No CUI renderer is initialized here.
 * One agent-owned pipe, one admitted child PID, one request at a time.
 * The worker handles bounded I/O; engine actions run on the agent message
 * thread. Posted messages contain no cross-process pointers. */
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#include "split_ui.h"
#include "xfer.h"
#include "update.h"
#include "../core/config_schema.h"
#include "../platform/audio_link.h"
#include <sddl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#ifndef PIPE_REJECT_REMOTE_CLIENTS
#define PIPE_REJECT_REMOTE_CLIENTS 0x00000008
#endif
#define WIN_SPLIT_VERSION 5u
#define WIN_SPLIT_IO_MS 1500u
#define WIN_SPLIT_ACTION_MS 5000u
#define WIN_SPLIT_ADDR_TTL_MS 10000u
#define WIN_SPLIT_MODEL_BYTES ((DWORD)offsetof(hopdesk_ui_model, logs))
#define WIN_CTRL_PIPE_NAME L"\\\\.\\pipe\\HopDesk.Control.v1"
enum { IPC_SNAPSHOT = 1, IPC_SAVE, IPC_CANCEL, IPC_RETRY,
       IPC_RECONNECT, IPC_DISCONNECT, IPC_DISPLAY, IPC_UPDATE,
       IPC_INPUT_MODE, IPC_THEME, IPC_CLI_STATUS, IPC_CLI_CONFIG_GET,
       IPC_CLI_RELOAD };
typedef struct {
    uint32_t version, op;
    uint64_t task;
    uint32_t offset, batch;
    int32_t index, action;
    hop_config config;
} ipc_request;
typedef struct {
    uint32_t version, op, bytes;
    int32_t result, page;
} ipc_reply;
static HWND s_owner;
static HANDLE s_pipe = INVALID_HANDLE_VALUE, s_ctrl_pipe = INVALID_HANDLE_VALUE;
static HANDLE s_stop, s_done, s_worker, s_ctrl_worker;
static CRITICAL_SECTION s_ipc_lock;
static int s_ipc_lock_init;
static HANDLE s_child, s_job;
static volatile LONG s_child_pid, s_pending;
static int s_pending_page = -1;
static ipc_request s_request;
static ipc_reply s_reply;
static void *s_payload; /* worker-owned; main fills only during dispatch */
static hopdesk_ui_model *s_agent_model; /* allocated only while UI is open */
static hop_config s_agent_config;
static int s_agent_config_loaded;
static char s_cached_addresses[512];
static ULONGLONG s_cached_addresses_tick;
static DWORD s_parent_pid;
static HANDLE s_parent;
static void *s_client_payload;

static int pipe_name(wchar_t *out, size_t cap, DWORD pid) {
    int n = swprintf(out, cap, L"\\\\.\\pipe\\HopDesk.UI.v1.%lu", (unsigned long)pid);
    return n > 0 && (size_t)n < cap;
}
/* Explicit current-user ACL plus exact child PID and local-only transport. */
static PSECURITY_DESCRIPTOR user_security(void) {
    HANDLE token = NULL;
    DWORD bytes = 0;
    TOKEN_USER *user = NULL;
    LPWSTR sid = NULL;
    PSECURITY_DESCRIPTOR sd = NULL;
    wchar_t sddl[256];
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return NULL;
    GetTokenInformation(token, TokenUser, NULL, 0, &bytes);
    if (bytes && (user = (TOKEN_USER *)malloc(bytes)) &&
        GetTokenInformation(token, TokenUser, user, bytes, &bytes) &&
        ConvertSidToStringSidW(user->User.Sid, &sid)) {
        int n = swprintf(sddl, ARRAYSIZE(sddl), L"D:P(A;;GA;;;%ls)", sid);
        if (n > 0 && (size_t)n < ARRAYSIZE(sddl))
            ConvertStringSecurityDescriptorToSecurityDescriptorW(
                sddl, SDDL_REVISION_1, &sd, NULL);
    }
    if (sid) LocalFree(sid);
    free(user);
    CloseHandle(token);
    return sd;
}
/* Drain each cancelled OVERLAPPED before its stack storage dies. */
static int io_finish(HANDLE pipe, OVERLAPPED *ov, HANDLE stop, DWORD timeout,
                     DWORD *bytes) {
    HANDLE events[2] = {ov->hEvent, stop};
    DWORD w = WaitForMultipleObjects(stop ? 2 : 1, events, FALSE, timeout);
    if (w == WAIT_OBJECT_0)
        return GetOverlappedResult(pipe, ov, bytes, FALSE) != 0;
    CancelIoEx(pipe, ov);
    GetOverlappedResult(pipe, ov, bytes, TRUE);
    return 0;
}
static int io_exact(HANDLE pipe, void *buffer, DWORD size, int writing,
                    HANDLE stop, DWORD timeout) {
    unsigned char *p = (unsigned char *)buffer;
    ULONGLONG deadline = GetTickCount64() + timeout;
    OVERLAPPED ov = {0};
    int ok = 0;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    while (size) {
        DWORD n = 0;
        ULONGLONG now = GetTickCount64();
        if (now >= deadline || (stop && WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)) break;
        ResetEvent(ov.hEvent);
        BOOL done = writing ? WriteFile(pipe, p, size, &n, &ov)
                            : ReadFile(pipe, p, size, &n, &ov);
        if (!done && (GetLastError() != ERROR_IO_PENDING ||
            !io_finish(pipe, &ov, stop, (DWORD)(deadline - now), &n))) break;
        if (!n || n > size) break;
        size -= n;
        p += n;
    }
    ok = size == 0;
    CloseHandle(ov.hEvent);
    return ok;
}
static int accept_pipe(HANDLE pipe) {
    OVERLAPPED ov = {0};
    DWORD n = 0;
    int ok = 0;
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;
    if (ConnectNamedPipe(pipe, &ov)) ok = 1;
    else {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) ok = 1;
        else if (error == ERROR_IO_PENDING)
            ok = io_finish(pipe, &ov, s_stop, INFINITE, &n);
    }
    CloseHandle(ov.hEvent);
    return ok;
}

static const hop_xfer_task_snapshot *find_task(const hopdesk_ui_model *model,
                                                hop_xfer_task_id id) {
    int i;
    if (!model || !id) return NULL;
    for (i = 0; i < model->task_count; ++i)
        if (model->tasks[i].id == id) return &model->tasks[i];
    return NULL;
}

static void agent_snapshot(hopdesk_ui_model *model,
                           hop_xfer_task_id selected_task,
                           uint32_t file_offset) {
    uint64_t task_revision = 0, history_revision = 0, revision = 0;
    int total = 0, n = 0;
    const hop_xfer_task_snapshot *selected;

    if (!model) return;
    if (!s_agent_config_loaded) {
        s_agent_config = g_app.cfg;
        (void)hop_config_load(&s_agent_config, g_app.config_path);
        s_agent_config_loaded = 1;
    }
    s_agent_config.input_mode = g_app.cfg.input_mode;
    model->config = s_agent_config;
    shared_display_status(model->display_status, sizeof(model->display_status));
    shared_display_devices(model->display_devices, sizeof(model->display_devices));
    snprintf(model->local_addresses, sizeof(model->local_addresses), "%s",
        s_cached_addresses[0] ? s_cached_addresses : "No active LAN IPv4 address");

    model->connected = g_app.state != HOP_LINK_DOWN;
    model->sharing = g_app.state == HOP_LINK_SHARING;
    model->data_connected = xfer_connected();
    model->manually_disconnected = g_app.net && net_server_is_disconnected(g_app.net);
    model->peer_input_mode = g_app.peer_input_mode;
    model->peer_audio_mode = g_app.peer_audio_mode;
    model->peer_capabilities_known = g_app.peer_capabilities_known;
    model->protocol_mismatch = g_app.net ? net_server_protocol_mismatch(g_app.net) : 0;
    update_get_snapshot(&model->update);
    model->permissions_known = 0;
    if (model->connected && g_app.peer_name[0])
        snprintf(model->peer_label, sizeof(model->peer_label), "%s", g_app.peer_name);
    else if (model->config.role == HOP_ROLE_CLIENT)
        snprintf(model->peer_label, sizeof(model->peer_label), "%s",
            model->config.peer_host[0] ? model->config.peer_host : "对端地址未配置");
    else
        snprintf(model->peer_label, sizeof(model->peer_label), "%s",
            model->connected ? "已连接的对端设备" : (model->manually_disconnected ? "已手动断开" : "等待对端连接"));

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


/* Identical executable ABI; unified Core schema validation at boundary. */
static int valid_config(const hop_config *c, int strict) {
    return hop_config_validate(c, strict, NULL, 0);
}
static void ensure_agent_config_loaded(void) {
    if (!s_agent_config_loaded) {
        s_agent_config = g_app.cfg;
        (void)hop_config_load(&s_agent_config, g_app.config_path);
        s_agent_config_loaded = 1;
    }
    s_agent_config.input_mode = g_app.cfg.input_mode;
}
void win_split_agent_dispatch(void) {
    if (InterlockedCompareExchange(&s_pending, 2, 1) != 1) return;
    s_reply.result = 0;
    s_reply.bytes = 0;
    s_reply.page = -1;
    switch (s_request.op) {
    case IPC_SNAPSHOT:
        if (!s_agent_model) s_agent_model = (hopdesk_ui_model *)calloc(1, sizeof(*s_agent_model));
        if (s_agent_model && s_payload) {
            agent_snapshot(s_agent_model, s_request.task, s_request.offset);
            memcpy(s_payload, s_agent_model, WIN_SPLIT_MODEL_BYTES);
            s_reply.bytes = WIN_SPLIT_MODEL_BYTES;
            s_reply.result = 1;
            s_reply.page = s_pending_page;
            s_pending_page = -1;
        }
        break;
    case IPC_CLI_STATUS:
        if (s_payload) {
            hop_cli_status st;
            memset(&st, 0, sizeof(st));
            st.running = 1;
            st.agent_pid = (int)GetCurrentProcessId();
            st.connected = g_app.state != HOP_LINK_DOWN;
            st.sharing = g_app.state == HOP_LINK_SHARING;
            st.data_connected = xfer_connected();
            st.manually_disconnected = g_app.net && net_server_is_disconnected(g_app.net);
            st.input_mode = g_app.cfg.input_mode;
            st.peer_input_mode = g_app.peer_input_mode;
            st.peer_audio_mode = g_app.peer_audio_mode;
            st.peer_capabilities_known = g_app.peer_capabilities_known;
            st.role = g_app.cfg.role;
            {
                hop_audio_status audio = hop_audio_get_status();
                st.audio_requested = audio.requested;
                st.audio_active = audio.active;
                st.audio_unavailable = audio.unavailable;
            }
            {
                int total = 0;
                uint64_t rev = 0;
                (void)hop_xfer_task_list_revisioned(NULL, 0, &total, &rev);
                st.active_tasks = total;
            }
            if (st.connected && g_app.peer_name[0])
                snprintf(st.peer_label, sizeof(st.peer_label), "%s", g_app.peer_name);
            else if (g_app.cfg.role == HOP_ROLE_CLIENT && g_app.cfg.peer_host[0])
                snprintf(st.peer_label, sizeof(st.peer_label), "%s", g_app.cfg.peer_host);
            memcpy(s_payload, &st, sizeof(st));
            s_reply.bytes = (uint32_t)sizeof(st);
            s_reply.result = 1;
        }
        break;
    case IPC_CLI_CONFIG_GET:
        if (s_payload) {
            ensure_agent_config_loaded();
            memcpy(s_payload, &s_agent_config, sizeof(s_agent_config));
            s_reply.bytes = (uint32_t)sizeof(s_agent_config);
            s_reply.result = 1;
        }
        break;
    case IPC_SAVE:
        if (valid_config(&s_request.config, s_request.action != 0) &&
            hop_config_save_atomic(&s_request.config, g_app.config_path)) {
            s_agent_config = s_request.config;
            s_agent_config_loaded = 1;
            s_reply.result = win_apply_hot_config(&s_request.config) ? 1 : 2;
        }
        break;
    case IPC_CLI_RELOAD: {
        hop_config reloaded;
        hop_config_defaults(&reloaded);
        snprintf(reloaded.device_name, sizeof(reloaded.device_name), "%s", "Alex");
        reloaded.screen_w = g_app.cfg.screen_w;
        reloaded.screen_h = g_app.cfg.screen_h;
        if (hop_config_load(&reloaded, g_app.config_path)) {
            s_agent_config = reloaded;
            s_agent_config_loaded = 1;
            (void)win_apply_hot_config(&reloaded);
            s_reply.result = 1;
        }
        break;
    }
    case IPC_INPUT_MODE:
        if (s_request.action >= 0 && !((unsigned)s_request.action & ~HOP_INPUT_MODES)) {
            unsigned prev_input = g_app.cfg.input_mode;
            unsigned next_input = (unsigned)s_request.action;
            unsigned takeover = next_input & ~prev_input;
            if (!s_agent_config_loaded) {
                s_agent_config = g_app.cfg;
                (void)hop_config_load(&s_agent_config, g_app.config_path);
                s_agent_config_loaded = 1;
            }
            s_agent_config.input_mode = next_input;
            win_apply_input_mode(next_input, takeover, 1);
            s_reply.result = 1;
        }
        break;
    case IPC_THEME:
        if (s_request.action >= HOP_THEME_SYSTEM && s_request.action <= HOP_THEME_DARK) {
            if (!s_agent_config_loaded) {
                s_agent_config = g_app.cfg;
                (void)hop_config_load(&s_agent_config, g_app.config_path);
                s_agent_config_loaded = 1;
            }
            s_agent_config.ui_theme = s_request.action;
            g_app.cfg.ui_theme = s_request.action;
            s_reply.result = hop_config_save_atomic(&s_agent_config, g_app.config_path) ? 1 : 2;
        }
        break;
    case IPC_CANCEL: s_reply.result = xfer_cancel_task(s_request.task); break;
    case IPC_RETRY:
        if (s_request.index >= 0) s_reply.result = xfer_retry_file(s_request.batch, s_request.index);
        break;
    case IPC_RECONNECT:
        if (g_app.net) {
            net_server_set_disconnected(g_app.net, 0);
            net_server_drop(g_app.net);
        }
        xfer_kick();
        ui_refresh(&g_app);
        s_reply.result = 1;
        break;
    case IPC_DISCONNECT:
        if (g_app.net) net_server_set_disconnected(g_app.net, 1);
        xfer_kick();
        ui_refresh(&g_app);
        s_reply.result = 1;
        break;
    case IPC_DISPLAY:
        if (s_request.action >= HOPDESK_UI_DISPLAY_TO_MAC &&
            s_request.action <= HOPDESK_UI_DISPLAY_TOGGLE) {
            s_reply.result = win_ui_display_action(s_request.action) ? 1 : 0;
        }
        break;
    case IPC_UPDATE:
        if (s_request.action == HOP_UPDATE_ACTION_DOWNLOAD) {
            update_download_async(g_app.hwnd);
            s_reply.result = 1;
        } else if (s_request.action == HOP_UPDATE_ACTION_APPLY) {
            update_apply_async(g_app.hwnd);
            s_reply.result = 1;
        } else {
            int ch = s_request.index == HOP_UPDATE_CHANNEL_DEV
                ? HOP_UPDATE_CHANNEL_DEV : HOP_UPDATE_CHANNEL_STABLE;
            update_check_async(g_app.hwnd, ch);
            s_reply.result = 1;
        }
        break;
    default: break;
    }
    InterlockedExchange(&s_pending, 0);
    SetEvent(s_done);
}
static void serve_pipe_request(HANDLE pipe, const ipc_request *req, DWORD payload_cap) {
    unsigned char ack = 0;
    EnterCriticalSection(&s_ipc_lock);
    s_request = *req;
    s_payload = payload_cap ? malloc(payload_cap) : NULL;
    memset(&s_reply, 0, sizeof(s_reply));
    s_reply.version = WIN_SPLIT_VERSION;
    s_reply.op = s_request.op;
    s_reply.page = -1;
    ResetEvent(s_done);
    InterlockedExchange(&s_pending, 1);
    if (PostMessageW(s_owner, WM_APP_SPLIT_REQUEST, 0, 0)) {
        HANDLE events[2] = {s_stop, s_done};
        if (WaitForMultipleObjects(2, events, FALSE, INFINITE) == WAIT_OBJECT_0 + 1 &&
            io_exact(pipe, &s_reply, sizeof(s_reply), 1, s_stop, WIN_SPLIT_IO_MS) &&
            (!s_reply.bytes || io_exact(pipe, s_payload, s_reply.bytes, 1, s_stop, WIN_SPLIT_IO_MS)))
            (void)io_exact(pipe, &ack, 1, 0, s_stop, WIN_SPLIT_IO_MS);
    }
    InterlockedExchange(&s_pending, 0);
    free(s_payload);
    s_payload = NULL;
    LeaveCriticalSection(&s_ipc_lock);
}
static DWORD WINAPI agent_worker(LPVOID unused) {
    (void)unused;
    /* Pre-populate local IPv4 addresses off the agent message/hook thread. */
    local_addresses(s_cached_addresses, sizeof(s_cached_addresses));
    s_cached_addresses_tick = GetTickCount64();
    while (WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0) {
        ULONG client = 0;
        ipc_request req;
        int did_snapshot = 0;
        if (!accept_pipe(s_pipe)) {
            if (WaitForSingleObject(s_stop, 0) == WAIT_OBJECT_0) break;
            DisconnectNamedPipe(s_pipe);
            Sleep(20);
            continue;
        }
        if (!GetNamedPipeClientProcessId(s_pipe, &client) || !client ||
            client != (ULONG)InterlockedCompareExchange(&s_child_pid, 0, 0))
            goto disconnect;
        if (!io_exact(s_pipe, &req, sizeof(req), 0, s_stop, WIN_SPLIT_IO_MS) ||
            req.version != WIN_SPLIT_VERSION ||
            req.op < IPC_SNAPSHOT || req.op > IPC_THEME) goto disconnect;
        did_snapshot = req.op == IPC_SNAPSHOT;
        serve_pipe_request(s_pipe, &req, did_snapshot ? WIN_SPLIT_MODEL_BYTES : 0);
    disconnect:
        DisconnectNamedPipe(s_pipe);
        if (did_snapshot && WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0 &&
            GetTickCount64() - s_cached_addresses_tick >= WIN_SPLIT_ADDR_TTL_MS) {
            local_addresses(s_cached_addresses, sizeof(s_cached_addresses));
            s_cached_addresses_tick = GetTickCount64();
        }
    }
    return 0;
}
static DWORD WINAPI ctrl_worker(LPVOID unused) {
    (void)unused;
    while (WaitForSingleObject(s_stop, 0) != WAIT_OBJECT_0) {
        ipc_request req;
        DWORD payload_cap = 0;
        if (!accept_pipe(s_ctrl_pipe)) {
            if (WaitForSingleObject(s_stop, 0) == WAIT_OBJECT_0) break;
            DisconnectNamedPipe(s_ctrl_pipe);
            Sleep(20);
            continue;
        }
        if (!io_exact(s_ctrl_pipe, &req, sizeof(req), 0, s_stop, WIN_SPLIT_IO_MS) ||
            req.version != WIN_SPLIT_VERSION) goto disconnect;
        switch (req.op) {
        case IPC_CLI_STATUS:     payload_cap = (DWORD)sizeof(hop_cli_status); break;
        case IPC_CLI_CONFIG_GET: payload_cap = (DWORD)sizeof(hop_config); break;
        case IPC_SAVE:
        case IPC_CLI_RELOAD:
        case IPC_RECONNECT:
        case IPC_DISCONNECT:
        case IPC_DISPLAY:        payload_cap = 0; break;
        default: goto disconnect;
        }
        serve_pipe_request(s_ctrl_pipe, &req, payload_cap);
    disconnect:
        DisconnectNamedPipe(s_ctrl_pipe);
    }
    return 0;
}
int win_split_agent_start(HWND owner) {
    wchar_t name[100];
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, FALSE};
    s_owner = owner;
    if (!owner || !pipe_name(name, ARRAYSIZE(name), GetCurrentProcessId())) return 0;
    sa.lpSecurityDescriptor = user_security();
    if (!sa.lpSecurityDescriptor) return 0;
    s_pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &sa);
    s_ctrl_pipe = CreateNamedPipeW(WIN_CTRL_PIPE_NAME,
        PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, 4096, 4096, 0, &sa);
    LocalFree(sa.lpSecurityDescriptor);
    if (s_pipe == INVALID_HANDLE_VALUE) { win_split_agent_stop(); return 0; }
    InitializeCriticalSection(&s_ipc_lock);
    s_ipc_lock_init = 1;
    s_stop = CreateEventW(NULL, TRUE, FALSE, NULL);
    s_done = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (s_stop && s_done) {
        s_worker = CreateThread(NULL, 0, agent_worker, NULL, 0, NULL);
        if (s_ctrl_pipe != INVALID_HANDLE_VALUE)
            s_ctrl_worker = CreateThread(NULL, 0, ctrl_worker, NULL, 0, NULL);
    }
    if (!s_worker) { win_split_agent_stop(); return 0; }
    return 1;
}
void win_split_agent_reap(void) {
    if (s_child && WaitForSingleObject(s_child, 0) == WAIT_OBJECT_0) {
        InterlockedExchange(&s_child_pid, 0);
        CloseHandle(s_child); s_child = NULL;
        if (s_job) { CloseHandle(s_job); s_job = NULL; }
        free(s_agent_model); s_agent_model = NULL;
        s_pending_page = -1;
    }
}
static BOOL CALLBACK child_window(HWND hwnd, LPARAM closing) {
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid != (DWORD)InterlockedCompareExchange(&s_child_pid, 0, 0) || !pid) return TRUE;
    if (closing) PostMessageW(hwnd, WM_CLOSE, 0, 0);
    else if (IsWindowVisible(hwnd)) {
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        SetForegroundWindow(hwnd);
        PostMessageW(hwnd, WM_NULL, 0, 0);
    }
    return TRUE;
}
void win_split_agent_open(int page) {
    wchar_t exe[4096], command[4200];
    STARTUPINFOW si = {0};
    PROCESS_INFORMATION pi = {0};
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit = {0};
    DWORD len;
    if (!s_worker || page < 0 || page >= HOPDESK_UI_PAGE_COUNT) return;
    win_split_agent_reap();
    if (s_child) {
        DWORD pid = (DWORD)InterlockedCompareExchange(&s_child_pid, 0, 0);
        s_pending_page = page;
        if (pid) AllowSetForegroundWindow(pid);
        EnumWindows(child_window, 0);
        return;
    }
    len = GetModuleFileNameW(NULL, exe, ARRAYSIZE(exe));
    if (!len || len >= ARRAYSIZE(exe) ||
        swprintf(command, ARRAYSIZE(command), L"\"%ls\" --ui %lu %d",
                 exe, (unsigned long)GetCurrentProcessId(), page) < 0) goto failed;
    s_job = CreateJobObjectW(NULL, NULL);
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!s_job || !SetInformationJobObject(s_job, JobObjectExtendedLimitInformation,
                                          &limit, sizeof(limit))) goto failed;
    si.cb = sizeof(si);
    if (!CreateProcessW(exe, command, NULL, NULL, FALSE, CREATE_SUSPENDED,
                        NULL, NULL, &si, &pi)) goto failed;
    if (!AssignProcessToJobObject(s_job, pi.hProcess)) goto failed;
    s_child = pi.hProcess;
    InterlockedExchange(&s_child_pid, (LONG)pi.dwProcessId);
    AllowSetForegroundWindow(pi.dwProcessId);
    if (ResumeThread(pi.hThread) == (DWORD)-1) {
        s_child = NULL;
        InterlockedExchange(&s_child_pid, 0);
        goto failed;
    }
    CloseHandle(pi.hThread);
    return;
failed:
    if (pi.hProcess) { TerminateProcess(pi.hProcess, 1); CloseHandle(pi.hProcess); }
    if (pi.hThread) CloseHandle(pi.hThread);
    if (s_job) { CloseHandle(s_job); s_job = NULL; }
    MessageBoxW(s_owner, L"Unable to start the separate HopDesk UI.", L"HopDesk", MB_OK | MB_ICONERROR);
}
void win_split_agent_stop(void) {
    /* Only on main thread after its message loop has ended. */
    if (s_stop) SetEvent(s_stop);
    if (s_worker) { WaitForSingleObject(s_worker, INFINITE); CloseHandle(s_worker); s_worker = NULL; }
    if (s_ctrl_worker) { WaitForSingleObject(s_ctrl_worker, INFINITE); CloseHandle(s_ctrl_worker); s_ctrl_worker = NULL; }
    if (s_pipe != INVALID_HANDLE_VALUE) { CloseHandle(s_pipe); s_pipe = INVALID_HANDLE_VALUE; }
    if (s_ctrl_pipe != INVALID_HANDLE_VALUE) { CloseHandle(s_ctrl_pipe); s_ctrl_pipe = INVALID_HANDLE_VALUE; }
    if (s_stop) { CloseHandle(s_stop); s_stop = NULL; }
    if (s_done) { CloseHandle(s_done); s_done = NULL; }
    if (s_ipc_lock_init) { DeleteCriticalSection(&s_ipc_lock); s_ipc_lock_init = 0; }
    if (s_child) {
        EnumWindows(child_window, 1);
        if (WaitForSingleObject(s_child, 3000) != WAIT_OBJECT_0 && s_job) {
            CloseHandle(s_job); s_job = NULL;
            WaitForSingleObject(s_child, INFINITE);
        }
        CloseHandle(s_child); s_child = NULL;
    }
    if (s_job) { CloseHandle(s_job); s_job = NULL; }
    InterlockedExchange(&s_child_pid, 0);
    free(s_agent_model); s_agent_model = NULL;
    s_cached_addresses[0] = '\0';
    s_cached_addresses_tick = 0;
    s_pending_page = -1;
}
int win_split_client_init(DWORD pid) {
    if (!pid || pid == GetCurrentProcessId()) return 0;
    s_parent = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!s_parent) return 0;
    s_parent_pid = pid;
    return 1;
}
void win_split_client_close(void) {
    if (s_parent) CloseHandle(s_parent);
    s_parent = NULL;
    s_parent_pid = 0;
    free(s_client_payload);
    s_client_payload = NULL;
}
int win_split_client_active(void) { return s_parent != NULL; }
int win_split_client_alive(void) {
    return s_parent && WaitForSingleObject(s_parent, 0) == WAIT_TIMEOUT;
}
static int client_call(ipc_request *req, void *payload, DWORD capacity, int *page) {
    wchar_t name[100];
    HANDLE pipe;
    ipc_reply reply;
    ULONG server = 0;
    unsigned char ack = 1;
    int result = 0;
    DWORD timeout = req->op == IPC_SNAPSHOT ? WIN_SPLIT_IO_MS : WIN_SPLIT_ACTION_MS;
    if (!win_split_client_alive() || !pipe_name(name, ARRAYSIZE(name), s_parent_pid)) return 0;
    pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(name, timeout))
        pipe = CreateFileW(name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return 0;
    req->version = WIN_SPLIT_VERSION;
    if (GetNamedPipeServerProcessId(pipe, &server) && server == s_parent_pid &&
        io_exact(pipe, req, sizeof(*req), 1, s_parent, timeout) &&
        io_exact(pipe, &reply, sizeof(reply), 0, s_parent, timeout) &&
        reply.version == WIN_SPLIT_VERSION && reply.op == req->op &&
        reply.bytes <= capacity && (!reply.bytes || payload) &&
        (!reply.result || reply.bytes == capacity) &&
        (!reply.bytes || io_exact(pipe, payload, reply.bytes, 0, s_parent, timeout))) {
        result = reply.result;
        if (page) *page = reply.page;
        (void)io_exact(pipe, &ack, 1, 1, s_parent, timeout);
    }
    CloseHandle(pipe);
    return result;
}
int win_split_snapshot(hopdesk_ui_model *model, hop_xfer_task_id task, uint32_t offset, int *page) {
    ipc_request r = {0};
    int ok;
    if (!model) return 0;
    if (!s_client_payload) s_client_payload = malloc(WIN_SPLIT_MODEL_BYTES);
    if (!s_client_payload) return 0;
    r.op = IPC_SNAPSHOT; r.task = task; r.offset = offset;
    ok = client_call(&r, s_client_payload, WIN_SPLIT_MODEL_BYTES, page);
    if (ok) memcpy(model, s_client_payload, WIN_SPLIT_MODEL_BYTES);
    return ok;
}
int win_split_save(const hop_config *config) {
    ipc_request r = {0}; if (!config) return 0; r.op = IPC_SAVE; r.action = 1; r.config = *config;
    return client_call(&r, NULL, 0, NULL);
}
int win_split_cancel(hop_xfer_task_id task) {
    ipc_request r = {0}; r.op = IPC_CANCEL; r.task = task;
    return client_call(&r, NULL, 0, NULL);
}
int win_split_retry(uint32_t batch, int index) {
    ipc_request r = {0}; r.op = IPC_RETRY; r.batch = batch; r.index = index;
    return client_call(&r, NULL, 0, NULL);
}
int win_split_reconnect(void) {
    ipc_request r = {0}; r.op = IPC_RECONNECT; return client_call(&r, NULL, 0, NULL);
}
int win_split_disconnect(void) {
    ipc_request r = {0}; r.op = IPC_DISCONNECT; return client_call(&r, NULL, 0, NULL);
}
int win_split_display(int action) {
    ipc_request r = {0}; r.op = IPC_DISPLAY; r.action = action;
    return client_call(&r, NULL, 0, NULL);
}
int win_split_update(int action, int channel) {
    ipc_request r = {0}; r.op = IPC_UPDATE; r.action = action; r.index = channel;
    return client_call(&r, NULL, 0, NULL);
}
int win_split_set_theme(int ui_theme) {
    ipc_request r = {0}; r.op = IPC_THEME; r.action = ui_theme;
    return client_call(&r, NULL, 0, NULL);
}
int win_split_set_input_mode(unsigned input_mode) {
    ipc_request r = {0}; r.op = IPC_INPUT_MODE; r.action = (int32_t)(input_mode & HOP_INPUT_MODES);
    return client_call(&r, NULL, 0, NULL);
}

/* ---- Local CLI -> Agent control pipe client ---- */
static int ctrl_call(ipc_request *req, void *payload, DWORD capacity, int *reached) {
    HANDLE pipe;
    ipc_reply reply;
    unsigned char ack = 1;
    int result = 0;
    DWORD timeout = WIN_SPLIT_ACTION_MS;
    if (reached) *reached = 0;
    pipe = CreateFileW(WIN_CTRL_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                       FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY &&
        WaitNamedPipeW(WIN_CTRL_PIPE_NAME, timeout))
        pipe = CreateFileW(WIN_CTRL_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                           FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, NULL);
    if (pipe == INVALID_HANDLE_VALUE) return 0;
    if (reached) *reached = 1;
    req->version = WIN_SPLIT_VERSION;
    if (io_exact(pipe, req, sizeof(*req), 1, NULL, timeout) &&
        io_exact(pipe, &reply, sizeof(reply), 0, NULL, timeout) &&
        reply.version == WIN_SPLIT_VERSION && reply.op == req->op &&
        reply.bytes <= capacity && (!reply.bytes || payload) &&
        (!reply.result || reply.bytes == capacity) &&
        (!reply.bytes || io_exact(pipe, payload, reply.bytes, 0, NULL, timeout))) {
        result = reply.result;
        (void)io_exact(pipe, &ack, 1, 1, NULL, timeout);
    }
    CloseHandle(pipe);
    return result;
}
int win_cli_agent_get_status(void *ctx, hop_cli_status *out) {
    (void)ctx;
    if (!out) return 0;
    ipc_request r = {0};
    r.op = IPC_CLI_STATUS;
    return ctrl_call(&r, out, (DWORD)sizeof(*out), NULL) > 0 ? 1 : 0;
}
int win_cli_agent_get_config(void *ctx, hop_config *out) {
    (void)ctx;
    if (!out) return 0;
    ipc_request r = {0};
    r.op = IPC_CLI_CONFIG_GET;
    return ctrl_call(&r, out, (DWORD)sizeof(*out), NULL) > 0 ? 1 : 0;
}
int win_cli_agent_save_config(void *ctx, const hop_config *cfg) {
    (void)ctx;
    if (!cfg) return -1;
    ipc_request r = {0};
    int reached = 0;
    r.op = IPC_SAVE;
    r.action = 0; /* CLI allows incremental save; schema ranges/cross-fields still enforced */
    r.config = *cfg;
    int rc = ctrl_call(&r, NULL, 0, &reached);
    if (!reached) return 0;
    return rc > 0 ? 1 : -1;
}
int win_cli_agent_action(void *ctx, hop_cli_action action) {
    (void)ctx;
    ipc_request r = {0};
    int reached = 0;
    switch (action) {
    case HOP_CLI_ACTION_RECONNECT:     r.op = IPC_RECONNECT; break;
    case HOP_CLI_ACTION_DISCONNECT:    r.op = IPC_DISCONNECT; break;
    case HOP_CLI_ACTION_DISPLAY_MAC:    r.op = IPC_DISPLAY; r.action = HOPDESK_UI_DISPLAY_TO_MAC; break;
    case HOP_CLI_ACTION_DISPLAY_WIN:    r.op = IPC_DISPLAY; r.action = HOPDESK_UI_DISPLAY_TO_WINDOWS; break;
    case HOP_CLI_ACTION_DISPLAY_TOGGLE: r.op = IPC_DISPLAY; r.action = HOPDESK_UI_DISPLAY_TOGGLE; break;
    case HOP_CLI_ACTION_DISPLAY_WAKE:   r.op = IPC_DISPLAY; r.action = HOPDESK_UI_DISPLAY_WAKE; break;
    case HOP_CLI_ACTION_DISPLAY_SLEEP:  r.op = IPC_DISPLAY; r.action = HOPDESK_UI_DISPLAY_SLEEP; break;
    case HOP_CLI_ACTION_RELOAD:        r.op = IPC_CLI_RELOAD; break;
    default: return -1;
    }
    int rc = ctrl_call(&r, NULL, 0, &reached);
    if (!reached) return 0;
    return rc > 0 ? 1 : -1;
}
