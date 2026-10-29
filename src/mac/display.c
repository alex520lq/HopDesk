/* =====================================================================
 *  HopDesk - macOS 显示控制（阶段 4，纯 C）
 *
 *  三件事：
 *    唤醒  IOPMAssertionCreateWithName（等价 caffeinate，保持 N 秒）
 *    切源  posix_spawn 调外部命令（默认 m1ddc），DDC 值作为参数追加
 *    睡眠  posix_spawn 调外部命令（默认 pmset displaysleepnow）
 *
 *  为什么不自己写 DDC over IOKit：m1ddc 已经踩完了 Apple Silicon 上
 *  DDC 的坑，重写一遍没有任何收益。命令做成配置项而不是硬编码，
 *  是因为不同 m1ddc 版本参数写法不同 —— 猜 CLI 语法是必错的。
 *
 *  posix_spawn 的子进程必须回收，否则常驻进程会攒僵尸。用一个一次性
 *  detach 线程 waitpid，避免在接收线程里阻塞。
 * ===================================================================== */
#include "mac_app.h"

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>

#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static IOPMAssertionID s_assertion;
static unsigned        s_gen;

/* ---- 子进程 ---- */
static void *reaper(void *arg) {
    int st;
    waitpid((pid_t)(intptr_t)arg, &st, 0);
    return NULL;
}

static void run_cmd(const char *cmd) {
    posix_spawn_file_actions_t fa;
    posix_spawnattr_t at;
    char *argv[4];
    pid_t pid;
    pthread_t t;

    if (!cmd || !*cmd) return;

    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = (char *)cmd;
    argv[3] = NULL;

    posix_spawn_file_actions_init(&fa);
    posix_spawnattr_init(&at);
    int rc = posix_spawn(&pid, "/bin/sh", &fa, &at, argv, environ);
    posix_spawnattr_destroy(&at);
    posix_spawn_file_actions_destroy(&fa);

    if (rc != 0) {
        hop_log(HOP_LOG_WARN, "spawn failed (%d): %s", rc, cmd);
        return;
    }
    if (pthread_create(&t, NULL, reaper, (void *)(intptr_t)pid) == 0)
        pthread_detach(t);
}

/* ---- 唤醒：保持显示 N 秒 ---- */
struct rel_arg { unsigned gen; int sec; };

static void *releaser(void *arg) {
    struct rel_arg *r = (struct rel_arg *)arg;
    sleep((unsigned)r->sec);

    pthread_mutex_lock(&s_lock);
    if (r->gen == s_gen && s_assertion) {
        IOPMAssertionRelease(s_assertion);
        s_assertion = 0;
    }
    pthread_mutex_unlock(&s_lock);

    free(r);
    return NULL;
}

void disp_wake(int sec) {
    IOPMAssertionID id = 0;
    struct rel_arg *r;
    pthread_t t;
    unsigned gen;

    if (sec <= 0) sec = 1;

    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                    kIOPMAssertionLevelOn,
                                    CFSTR("HopDesk keep display awake"),
                                    &id) != kIOReturnSuccess) {
        hop_log(HOP_LOG_WARN, "display wake assertion failed");
        return;
    }

    pthread_mutex_lock(&s_lock);
    if (s_assertion) IOPMAssertionRelease(s_assertion);
    s_assertion = id;
    gen = ++s_gen;
    pthread_mutex_unlock(&s_lock);

    r = (struct rel_arg *)malloc(sizeof(*r));
    if (!r) return;
    r->gen = gen;
    r->sec = sec;
    if (pthread_create(&t, NULL, releaser, r) == 0) pthread_detach(t);
    else free(r);

    hop_log(HOP_LOG_INFO, "display awake for %d s", sec);
}

void disp_sleep(void) {
    pthread_mutex_lock(&s_lock);
    if (s_assertion) {
        IOPMAssertionRelease(s_assertion);
        s_assertion = 0;
    }
    s_gen++;                       /* 让等待中的 releaser 失效 */
    pthread_mutex_unlock(&s_lock);

    hop_log(HOP_LOG_INFO, "display sleep");
    run_cmd(g_mac.cfg.mac_sleep_cmd);
}

/* ---- 切输入源：<mac_input_cmd> <vcp> ---- */
void disp_set_input(uint16_t vcp) {
    char buf[192];
    snprintf(buf, sizeof(buf), "%s %u", g_mac.cfg.mac_input_cmd, (unsigned)vcp);
    hop_log(HOP_LOG_INFO, "display input -> 0x%02x (%s)", (unsigned)vcp, buf);
    run_cmd(buf);
}
