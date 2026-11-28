/* =====================================================================
 *  HopDesk - macOS 入口（纯 C）
 *  初始化配置、网络线程、状态栏，进入 Cocoa 主循环。
 * ===================================================================== */
#include "mac_app.h"
#include "split_agent.h"
#include "split_ui.h"
#include "xfer.h"
#include "../core/cli.h"
#include "../core/config_migrate.h"
#include "../platform/audio_link.h"
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

int mac_apply_hot_config(const hop_config *next) {
    if (!next) return 0;
    unsigned prev_input = net_get_input_mode();
    unsigned next_input = next->input_mode & HOP_INPUT_MODES;
    unsigned takeover = next_input & ~prev_input;

    hop_role live_role = g_mac.cfg.role;
    uint16_t live_port = g_mac.cfg.port;
    uint16_t live_data_port = g_mac.cfg.data_port;
    uint16_t live_audio_port = g_mac.cfg.audio_port;
    int live_rx_slots = g_mac.cfg.xfer_rx_queue_slots;
    int live_rx_mb = g_mac.cfg.xfer_rx_queue_mb;
    int live_poll_ms = g_mac.cfg.clip_poll_ms;

    g_mac.cfg = *next;
    g_mac.cfg.role = live_role;
    g_mac.cfg.port = live_port;
    g_mac.cfg.data_port = live_data_port;
    g_mac.cfg.audio_port = live_audio_port;
    g_mac.cfg.xfer_rx_queue_slots = live_rx_slots;
    g_mac.cfg.xfer_rx_queue_mb = live_rx_mb;
    g_mac.cfg.clip_poll_ms = live_poll_ms;

    hop_runtime_apply_config(&g_mac.cfg);
    if (next_input != prev_input)
        net_apply_input_mode(next_input, takeover, 0);
    else
        net_send_capabilities(0);
    hop_config runtime = g_mac.cfg;
    return hop_audio_set_mode(&runtime, runtime.audio_mode);
}

int main(int argc, char **argv) {
    /* Resolve canonical Application Support paths first so headless CLI works
     * without acquiring the single-instance lock or initializing Cocoa. */
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

    if (hop_cli_is_invocation(argc, (const char *const *)argv)) {
        hop_cli_ops ops;
        memset(&ops, 0, sizeof(ops));
        ops.default_config_path = config;
        ops.default_log_path = log;
        ops.default_device_name = "Mac";
        ops.agent_get_status = mac_cli_agent_get_status;
        ops.agent_get_config = mac_cli_agent_get_config;
        ops.agent_save_config = mac_cli_agent_save_config;
        ops.agent_action = mac_cli_agent_action;
        return hop_cli_run(argc, (const char *const *)argv, &ops);
    }

    const char *config_override = hop_cli_extract_config_flag(argc, (const char *const *)argv);

    /* Manual first launch starts the agent and opens the main UI once; closing
     * the UI leaves the agent in the menu bar. --agent (LaunchAgent) and
     * updater relaunches stay silent in the background. */
    const int start_agent = argc == 2 && strcmp(argv[1], "--split") == 0;
    const int open_ui = argc == 2 && strcmp(argv[1], "--ui") == 0;
    int silent_agent = getenv("HOPDESK_UPDATE_READY") != NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--agent") == 0) silent_agent = 1;
    }
    const int finder_launch = argc == 1 || (argc == 2 && strncmp(argv[1], "-psn_", 5) == 0);
    const int ui = start_agent || open_ui || (finder_launch && split_ui_agent_available());
    int startup_guard = 0;

    if (!ui) {
        if (!acquire_single_instance()) {
            fprintf(stderr, "hopdesk: 已经有一个实例在运行，本进程退出。\n");
            return 0; /* a duplicate launch is not a crash to restart */
        }
        startup_guard = hop_watchdog_begin("macOS startup", 30000);
    }

    memset(&g_mac, 0, sizeof(g_mac));

    hop_config_defaults(&g_mac.cfg);
    snprintf(g_mac.cfg.device_name, HOP_NAME_MAX, "%s", "Mac");
    g_mac.cfg.peer_host[0]=0;

    if (config_override && config_override[0])
        snprintf(config, sizeof(config), "%s", config_override);
    snprintf(g_mac.config_path, sizeof(g_mac.config_path), "%s", config);
    snprintf(g_mac.log_path, sizeof(g_mac.log_path), "%s", log);

    if (ui) return split_ui_run(start_agent);

    hop_log_open(log);
    const char *loaded_from=config;
    int config_loaded = hop_config_load(&g_mac.cfg, config);
    if (!config_loaded && !(config_override && config_override[0])) {
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
    hop_runtime_apply_config(&g_mac.cfg);
    hop_log(HOP_LOG_INFO, "HopDesk %s starting", HOP_VERSION);
    hop_log(HOP_LOG_INFO, "config: %s (%s), host=%s port=%u data_port=%u token_len=%u",
            loaded_from, config_loaded ? "loaded" : "defaults",
            g_mac.cfg.peer_host[0] ? g_mac.cfg.peer_host : "(not configured)",
            (unsigned)g_mac.cfg.port, (unsigned)g_mac.cfg.data_port,
            (unsigned)strlen(g_mac.cfg.token));

    inject_init();
    /* Client still needs the local Command+V tap for a remote file Offer. */
    mac_capture_init();
    if (!xfer_init(&g_mac.cfg)) {
        hop_log(HOP_LOG_ERROR, "xfer: initialization failed; settling partial startup before exit");
        while (!xfer_shutdown()) usleep(100000);
        return 1;
    }
    if (!split_agent_start()) {
        hop_log(HOP_LOG_ERROR, "split agent: unable to start local service");
        while (!xfer_shutdown()) usleep(100000);
        return 1;
    }
    mac_check_input_permissions();
    clipboard_init();
    net_loop_run();
    if(!hop_audio_set_mode(&g_mac.cfg,g_mac.cfg.audio_mode))
        hop_log(HOP_LOG_WARN,"audio: unable to start configured audio mode %u",g_mac.cfg.audio_mode);
    hop_watchdog_end(startup_guard);
    split_agent_set_open_on_launch(!silent_agent);
    split_agent_run();
    mac_update_shutdown();
    hop_audio_shutdown();
    /* clipboard workers may still own image-temp Reservations; the control
     * reader may still call xfer_kick. Join both before tearing down xfer. */
    while (!clipboard_shutdown()) usleep(100000); /* retain unresolved image Task owners */
    net_loop_shutdown();
    /* No control or clipboard worker may call into xfer after this point. */
    while (!xfer_shutdown()) usleep(100000);
    return 0;
}
