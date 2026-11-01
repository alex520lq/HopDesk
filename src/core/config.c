/* =====================================================================
 *  HopDesk - 配置（平台无关）
 *  默认值 + 可选的 key=value 配置文件（# 开头注释）。
 * ===================================================================== */
#include "hop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#if !defined(_WIN32)
#include <sys/stat.h>
#endif
#include <stdint.h>
#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#else
#include <unistd.h>
#endif

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

static int set_kv(hop_config *c, const char *k, const char *v) {
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
    else return 0;
    return 1;
}

int hop_config_load(hop_config *c, const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    hop_config candidate=*c;
    int recognized=0;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char *p = trim(line);
        if (!*p || *p == '#') continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = '\0';
        char *k = trim(p);
        char *v = trim(eq + 1);
        recognized+=set_kv(&candidate, k, v);
    }
    int ok=!ferror(f) && recognized>0;
    if(fclose(f)!=0)ok=0;
    if(ok)*c=candidate;
    return ok;
}

/* Save a complete, canonical config through a same-directory temporary file.
 * The previous file remains intact unless the new file is fully flushed and
 * atomically committed. Paths are UTF-8 on every platform. */
static void config_safe_value(const char *src, char *dst, size_t cap) {
    size_t n = 0;
    if (!cap) return;
    while (src && *src && n + 1 < cap) {
        unsigned char ch = (unsigned char)*src++;
        if (ch == '\\r' || ch == '\\n' || ch == 0) continue;
        dst[n++] = (char)ch;
    }
    dst[n] = '\\0';
}

#if defined(_WIN32)
static int config_utf8_path(const char *src, wchar_t *dst, int cap) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, -1, dst, cap) > 0;
}
#endif

int hop_config_save_atomic(const hop_config *c, const char *path) {
    if (!c || !path || !path[0] || strlen(path) > 900) return 0;
    char temp[1024];
    int temp_len = snprintf(temp, sizeof(temp), "%s.tmp", path);\n    if (temp_len <= 0 || (size_t)temp_len >= sizeof(temp)) return 0;
#if defined(_WIN32)
    wchar_t wtemp[1024], wpath[1024];
    if (!config_utf8_path(temp, wtemp, 1024) || !config_utf8_path(path, wpath, 1024)) return 0;
    FILE *f = _wfopen(wtemp, L"wb");
#else
    FILE *f = fopen(temp, "wb");
#endif
    if (!f) return 0;

    char host[128], token[HOP_TOKEN_MAX + 1], name[HOP_NAME_MAX + 1];
    char input_cmd[sizeof(c->mac_input_cmd) + 1], sleep_cmd[sizeof(c->mac_sleep_cmd) + 1];
    config_safe_value(c->peer_host, host, sizeof(host));
    config_safe_value(c->token, token, sizeof(token));
    config_safe_value(c->device_name, name, sizeof(name));
    config_safe_value(c->mac_input_cmd, input_cmd, sizeof(input_cmd));
    config_safe_value(c->mac_sleep_cmd, sleep_cmd, sizeof(sleep_cmd));

    int ok = 1;
#define CFG_WRITE(...) do { if (fprintf(f, __VA_ARGS__) < 0) ok = 0; } while (0)
    CFG_WRITE("# HopDesk configuration — edited by the native UI\\n");
    CFG_WRITE("name=%s\\n", name);
    CFG_WRITE("host=%s\\n", host);
    CFG_WRITE("port=%u\\n", (unsigned)c->port);
    CFG_WRITE("data_port=%u\\n", (unsigned)c->data_port);
    CFG_WRITE("token=%s\\n", token);
    CFG_WRITE("server_edge=%d\\n", c->server_edge);
    CFG_WRITE("client_edge=%d\\n", c->client_edge);
    CFG_WRITE("switch_delay_ms=%d\\n", c->switch_delay_ms);
    CFG_WRITE("switch_cooldown_ms=%d\\n", c->switch_cooldown_ms);
    CFG_WRITE("edge_margin_px=%d\\n", c->edge_margin_px);
    CFG_WRITE("disp_win_input=0x%04X\\n", (unsigned)c->disp_win_input);
    CFG_WRITE("disp_mac_input=0x%04X\\n", (unsigned)c->disp_mac_input);
    CFG_WRITE("disp_wake_sec=%d\\n", c->disp_wake_sec);
    CFG_WRITE("mac_input_cmd=%s\\n", input_cmd);
    CFG_WRITE("mac_sleep_cmd=%s\\n", sleep_cmd);
    CFG_WRITE("xfer_chunk_kb=%d\\n", c->xfer_chunk_kb);
#undef CFG_WRITE
    if (fflush(f) != 0) ok = 0;
    if (ok) {
#if defined(_WIN32)
        if (_commit(_fileno(f)) != 0) ok = 0;
#else
        if (fsync(fileno(f)) != 0) ok = 0;
#endif
    }
    if (fclose(f) != 0) ok = 0;
    if (!ok) {
#if defined(_WIN32)
        _wremove(wtemp);
#else
        unlink(temp);
#endif
        return 0;
    }
#if defined(_WIN32)
    if (!MoveFileExW(wtemp, wpath, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        _wremove(wtemp);
        return 0;
    }
#else
    if (rename(temp, path) != 0) {
        unlink(temp);
        return 0;
    }
    (void)chmod(path, 0600);
#endif
    return 1;
}
