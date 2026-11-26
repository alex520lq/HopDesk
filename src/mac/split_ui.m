#import <AppKit/AppKit.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "mac_app.h"
#include "split_ipc.h"
#include "split_ui.h"
#include "../platform/audio_link.h"

static BOOL s_splitUI;
static BOOL s_agentAvailable;
static split_ipc_status s_status;

static int wait_socket(int fd, short events, uint64_t deadline) {
    struct pollfd socket = {.fd = fd, .events = events};
    for (;;) {
        uint64_t now = hop_monotonic_ms();
        if (now >= deadline) return 0;
        int result = poll(&socket, 1, (int)(deadline - now));
        if (result >= 0) return result > 0 && (socket.revents & events);
        if (errno != EINTR) return 0;
    }
}

static int copy_socket(int fd, void *buffer, size_t size, int send_data, uint64_t deadline) {
    unsigned char *bytes = buffer;
    while (size > 0) {
        if (!wait_socket(fd, send_data ? POLLOUT : POLLIN, deadline)) return 0;
        ssize_t n = send_data ? write(fd, bytes, size) : read(fd, bytes, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return 0;
        bytes += n;
        size -= (size_t)n;
    }
    return 1;
}

static int connect_agent(uint64_t deadline) {
    struct sockaddr_un address = {0};
    address.sun_family = AF_UNIX;
    if (!split_ipc_socket_path(address.sun_path, sizeof(address.sun_path))) return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) ||
        fcntl(fd, F_SETFD, FD_CLOEXEC) || fcntl(fd, F_SETFL, O_NONBLOCK)) goto fail;
    if (connect(fd, (struct sockaddr *)&address, SUN_LEN(&address)) != 0) {
        if (errno != EINPROGRESS || !wait_socket(fd, POLLOUT, deadline)) goto fail;
        int error = 0;
        socklen_t size = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) goto fail;
    }
    uid_t uid;
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) == 0 && uid == geteuid()) return fd;
fail:
    close(fd);
    return -1;
}

static int open_exchange(split_ipc_operation operation, split_ipc_query *query,
                         uint64_t deadline, size_t *size) {
    int fd = connect_agent(deadline);
    if (fd < 0) return -1;
    split_ipc_header header = {htonl(SPLIT_IPC_VERSION), htonl((uint32_t)operation),
                               htonl(query ? sizeof(*query) : 0), htonl(1)};
    split_ipc_reply reply;
    if (copy_socket(fd, &header, sizeof(header), 1, deadline) &&
        (!query || copy_socket(fd, query, sizeof(*query), 1, deadline)) &&
        copy_socket(fd, &reply, sizeof(reply), 0, deadline) &&
        ntohl(reply.version) == SPLIT_IPC_VERSION && ntohl(reply.request_id) == 1 &&
        ntohl(reply.operation) == (uint32_t)operation &&
        ntohl(reply.payload_size) <= SPLIT_IPC_MAX_PAYLOAD) {
        *size = ntohl(reply.payload_size);
        return fd;
    }
    close(fd);
    return -1;
}

static int exchange(split_ipc_operation operation, split_ipc_query *query,
                    void *result, size_t result_size) {
    const uint64_t deadline = hop_monotonic_ms() + 250;
    size_t size = 0;
    int fd = open_exchange(operation, query, deadline, &size);
    if (fd < 0) return 0;
    int ok = size == result_size && copy_socket(fd, result, size, 0, deadline);
    close(fd);
    return ok;
}

static int exchange_list(split_ipc_operation operation, split_ipc_query *query,
                         void *result, size_t element_size, int *total, uint64_t *revision) {
    if (total) *total = 0;
    if (revision) *revision = 0;
    if (!s_agentAvailable) return 0;
    if (query->capacity < 0 || (query->capacity && !result)) return -1;

    const uint64_t deadline = hop_monotonic_ms() + 250;
    size_t size = 0;
    int fd = open_exchange(operation, query, deadline, &size);
    if (fd < 0) return -1;
    split_ipc_list rows = {0};
    int ok = size >= sizeof(rows) && copy_socket(fd, &rows, sizeof(rows), 0, deadline) &&
             rows.count >= 0 && rows.count <= query->capacity && rows.total >= rows.count;
    if (ok) {
        size_t bytes = (size_t)rows.count * element_size;
        ok = size - sizeof(rows) == bytes &&
             (!bytes || (result && copy_socket(fd, result, bytes, 0, deadline)));
    }
    close(fd);
    if (!ok) return -1;
    if (total) *total = rows.total;
    if (revision) *revision = rows.revision;
    return rows.count;
}

