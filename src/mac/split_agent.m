#import <ApplicationServices/ApplicationServices.h>
#import <AppKit/AppKit.h>
#import <CoreFoundation/CoreFoundation.h>
#import <dispatch/dispatch.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include "mac_app.h"
#include "split_agent.h"
#include "split_ipc.h"

static int s_listener = -1;
static struct sockaddr_un s_address;
static dispatch_source_t s_source, s_sigterm;
static dispatch_queue_t s_io_queue;
static _Atomic unsigned int s_connection;
static _Atomic bool s_stopping;
static bool s_active;

int split_agent_is_active(void) { return s_active; }

void split_agent_set_status(int connected, int sharing) {
    atomic_store(&s_connection, (connected ? 1u : 0u) | (sharing ? 2u : 0u));
}

static bool configure_socket(int fd) {
    int yes = 1;
    return fcntl(fd, F_SETFD, FD_CLOEXEC) == 0 &&
           fcntl(fd, F_SETFL, O_NONBLOCK) == 0 &&
           setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &yes, sizeof(yes)) == 0;
}

static bool io_full(int fd, void *buffer, size_t size, bool writing, uint64_t deadline) {
    unsigned char *bytes = buffer;
    short events = writing ? POLLOUT : POLLIN;
    while (size) {
        uint64_t now = hop_monotonic_ms();
        if (now >= deadline) return false;
        struct pollfd socket = {.fd = fd, .events = events};
        int ready = poll(&socket, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(socket.revents & events)) return false;
        ssize_t n = writing ? write(fd, bytes, size) : read(fd, bytes, size);
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (n <= 0) return false;
        bytes += n;
        size -= (size_t)n;
    }
    return true;
}

static bool reply(int fd, const split_ipc_header *request, bool success,
                  const void *prefix, size_t prefix_size, const void *rows, size_t rows_size,
                  uint64_t deadline) {
    const uint32_t operation = ntohl(request->operation);
    const size_t length = prefix_size + rows_size;
    if (length > SPLIT_IPC_MAX_PAYLOAD) return false;
    split_ipc_reply header = {
        .version = htonl(SPLIT_IPC_VERSION),
        .operation = htonl(success ? operation : operation | SPLIT_IPC_ERROR),
        .payload_size = htonl((uint32_t)length),
        .request_id = request->request_id
    };
    return io_full(fd, &header, sizeof(header), true, deadline) &&
           io_full(fd, (void *)prefix, prefix_size, true, deadline) &&
           io_full(fd, (void *)rows, rows_size, true, deadline);
}

static bool reply_list(int fd, const split_ipc_header *request, const split_ipc_query *q,
                       uint64_t deadline) {
    const uint32_t operation = ntohl(request->operation);
    size_t item_size, max_items;
    if (operation == SPLIT_IPC_TASKS_GET) {
        item_size = sizeof(hop_xfer_task_snapshot);
        max_items = HOP_TASK_MANAGER_MAX_LIVE;
    } else if (operation == SPLIT_IPC_HISTORY_GET) {
        item_size = sizeof(hop_xfer_task_history_record);
        max_items = HOP_TASK_HISTORY_CAPACITY;
    } else {
        item_size = sizeof(hop_xfer_file_status);
        max_items = HOP_XFER_FILE_LIMIT;
    }
    if (q->capacity <= 0 || (size_t)q->capacity > max_items ||
        (size_t)q->capacity > (SPLIT_IPC_MAX_PAYLOAD - sizeof(split_ipc_list)) / item_size)
        return false;
    void *items = calloc((size_t)q->capacity, item_size);
    if (!items) return false;
    int total = 0, count;
    if (operation == SPLIT_IPC_TASKS_GET) {
        count = hop_xfer_task_list(items, q->capacity, &total);
    } else if (operation == SPLIT_IPC_HISTORY_GET) {
        count = hop_xfer_task_history_store_list(items, q->capacity, q->direction,
                                                 (hop_xfer_task_state)q->state, q->newest, &total);
    } else {
        count = xfer_get_files(q->direction, q->batch, items, q->capacity);
        total = count;
    }
    bool valid = count >= 0 && count <= q->capacity && total >= count;
    if (valid) {
        split_ipc_list list = {.count = count, .total = total};
        (void)reply(fd, request, true, &list, sizeof(list), items, (size_t)count * item_size, deadline);
    }
    free(items);
    return valid;
}

