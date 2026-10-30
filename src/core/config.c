/* =====================================================================
 *  HopDesk - 配置（平台无关）
 *  默认值 + 可选的 key=value 配置文件（# 开头注释）。
 * ===================================================================== */
#include "hop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

void hop_config_defaults(hop_config *c) {
    memset(c, 0, sizeof(*c));
    c->port            = HOP_DEFAULT_PORT;
    c->server_edge     = HOP_EDGE_LEFT;
    c->client_edge     = HOP_EDGE_RIGHT;
    c->screen_w        = 0;
    c->screen_h        = 0;

    /* 边缘保护：Deskflow 的 switchDelay 是时间判定，不是距离判定。
     * 250ms 是 Deskflow 的传统默认值，够你点一下边缘的按钮而不被弹走。 */
    c->switch_delay_ms    = 250;
    c->switch_cooldown_ms = 300;
    c->edge_margin_px     = 40;    /* 进入内移 / 退出越界阈值 */
    /* 空 token = 不校验（局域网自用） */
    c->token[0]        = '\0';
    c->peer_host[0]    = '\0';
    c->device_name[0]  = '\0';

    /* 显示切换默认值：VESA VCP 0x60 —— 0x0F = DP-1，0x11 = HDMI-1。
     * 这两项与具体显示器型号强相关，必须按实际接线改。 */
    c->disp_win_input  = 0x0F;
    c->disp_mac_input  = 0x11;
    c->disp_wake_sec   = 60;
    snprintf(c->mac_input_cmd, sizeof(c->mac_input_cmd),
             "%s", "m1ddc display 1 set input");
    snprintf(c->mac_sleep_cmd, sizeof(c->mac_sleep_cmd),
             "%s", "pmset displaysleepnow");

    /* 文件传输：数据通道默认主端口 +1。
     * 分块默认 256 KB —— 实测 1 MB 会让控制通道尾延迟劣化 5 倍，
     * 而省下的 11% 吞吐在真实链路上不可见。详见 hop.h 的说明。 */
    c->data_port      = HOP_DATA_PORT;
    c->xfer_chunk_kb  = HOP_CHUNK_KB_DEFAULT;
}

/* 支持 0x 前缀（DDC 值习惯写十六进制） */
static int to_int(const char *v) {
    if (v[0] == '0' && (v[1] == 'x' || v[1] == 'X'))
        return (int)strtol(v + 2, NULL, 16);
    return atoi(v);
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = '\0';
    return s;
}

static void set_kv(hop_config *c, const char *k, const char *v) {
    if (!strcmp(k, "port"))          c->port = (uint16_t)atoi(v);
    else if (!strcmp(k, "token"))    snprintf(c->token, sizeof(c->token), "%s", v);
    else if (!strcmp(k, "host"))     snprintf(c->peer_host, sizeof(c->peer_host), "%s", v);
    else if (!strcmp(k, "name"))     snprintf(c->device_name, sizeof(c->device_name), "%s", v);
    else if (!strcmp(k, "server_edge")) c->server_edge = atoi(v);
    else if (!strcmp(k, "client_edge")) c->client_edge = atoi(v);
    else if (!strcmp(k, "switch_push_ms"))     c->switch_delay_ms = atoi(v);   /* 旧键名 */
    else if (!strcmp(k, "switch_delay_ms"))    c->switch_delay_ms = atoi(v);
    else if (!strcmp(k, "switch_cooldown_ms")) c->switch_cooldown_ms = atoi(v);
    else if (!strcmp(k, "edge_margin_px"))     c->edge_margin_px = atoi(v);
    else if (!strcmp(k, "disp_win_input")) c->disp_win_input = (uint16_t)to_int(v);
    else if (!strcmp(k, "disp_mac_input")) c->disp_mac_input = (uint16_t)to_int(v);
    else if (!strcmp(k, "disp_wake_sec"))  c->disp_wake_sec = atoi(v);
    else if (!strcmp(k, "mac_input_cmd"))  snprintf(c->mac_input_cmd, sizeof(c->mac_input_cmd), "%s", v);
    else if (!strcmp(k, "mac_sleep_cmd"))  snprintf(c->mac_sleep_cmd, sizeof(c->mac_sleep_cmd), "%s", v);
    else if (!strcmp(k, "data_port"))      c->data_port = (uint16_t)to_int(v);
    else if (!strcmp(k, "xfer_chunk_kb"))  c->xfer_chunk_kb = atoi(v);
}

int hop_config_load(hop_config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);
        set_kv(c, k, v);
    }
    fclose(f);
    return 1;
}