int split_ui_agent_available(void) {
    split_ipc_status status;
    return exchange(SPLIT_IPC_STATUS, NULL, &status, sizeof(status)) && status.pid > 0;
}

int split_ui_is_active(void) { return s_splitUI; }

void split_ui_refresh_connection(int *connected, int *sharing) {
    if (!s_splitUI) return;
    s_status = (split_ipc_status){0};
    s_agentAvailable = exchange(SPLIT_IPC_STATUS, NULL, &s_status, sizeof(s_status));
    *connected = s_agentAvailable && s_status.control;
    *sharing = s_agentAvailable && s_status.sharing;
}

int split_ui_get_input_permissions(int *post, int *listen, int *accessibility) {
    if (!s_splitUI || !s_agentAvailable) return 0;
    *post = s_status.post_permission;
    *listen = s_status.listen_permission;
    *accessibility = s_status.accessibility;
    return 1;
}

int split_ui_data_connected(void) {
    return s_splitUI ? s_agentAvailable && s_status.data : xfer_connected();
}

int split_ui_get_transfer_state(split_ipc_transfer_state *out) {
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (!s_splitUI) {
        xfer_get_status_pair(&out->status);
        hop_xfer_task_get_revisions(&out->task_revision, &out->history_revision);
        return 1;
    }
    return s_agentAvailable && exchange(SPLIT_IPC_XFER_STATUS, NULL, out, sizeof(*out));
}

void split_ui_get_status_pair(hop_xfer_status_pair *out) {
    if (!out) return;
    split_ipc_transfer_state state = {0};
    if (split_ui_get_transfer_state(&state)) *out = state.status;
    else memset(out, 0, sizeof(*out));
}

int split_ui_get_tasks(hop_xfer_task_snapshot *out, int capacity, int *total, uint64_t *revision) {
    if (!s_splitUI)
        return hop_xfer_task_list_revisioned(out, capacity, total, revision);
    split_ipc_query query = {.capacity = capacity};
    return exchange_list(SPLIT_IPC_TASKS_GET, &query, out, sizeof(*out), total, revision);
}

int split_ui_get_task(hop_xfer_task_id id, hop_xfer_task_snapshot *out) {
    if (!s_splitUI) return hop_xfer_task_get_snapshot(id, out);
    split_ipc_query query = {.id = id};
    return s_agentAvailable && exchange(SPLIT_IPC_TASK_GET, &query, out, sizeof(*out));
}

int split_ui_get_history(hop_xfer_task_history_record *out, int capacity, int direction,
                         hop_xfer_task_state state, int newest, int *total, uint64_t *revision) {
    if (!s_splitUI) {
        for (int attempt = 0; attempt < 3; ++attempt) {
            uint64_t before = 0, after = 0;
            hop_xfer_task_get_revisions(NULL, &before);
            int n=hop_xfer_task_history_store_list(out, capacity, direction, state, newest, total);
            if (n < 0) return n;
            hop_xfer_task_get_revisions(NULL, &after);
            if (before == after) {
                if (revision) *revision = after;
                return n;
            }
        }
        return -1;
    }
    split_ipc_query query = {.capacity = capacity, .direction = direction, .state = state, .newest = newest};
    return exchange_list(SPLIT_IPC_HISTORY_GET, &query, out, sizeof(*out), total, revision);
}

int split_ui_get_files(int direction, uint32_t batch, uint32_t offset,
                       hop_xfer_file_status *out, int capacity, int *total, uint64_t *revision) {
    if (!s_splitUI) return xfer_get_files_page(direction, batch, offset, out, capacity, total, revision);
    split_ipc_query query = {.capacity = capacity, .direction = direction, .batch = batch, .offset = (int32_t)offset};
    return exchange_list(SPLIT_IPC_FILES_GET, &query, out, sizeof(*out), total, revision);
}

static int action(split_ipc_operation operation, split_ipc_query *query) {
    int32_t result = 0;
    return exchange(operation, query, &result, sizeof(result)) && result;
}

int split_ui_set_audio_mode(unsigned mode) {
    if (mode & ~HOP_AUDIO_MODES) return 0;
    if (!s_splitUI) {
        g_mac.cfg.audio_mode = mode;
        hop_config runtime = g_mac.cfg;
        return hop_audio_set_mode(&runtime, mode);
    }
    g_mac.cfg.audio_mode = mode;
    split_ipc_query query = {.index = (int32_t)mode};
    return action(SPLIT_IPC_AUDIO_MODE_SET, &query);
}

