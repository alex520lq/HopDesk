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
                         void *result, size_t element_size, int *total) {
    if (total) *total = 0;
    if (!s_agentAvailable || !result || query->capacity <= 0) return 0;

    const uint64_t deadline = hop_monotonic_ms() + 250;
    size_t size = 0;
    int fd = open_exchange(operation, query, deadline, &size);
    if (fd < 0) return -1;
    split_ipc_list rows = {0};
    int ok = size >= sizeof(rows) && copy_socket(fd, &rows, sizeof(rows), 0, deadline) &&
             rows.count >= 0 && rows.count <= query->capacity && rows.total >= rows.count;
    if (ok) {
        size_t bytes = (size_t)rows.count * element_size;
        ok = size - sizeof(rows) == bytes && copy_socket(fd, result, bytes, 0, deadline);
    }
    close(fd);
    if (!ok) return -1;
    if (total) *total = rows.total;
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

int split_ui_data_connected(void) {
    return s_splitUI ? s_agentAvailable && s_status.data : xfer_connected();
}

void split_ui_get_status_pair(hop_xfer_status_pair *out) {
    if (!s_splitUI) {
        xfer_get_status_pair(out);
        return;
    }
    memset(out, 0, sizeof(*out));
    if (s_agentAvailable) (void)exchange(SPLIT_IPC_XFER_STATUS, NULL, out, sizeof(*out));
}

int split_ui_get_tasks(hop_xfer_task_snapshot *out, int capacity, int *total) {
    if (!s_splitUI) return hop_xfer_task_list(out, capacity, total);
    split_ipc_query query = {.capacity = capacity};
    return exchange_list(SPLIT_IPC_TASKS_GET, &query, out, sizeof(*out), total);
}

int split_ui_get_task(hop_xfer_task_id id, hop_xfer_task_snapshot *out) {
    if (!s_splitUI) return hop_xfer_task_get_snapshot(id, out);
    split_ipc_query query = {.id = id};
    return s_agentAvailable && exchange(SPLIT_IPC_TASK_GET, &query, out, sizeof(*out));
}

int split_ui_get_history(hop_xfer_task_history_record *out, int capacity, int direction,
                         hop_xfer_task_state state, int newest, int *total) {
    if (!s_splitUI) return hop_xfer_task_history_store_list(out, capacity, direction, state, newest, total);
    split_ipc_query query = {.capacity = capacity, .direction = direction, .state = state, .newest = newest};
    return exchange_list(SPLIT_IPC_HISTORY_GET, &query, out, sizeof(*out), total);
}

int split_ui_get_files(int direction, uint32_t batch, hop_xfer_file_status *out, int capacity) {
    if (!s_splitUI) return xfer_get_files(direction, batch, out, capacity);
    split_ipc_query query = {.capacity = capacity, .direction = direction, .batch = batch};
    int count = exchange_list(SPLIT_IPC_FILES_GET, &query, out, sizeof(*out), NULL);
    return count > 0 ? count : 0;
}

static int action(split_ipc_operation operation, split_ipc_query *query) {
    int32_t result = 0;
    return exchange(operation, query, &result, sizeof(result)) && result;
}

int split_ui_set_audio_mode(unsigned mode) {
    if (mode & ~HOP_AUDIO_MODES) return 0;
    if (!s_splitUI) {
        hop_config runtime = g_mac.cfg;
        runtime.audio_mode = mode;
        if (!hop_audio_set_mode(&runtime, mode)) return 0;
        g_mac.cfg.audio_mode = mode;
        return 1;
    }
    g_mac.cfg.audio_mode = mode;
    split_ipc_query query = {.index = (int32_t)mode};
    return action(SPLIT_IPC_AUDIO_MODE_SET, &query);
}

void split_ui_get_audio_status(hop_audio_status *out) {
    if (!out) return;
    if (!s_splitUI) {
        *out = hop_audio_get_status();
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

        if (start_agent && !split_ui_agent_available() && start_agent_job()) {
            uint64_t deadline = hop_monotonic_ms() + 4000;
            while (!split_ui_agent_available() && hop_monotonic_ms() < deadline)
                usleep(50000);
        }
        NSApplication *app = [NSApplication sharedApplication];
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
        [app run];
        close(lock);
    }
    return 0;
}
