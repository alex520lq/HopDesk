/* =====================================================================
 *  HopDesk - macOS 入口（纯 C）
 *  初始化配置、网络线程、状态栏，进入 Cocoa 主循环。
 * ===================================================================== */
#include "mac_app.h"
#include "xfer.h"
#include "../core/config_migrate.h"
#include <mach-o/dyld.h>

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
    int startup_guard = hop_watchdog_begin("macOS startup", 30000);

    if (!acquire_single_instance()) {
        fprintf(stderr, "hopdesk: 已经有一个实例在运行，本进程退出。\n");
        return 0; /* a duplicate launch is not a crash to restart */
    }

    memset(&g_mac, 0, sizeof(g_mac));

    hop_config_defaults(&g_mac.cfg);
    snprintf(g_mac.cfg.device_name, HOP_NAME_MAX, "%s", "Mac");
    g_mac.cfg.peer_host[0]=0;

    /* Finder launches apps with an arbitrary working directory.
     * Keep the canonical config in Application Support, but migrate legacy
     * configs from beside the executable or the old relative working path. */
    const char *home = getenv("HOME");
    char library[PATH_MAX], app_support[PATH_MAX], support[PATH_MAX];
    char config[PATH_MAX], log[PATH_MAX], legacy[PATH_MAX] = {0};
    if (!home || !home[0]) home = "/tmp";
    snprintf(library, sizeof(library), "%s/Library", home);
    snprintf(app_support, sizeof(app_support), "%s/Application Support", library);
    snprintf(support, sizeof(support), "%s/HopDesk", app_support);
    (void)mkdir(library, 0700);
    (void)mkdir(app_support, 0700);
    (void)mkdir(support, 0700);
    snprintf(config, sizeof(config), "%s/hopdesk.conf", support);
    snprintf(log, sizeof(log), "%s/hopdesk.log", support);
    snprintf(g_mac.config_path, sizeof(g_mac.config_path), "%s", config);
    snprintf(g_mac.log_path, sizeof(g_mac.log_path), "%s", log);

    hop_log_open(log);
    const char *loaded_from=config;
    int config_loaded = hop_config_load(&g_mac.cfg, config);
    if (!config_loaded) {
        uint32_t exe_len = (uint32_t)sizeof(legacy);
        if (_NSGetExecutablePath(legacy, &exe_len) == 0) {
            char *slash = strrchr(legacy, '/');
            if (slash) {
                *slash = '\0';
                size_t used = strlen(legacy);
                if (used + sizeof("/hopdesk.conf") < sizeof(legacy))
                    strcat(legacy, "/hopdesk.conf");
                else legacy[0] = '\0';
            } else legacy[0] = '\0';
        } else legacy[0] = '\0';
        if (legacy[0]) config_loaded = hop_config_load(&g_mac.cfg, legacy);
        if (!config_loaded) {
            config_loaded = hop_config_load(&g_mac.cfg, "hopdesk.conf");
            if (config_loaded) snprintf(legacy, sizeof(legacy), "%s", "hopdesk.conf");
        }
        if (config_loaded && legacy[0]) {
            loaded_from=legacy;
            if(!hop_config_copy_atomic(legacy,config))
                hop_log(HOP_LOG_WARN,"config: migration %s -> %s failed: %s; keeping legacy source",
                        legacy,config,strerror(errno));
        }
    }
    hop_log(HOP_LOG_INFO, "HopDesk %s starting", HOP_VERSION);
    hop_log(HOP_LOG_INFO, "config: %s (%s), host=%s port=%u data_port=%u token_len=%u",
            loaded_from, config_loaded ? "loaded" : "defaults",
            g_mac.cfg.peer_host[0] ? g_mac.cfg.peer_host : "(not configured)",
            (unsigned)g_mac.cfg.port, (unsigned)g_mac.cfg.data_port,
            (unsigned)strlen(g_mac.cfg.token));

    inject_init();
    if(g_mac.cfg.role==HOP_ROLE_SERVER)mac_capture_init();
    if (!xfer_init(&g_mac.cfg)) {
        hop_log(HOP_LOG_ERROR, "xfer: initialization failed; settling partial startup before exit");
        while (!xfer_shutdown()) usleep(100000);
        return 1;
    }
    statusbar_init();
    clipboard_init();
    net_loop_run();
    hop_watchdog_end(startup_guard);
    app_run();
    /* clipboard workers may still own image-temp Reservations; the control
     * reader may still call xfer_kick. Join both before tearing down xfer. */
    while (!clipboard_shutdown()) usleep(100000); /* retain unresolved image Task owners */
    net_loop_shutdown();
    /* No control or clipboard worker may call into xfer after this point. */
    while (!xfer_shutdown()) usleep(100000);
    return 0;
}