static void stop_run_loop(void) {
    atomic_store(&s_stopping, true);
    [NSApp stop:nil];
    [NSApp postEvent:[NSEvent otherEventWithType:NSEventTypeApplicationDefined
        location:NSZeroPoint modifierFlags:0 timestamp:0 windowNumber:0 context:nil
        subtype:0 data1:0 data2:0] atStart:YES];
}

int split_agent_stop_if_idle(void) {
    if (xfer_busy()) return 0;
    stop_run_loop();
    return 1;
}

static void serve(int fd) {
    uid_t uid;
    gid_t gid;
    if (getpeereid(fd, &uid, &gid) != 0 || uid != geteuid()) return;

    uint64_t deadline = hop_monotonic_ms() + SPLIT_IPC_TIMEOUT_MS;
    split_ipc_header header;
    if (!io_full(fd, &header, sizeof(header), false, deadline) ||
        ntohl(header.version) != SPLIT_IPC_VERSION) return;
    uint32_t operation = ntohl(header.operation);
    if (operation < SPLIT_IPC_STATUS || operation > SPLIT_IPC_STOP) return;

    split_ipc_query query = {0};
    bool has_query = (operation >= SPLIT_IPC_TASKS_GET && operation <= SPLIT_IPC_FILES_GET) ||
                     operation == SPLIT_IPC_RETRY_FILE || operation == SPLIT_IPC_CANCEL_TASK;
    if (ntohl(header.payload_size) != (has_query ? sizeof(query) : 0)) {
        (void)reply(fd, &header, false, NULL, 0, NULL, 0, deadline);
        return;
    }
    if (has_query && !io_full(fd, &query, sizeof(query), false, deadline)) return;
    switch (operation) {
    case SPLIT_IPC_STATUS: {
        unsigned int connection = atomic_load(&s_connection);
        split_ipc_status status = {
            .pid = getpid(),
            .control = (connection & 1u) != 0,
            .data = xfer_connected(),
            .sharing = (connection & 2u) != 0,
            .post_permission = CGPreflightPostEventAccess(),
            .listen_permission = CGPreflightListenEventAccess(),
            .accessibility = AXIsProcessTrusted(),
            .busy = xfer_busy()
        };
        (void)reply(fd, &header, true, &status, sizeof(status), NULL, 0, deadline);
        return;
    }
    case SPLIT_IPC_CONFIG_GET:
        (void)reply(fd, &header, true, &g_mac.cfg, sizeof(g_mac.cfg), NULL, 0, deadline);
        return;
    case SPLIT_IPC_XFER_STATUS: {
        hop_xfer_status_pair status = {0};
        xfer_get_status_pair(&status);
        (void)reply(fd, &header, true, &status, sizeof(status), NULL, 0, deadline);
        return;
    }
    case SPLIT_IPC_TASKS_GET:
    case SPLIT_IPC_HISTORY_GET:
    case SPLIT_IPC_FILES_GET:
        if (reply_list(fd, &header, &query, deadline)) return;
        break;
    case SPLIT_IPC_TASK_GET: {
        hop_xfer_task_snapshot task = {0};
        if (!hop_xfer_task_get_snapshot(query.id, &task)) break;
        (void)reply(fd, &header, true, &task, sizeof(task), NULL, 0, deadline);
        return;
    }
    case SPLIT_IPC_RECONNECT:
    case SPLIT_IPC_RETRY_FILE:
    case SPLIT_IPC_CANCEL_TASK:
    case SPLIT_IPC_STOP: {
        split_ipc_result result = {0};
        if (operation == SPLIT_IPC_RECONNECT) {
            net_request_reconnect();
            result.result = 1;
        } else if (operation == SPLIT_IPC_RETRY_FILE) {
            result.result = xfer_retry_file(query.batch, query.index);
        } else if (operation == SPLIT_IPC_CANCEL_TASK) {
            result.result = xfer_cancel_task(query.id);
        } else {
            result.result = !xfer_busy();
        }
        if (reply(fd, &header, true, &result, sizeof(result), NULL, 0, deadline) &&
            operation == SPLIT_IPC_STOP && result.result) {
            atomic_store(&s_stopping, true);
            dispatch_async(dispatch_get_main_queue(), ^{ stop_run_loop(); });
        }
        return;
    }
    }
    (void)reply(fd, &header, false, NULL, 0, NULL, 0, deadline);
}

