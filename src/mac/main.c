/* =====================================================================
 *  HopDesk - macOS 入口（纯 C）
 *  初始化配置、网络线程、状态栏，进入 Cocoa 主循环。
 * ===================================================================== */
#include "mac_app.h"

#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    memset(&g_mac, 0, sizeof(g_mac));
    g_mac.sock = -1;

    hop_config_defaults(&g_mac.cfg);
    snprintf(g_mac.cfg.device_name, HOP_NAME_MAX, "%s", "Alex67deMac");
    snprintf(g_mac.cfg.peer_host, sizeof(g_mac.cfg.peer_host),
             "%s", "192.168.31.1");

    /* 可选：exe 同目录 hopdesk.conf / hopdesk.log */
    hop_config_load(&g_mac.cfg, "hopdesk.conf");
    hop_log_open("hopdesk.log");
    hop_log(HOP_LOG_INFO, "HopDesk %s starting", HOP_VERSION);

    inject_init();
    net_loop_run();
    statusbar_init();
    app_run();
    return 0;
}
