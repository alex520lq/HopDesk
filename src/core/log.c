/* =====================================================================
 *  HopDesk - 日志（平台无关）
 *  输出到 stderr；平台层可把日志文件路径（UTF-8）交给 hop_log_open。
 *
 *  常驻进程的日志必须有上限：超过 HOP_LOG_ROTATE_BYTES 就把当前文件
 *  改名为 <path>.1（覆盖上一份）再重新打开，磁盘占用最多两份。
 *  多个线程（网络、数据通道、主线程）同时写，必须加锁 —— 否则行会交错，
 *  轮转时还可能对已经 fclose 的 FILE* 写入。
 * ===================================================================== */
#include "hop.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <wchar.h>

#if defined(_WIN32)
#include <windows.h>
static SRWLOCK g_lock = SRWLOCK_INIT;
#define LOG_LOCK()   AcquireSRWLockExclusive(&g_lock)
#define LOG_UNLOCK() ReleaseSRWLockExclusive(&g_lock)
#else
#include <pthread.h>
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOG_LOCK()   pthread_mutex_lock(&g_lock)
#define LOG_UNLOCK() pthread_mutex_unlock(&g_lock)
#endif

static int g_enabled = 1;
static FILE *g_file = NULL;
static char g_path[1024];

void hop_log_set(int enable) { LOG_LOCK(); g_enabled = enable; LOG_UNLOCK(); }

#if defined(_WIN32)
/* Windows 的 fopen/rename 按 ANSI 代码页解释路径：exe 放在中文目录下就打不开。 */
static int to_wide(const char *u, wchar_t *w, int n) {
    return MultiByteToWideChar(CP_UTF8, 0, u, -1, w, n) > 0;
}
static FILE *open_append(const char *path) {
    wchar_t w[1024];
    if (!to_wide(path, w, 1024)) return NULL;
    return _wfopen(w, L"a");
}
static void rotate_files(const char *path) {
    wchar_t w[1024], old[1030];
    if (!to_wide(path, w, 1024)) return;
    _snwprintf(old, 1030, L"%ls.1", w);
    old[1029] = L'\0';
    _wremove(old);
    _wrename(w, old);
}
#else
static FILE *open_append(const char *path) { return fopen(path, "a"); }
static void rotate_files(const char *path) {
    char old[1030];
    snprintf(old, sizeof(old), "%s.1", path);
    rename(path, old);              /* POSIX rename 原子覆盖旧的 .1 */
}
#endif

void hop_log_open(const char *path) {
    LOG_LOCK();
    if (g_file) { fclose(g_file); g_file = NULL; }
    g_path[0] = '\0';
    if (path && strlen(path) < sizeof(g_path)) {
        memcpy(g_path, path, strlen(path) + 1);
        g_file = open_append(g_path);
    }
    LOG_UNLOCK();
}

static void rotate_if_needed_locked(void) {
    long pos;
    if (!g_file || !g_path[0]) return;
    pos = ftell(g_file);
    if (pos < 0 || pos < HOP_LOG_ROTATE_BYTES) return;
    fclose(g_file);
    g_file = NULL;
    rotate_files(g_path);
    g_file = open_append(g_path);   /* 失败就退回 stderr，不能因为日志让进程出事 */
}

static const char *tag(hop_log_level l) {
    switch (l) {
        case HOP_LOG_WARN:  return "WARN";
        case HOP_LOG_ERROR: return "ERR ";
        default:            return "INFO";
    }
}

void hop_log(hop_log_level lvl, const char *fmt, ...) {

    time_t t = time(NULL);
    struct tm tmv;
    char ts[24];
#if defined(_WIN32)
    if (localtime_s(&tmv, &t) == 0) strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);
    else ts[0] = '\0';
#else
    if (localtime_r(&t, &tmv)) strftime(ts, sizeof(ts), "%m-%d %H:%M:%S", &tmv);
    else ts[0] = '\0';
#endif

    LOG_LOCK();
    if (!g_enabled) { LOG_UNLOCK(); return; }
    FILE *out = g_file ? g_file : stderr;
    fprintf(out, "%s %s ", ts, tag(lvl));
    va_list ap;
    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
    fputc('\n', out);
    fflush(out);
    rotate_if_needed_locked();
    LOG_UNLOCK();
}