static bool clear_stale_socket(void) {
    struct stat st;
    if (lstat(s_address.sun_path, &st)) return errno == ENOENT;
    if (!S_ISSOCK(st.st_mode) || st.st_uid != geteuid()) return false;
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    bool stale = configure_socket(fd) &&
        connect(fd, (struct sockaddr *)&s_address, SUN_LEN(&s_address)) != 0 && errno == ECONNREFUSED;
    close(fd);
    return stale && unlink(s_address.sun_path) == 0;
}

int split_agent_start(void) {
    s_address.sun_family = AF_UNIX;
    if (!split_ipc_socket_path(s_address.sun_path, sizeof(s_address.sun_path)) ||
        !clear_stale_socket()) return 0;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    if (!configure_socket(fd) ||
        bind(fd, (struct sockaddr *)&s_address, SUN_LEN(&s_address)) != 0) {
        close(fd);
        return 0;
    }
    if (chmod(s_address.sun_path, 0600) || listen(fd, 8)) {
        close(fd);
        unlink(s_address.sun_path);
        return 0;
    }

    s_listener = fd;
    s_active = true;
    s_io_queue = dispatch_queue_create("com.hopdesk.split-agent-io", DISPATCH_QUEUE_SERIAL);
    s_source = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, fd, 0, s_io_queue);
    dispatch_source_set_event_handler(s_source, ^{
        for (int i = 0; i < 4 && !atomic_load(&s_stopping); ++i) {
            int client = accept(fd, NULL, NULL);
            if (client < 0) return;
            if (configure_socket(client)) serve(client);
            close(client);
        }
    });
    dispatch_resume(s_source);

    signal(SIGTERM, SIG_IGN);
    s_sigterm = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(s_sigterm, ^{ stop_run_loop(); });
    dispatch_resume(s_sigterm);
    return 1;
}

void split_agent_run(void) {
    statusbar_init();
    int guard = hop_watchdog_begin("macOS split-agent run loop", 30000);
    CFRunLoopTimerRef timer = CFRunLoopTimerCreateWithHandler(NULL, CFAbsoluteTimeGetCurrent() + 1,
        1, 0, 0, ^(CFRunLoopTimerRef heartbeat) {
            (void)heartbeat;
            hop_watchdog_ping(guard);
        });
    CFRunLoopAddTimer(CFRunLoopGetMain(), timer, kCFRunLoopCommonModes);
    if (!atomic_load(&s_stopping)) [NSApp run];
    CFRunLoopTimerInvalidate(timer);
    CFRelease(timer);
    hop_watchdog_end(guard);

    atomic_store(&s_stopping, true);
    dispatch_source_cancel(s_sigterm);
    dispatch_source_cancel(s_source);
    dispatch_sync(s_io_queue, ^{});
    close(s_listener);
    unlink(s_address.sun_path);
    s_sigterm = nil;
    s_source = nil;
    s_io_queue = nil;
    s_listener = -1;

    mac_capture_return();
    inject_release_all();
    s_active = false;
}