void split_ui_get_audio_status(hop_audio_status *out) {
    if (!out) return;
    if (!s_splitUI) {
        *out = hop_audio_get_status();
        if (!out->requested && g_mac.cfg.audio_mode) {
            out->requested = g_mac.cfg.audio_mode;
            out->unavailable = g_mac.cfg.audio_mode;
        }
        return;
    }
    *out = (hop_audio_status){0};
    if (s_agentAvailable) {
        out->requested = (unsigned)s_status.audio_requested;
        out->active = (unsigned)s_status.audio_active;
        out->unavailable = (unsigned)s_status.audio_unavailable;
        if (!out->requested && g_mac.cfg.audio_mode) {
            out->requested = g_mac.cfg.audio_mode;
            out->unavailable = g_mac.cfg.audio_mode;
        }
    } else {
        out->requested = g_mac.cfg.audio_mode;
        out->unavailable = g_mac.cfg.audio_mode;
    }
}

int split_ui_cancel_task(hop_xfer_task_id id) {
    if (!s_splitUI) return xfer_cancel_task(id);
    split_ipc_query query = {.id = id};
    return action(SPLIT_IPC_CANCEL_TASK, &query);
}

int split_ui_retry_file(uint32_t batch, int index) {
    if (!s_splitUI) return xfer_retry_file(batch, index);
    split_ipc_query query = {.batch = batch, .index = index};
    return action(SPLIT_IPC_RETRY_FILE, &query);
}

static void show_message(NSString *title, NSString *detail) {
    NSAlert *alert = [NSAlert new];
    alert.messageText = title;
    alert.informativeText = detail;
    [alert runModal];
}

void split_ui_reconnect(void) {
    if (!s_splitUI) {
        net_request_reconnect();
        return;
    }
    if (!action(SPLIT_IPC_RECONNECT, NULL))
        show_message(@"后台未响应", @"请确认 HopDesk 后台正在运行，再尝试重新连接。");
}

static BOOL launchctl(NSArray<NSString *> *arguments) {
    NSTask *task = [NSTask new];
    task.executableURL = [NSURL fileURLWithPath:@"/bin/launchctl"];
    task.arguments = arguments;
    if (![task launchAndReturnError:NULL]) return NO;
    [task waitUntilExit];
    return task.terminationStatus == 0;
}

typedef struct {
    uint64_t id;
    int32_t capacity, direction, state, newest, index;
    uint32_t batch;
} legacy_split_ipc_query;
typedef struct { int32_t count, total; } legacy_split_ipc_list;

static int legacy_open_exchange(split_ipc_operation operation, const void *query,
                                size_t query_size, uint64_t deadline, size_t *payload_size) {
    int fd = connect_agent(deadline);
    if (fd < 0) return -1;
    split_ipc_header header = {htonl(2u), htonl((uint32_t)operation),
        htonl((uint32_t)query_size), htonl(1)};
    split_ipc_reply reply;
    if (copy_socket(fd, &header, sizeof(header), 1, deadline) &&
        (!query_size || copy_socket(fd, (void *)query, query_size, 1, deadline)) &&
        copy_socket(fd, &reply, sizeof(reply), 0, deadline) &&
        ntohl(reply.version) == 2u && ntohl(reply.request_id) == 1u &&
        ntohl(reply.operation) == (uint32_t)operation &&
        ntohl(reply.payload_size) <= SPLIT_IPC_MAX_PAYLOAD) {
        *payload_size = ntohl(reply.payload_size);
        return fd;
    }
    close(fd);
    return -1;
}

/* A newly updated UI can find an older launch-agent still running. Probe its
 * v2 task list before restarting it; never kill an agent that still owns jobs. */
static int legacy_agent_available(void) {
    uint64_t deadline = hop_monotonic_ms() + 300;
    size_t size = 0;
    int fd = legacy_open_exchange(SPLIT_IPC_STATUS, NULL, 0, deadline, &size);
    if (fd < 0) return 0;
    split_ipc_status status = {0};
    int ok = size == sizeof(status) && copy_socket(fd, &status, sizeof(status), 0, deadline);
    close(fd);
    return ok && status.pid > 0;
}

