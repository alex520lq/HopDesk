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
    c->switch_push_ms  = 0;        /* 0 = 一碰边缘即移交 */
    c->screen_w        = 0;
    c->screen_h        = 0;
    /* 空 token = 不校验（局域网自用） */
    c->token[0]        = '\0';
    c->peer_host[0]    = '\0';
    c->device_name[0]  = '\0';
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
    else if (!strcmp(k, "switch_push_ms")) c->switch_push_ms = atoi(v);
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
