/* =====================================================================
 *  HopDesk - 日志（平台无关）
 *  输出到 stderr；平台层可把日志文件路径交给 hop_log_open。
 * ===================================================================== */
#include "hop.h"
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

static int g_enabled = 1;
static FILE *g_file = NULL;

void hop_log_set(int enable) { g_enabled = enable; }

void hop_log_open(const char *path) {
    if (g_file) { fclose(g_file); g_file = NULL; }
    if (path) g_file = fopen(path, "a");
}

static const char *tag(hop_log_level l) {
    switch (l) {
        case HOP_LOG_WARN:  return "WARN";
        case HOP_LOG_ERROR: return "ERR ";
        default:            return "INFO";
    }
}

void hop_log(hop_log_level lvl, const char *fmt, ...) {
    if (!g_enabled) return;
    FILE *out = g_file ? g_file : stderr;
    time_t t = time(NULL);
    struct tm tmv;
    char ts[16];
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) == 0) strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    else ts[0] = '\0';
#else
    if (localtime_r(&t, &tmv)) strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    else ts[0] = '\0';
#endif

    fprintf(out, "%s %s ", ts, tag(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
}