static int legacy_agent_has_active_tasks(void) {
    legacy_split_ipc_query query = {.capacity = HOP_TASK_MANAGER_MAX_LIVE};
    uint64_t deadline = hop_monotonic_ms() + 500;
    size_t size = 0;
    int fd = legacy_open_exchange(SPLIT_IPC_TASKS_GET, &query, sizeof(query), deadline, &size);
    if (fd < 0 || size < sizeof(legacy_split_ipc_list)) {
        if (fd >= 0) close(fd);
        return -1;
    }
    legacy_split_ipc_list list = {0};
    int ok = copy_socket(fd, &list, sizeof(list), 0, deadline);
    if (!ok || list.count < 0 || list.total < list.count ||
        size != sizeof(list) + (size_t)list.count * sizeof(hop_xfer_task_snapshot)) {
        close(fd);
        return -1;
    }
    hop_xfer_task_snapshot *tasks = list.count ?
        calloc((size_t)list.count, sizeof(*tasks)) : NULL;
    if (list.count && !tasks) { close(fd); return -1; }
    ok = !list.count || copy_socket(fd, tasks,
        (size_t)list.count * sizeof(*tasks), 0, deadline);
    close(fd);
    if (!ok) { free(tasks); return -1; }
    int active = 0;
    for (int i = 0; i < list.count; ++i) {
        if (tasks[i].state < HOP_XFER_TASK_COMPLETED) { active = 1; break; }
    }
    free(tasks);
    return active;
}

static BOOL legacy_agent_stop_if_idle(void) {
    uint64_t deadline = hop_monotonic_ms() + 500;
    size_t size = 0;
    int fd = legacy_open_exchange(SPLIT_IPC_STOP, NULL, 0, deadline, &size);
    if (fd < 0 || size != sizeof(split_ipc_result)) {
        if (fd >= 0) close(fd);
        return NO;
    }
    split_ipc_result result = {0};
    int ok = copy_socket(fd, &result, sizeof(result), 0, deadline);
    close(fd);
    return ok && result.result;
}

static BOOL start_agent_job(void) {
    NSString *directory = [NSFileManager.defaultManager
        stringWithFileSystemRepresentation:g_mac.config_path length:strlen(g_mac.config_path)].stringByDeletingLastPathComponent;
    NSString *executable = NSBundle.mainBundle.executablePath;
    if (!directory.length || !executable.length) return NO;
    NSString *file = [directory stringByAppendingPathComponent:@"split-agent.plist"];
    NSDictionary *definition = @{
        @"Label": @"com.hopdesk.split-agent",
        @"ProgramArguments": @[executable, @"--agent"],
        @"AssociatedBundleIdentifiers": @[@"com.hopdesk.app"],
        @"RunAtLoad": @YES
    };
    NSData *data = [NSPropertyListSerialization dataWithPropertyList:definition
        format:NSPropertyListXMLFormat_v1_0 options:0 error:NULL];
    if (![data writeToFile:file options:NSDataWritingAtomic error:NULL]) return NO;
    chmod(file.fileSystemRepresentation, 0600);
    NSString *domain = [NSString stringWithFormat:@"gui/%u", getuid()];
    return launchctl(@[@"bootstrap", domain, file]) ||
           launchctl(@[@"kickstart", [domain stringByAppendingString:@"/com.hopdesk.split-agent"]]);
}

@interface HopDeskSplitDelegate : NSObject <NSApplicationDelegate>
- (void)stopAgent:(id)sender;
- (void)requestInputAccess:(id)sender;
@end

@implementation HopDeskSplitDelegate
- (BOOL)applicationShouldTerminateAfterLastWindowClosed:(NSApplication *)app {
    (void)app;
    return YES;
}
- (BOOL)applicationShouldHandleReopen:(NSApplication *)app hasVisibleWindows:(BOOL)visible {
    (void)app;
    if (!visible) statusbar_show_main();
    return YES;
}
- (void)stopAgent:(id)sender {
    (void)sender;
    if (!action(SPLIT_IPC_STOP, NULL)) {
        show_message(@"无法停止后台", @"如果仍有传输任务，请在传输页取消，或等待传输结束后重试。");
        return;
    }
    [NSApp.keyWindow performClose:nil];
}
- (void)requestInputAccess:(id)sender {
    (void)sender;
    mac_request_accessibility_permission();
}
@end

