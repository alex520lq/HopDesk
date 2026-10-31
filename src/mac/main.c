/* =====================================================================
 *  HopDesk - macOS 入口（纯 C）
 *  初始化配置、网络线程、状态栏，进入 Cocoa 主循环。
 * ===================================================================== */
#include "mac_app.h"
#include "xfer.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

/* ---- 单实例 ----
 * macOS 端是客户端，不 bind 端口，所以不会"抢端口"；但两个实例会同时
 * 连上 Windows 端的同一个数据通道，谁后连谁把 s_x.client 顶掉 ——
 * 键鼠和剪贴板会来回跳，而且两端日志都看不出原因。
 *
 * 用 flock 拦：锁由内核在进程退出时释放，**崩溃也不会留下僵尸锁**，
 * 不像 pidfile 那样需要判断"那个 pid 还活着吗、是不是复用了"。
 *
 * 锁文件放 $TMPDIR：macOS 上它是每用户私有目录（/var/folders/…），
 * 天然按用户隔离，也不用自己拼 ~/Library 再建目录。
 * 故意不关 fd —— 锁要活到进程结束。 */
static int acquire_single_instance(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !dir[0]) dir = "/tmp";

    char path[PATH_MAX];
    size_t n = strlen(dir);
    snprintf(path, sizeof(path), "%s%shopdesk.lock",
             dir, (n > 0 && dir[n - 1] == '/') ? "" : "/");

    int fd = open(path, O_CREAT | O_RDWR, 0600);
    if (fd < 0) {
        /* 开不了就别拦：宁可双开，也别让用户完全打不开 */
        fprintf(stderr, "hopdesk: 无法创建实例锁 %s\n", path);
        return 1;
    }
    if (flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    if (!acquire_single_instance()) {
        fprintf(stderr, "hopdesk: 已经有一个实例在运行，本进程退出。\n");
        return 1;
    }

    memset(&g_mac, 0, sizeof(g_mac));
    g_mac.sock = -1;

    hop_config_defaults(&g_mac.cfg);
    snprintf(g_mac.cfg.device_name, HOP_NAME_MAX, "%s", "Alex67deMac");
    snprintf(g_mac.cfg.peer_host, sizeof(g_mac.cfg.peer_host),
             "%s", "192.168.31.8");

    /* Finder launches apps with an arbitrary working directory.
     * Store mutable files in the user's Application Support directory. */
    const char *home = getenv("HOME");
    char support[PATH_MAX], config[PATH_MAX], log[PATH_MAX];
    if (!home || !home[0]) home = "/tmp";
    snprintf(support, sizeof(support), "%s/Library/Application Support/HopDesk", home);
    (void)mkdir(support, 0700);
    snprintf(config, sizeof(config), "%s/hopdesk.conf", support);
    snprintf(log, sizeof(log), "%s/hopdesk.log", support);
    hop_config_load(&g_mac.cfg, config);
    hop_log_open(log);
    hop_log(HOP_LOG_INFO, "HopDesk %s starting", HOP_VERSION);

    inject_init();
    net_loop_run();
    xfer_init(&g_mac.cfg);
    statusbar_init();
    clipboard_init();
    app_run();
    return 0;
}