static void configure_menu(HopDeskSplitDelegate *delegate) {
    NSMenu *menu = [NSMenu new];
    NSMenuItem *appItem = [NSMenuItem new];
    NSMenuItem *editItem = [NSMenuItem new];
    [menu addItem:appItem];
    [menu addItem:editItem];

    NSMenu *appMenu = [[NSMenu alloc] initWithTitle:@"HopDesk"];
    [appMenu addItemWithTitle:@"设置键鼠权限…" action:@selector(requestInputAccess:) keyEquivalent:@""].target = delegate;
    [appMenu addItemWithTitle:@"停止 HopDesk 后台" action:@selector(stopAgent:) keyEquivalent:@""].target = delegate;
    [appMenu addItem:[NSMenuItem separatorItem]];
    [appMenu addItemWithTitle:@"关闭 HopDesk 窗口" action:@selector(performClose:) keyEquivalent:@"w"];
    [appMenu addItemWithTitle:@"退出 HopDesk 界面" action:@selector(performClose:) keyEquivalent:@"q"];
    appItem.submenu = appMenu;

    NSMenu *edit = [[NSMenu alloc] initWithTitle:@"编辑"];
    [edit addItemWithTitle:@"撤销" action:@selector(undo:) keyEquivalent:@"z"];
    [edit addItem:[NSMenuItem separatorItem]];
    [edit addItemWithTitle:@"剪切" action:@selector(cut:) keyEquivalent:@"x"];
    [edit addItemWithTitle:@"复制" action:@selector(copy:) keyEquivalent:@"c"];
    [edit addItemWithTitle:@"粘贴" action:@selector(paste:) keyEquivalent:@"v"];
    [edit addItemWithTitle:@"全选" action:@selector(selectAll:) keyEquivalent:@"a"];
    editItem.submenu = edit;
    NSApp.mainMenu = menu;
}

int split_ui_run(int start_agent) {
    @autoreleasepool {
        char directory[PATH_MAX], lock_path[PATH_MAX];
        size_t length = confstr(_CS_DARWIN_USER_TEMP_DIR, directory, sizeof(directory));
        if (!length || length > sizeof(directory) ||
            snprintf(lock_path, sizeof(lock_path), "%shopdesk-split-ui.lock", directory) >= (int)sizeof(lock_path))
            return 1;
        int lock = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (lock < 0) return 1;
        if (flock(lock, LOCK_EX | LOCK_NB) != 0) {
            close(lock);
            return 0;
        }
        /* Retain the lock until AppKit exits so settings have one writer. */
        NSApplication *app = [NSApplication sharedApplication];

        if (start_agent && !split_ui_agent_available()) {
            int legacy_tasks = legacy_agent_available() ? legacy_agent_has_active_tasks() : -2;
            if (legacy_tasks > 0 || legacy_tasks == -1) {
                show_message(@"后台需要安全更新",
                    legacy_tasks > 0 ?
                    @"检测到旧版后台仍在执行传输任务。为避免中断文件传输，请等待任务完成后重新打开 HopDesk。" :
                    @"检测到旧版后台，但暂时无法确认它是否仍拥有传输任务。为避免数据丢失，请先从旧版 HopDesk 确认所有任务结束，再重新打开。");
                close(lock);
                return 1;
            }
            if (legacy_tasks == 0) {
                if (!legacy_agent_stop_if_idle()) {
                    show_message(@"后台暂时无法更新",
                        @"旧版后台拒绝停止，可能仍有任务或连接正在收尾。请稍后重新打开 HopDesk。");
                    close(lock);
                    return 1;
                }
                uint64_t stop_deadline = hop_monotonic_ms() + 2000;
                while (legacy_agent_available() && hop_monotonic_ms() < stop_deadline)
                    usleep(50000);
                if (legacy_agent_available()) {
                    show_message(@"后台正在退出",
                        @"旧版后台尚未完全退出。请稍后重新打开 HopDesk。");
                    close(lock);
                    return 1;
                }
            }
            if (start_agent_job()) {
                uint64_t deadline = hop_monotonic_ms() + 4000;
                while (!split_ui_agent_available() && hop_monotonic_ms() < deadline)
                    usleep(50000);
            }
        }
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        s_splitUI = YES;
        if (!exchange(SPLIT_IPC_CONFIG_GET, NULL, &g_mac.cfg, sizeof(g_mac.cfg))) {
            [app activateIgnoringOtherApps:YES];
            show_message(@"无法连接 HopDesk 后台",
                         @"如果单进程 HopDesk 仍在运行，请先从菜单栏退出，再启动双进程模式。");
            close(lock);
            return 1;
        }

        __attribute__((objc_precise_lifetime)) HopDeskSplitDelegate *delegate = [HopDeskSplitDelegate new];
        app.delegate = delegate;
        configure_menu(delegate);
        statusbar_show_main();
        int connected = 0, sharing = 0;
        split_ui_refresh_connection(&connected, &sharing);
        mac_check_input_permissions();
        [app run];
        close(lock);
    }
    return 0;
}
