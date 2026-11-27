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
#include "display_switch.h"
#include <ApplicationServices/ApplicationServices.h>
#include <signal.h>
#include <errno.h>
#include <stdatomic.h>

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

int disp_wake(int sec) {
    IOPMAssertionID id = 0;
    struct rel_arg *r;
    pthread_t t;
    unsigned gen;

    if (sec <= 0) sec = 1;
    IOPMAssertionID activity = 0;
    if (IOPMAssertionDeclareUserActivity(CFSTR("HopDesk remote display switch"),
        kIOPMUserActiveLocal, &activity) != kIOReturnSuccess) return 0;

    if (IOPMAssertionCreateWithName(kIOPMAssertionTypePreventUserIdleDisplaySleep,
                                    kIOPMAssertionLevelOn,
                                    CFSTR("HopDesk keep display awake"),
                                    &id) != kIOReturnSuccess) {
        hop_log(HOP_LOG_WARN, "display wake assertion failed");
        return 0;
    }

    pthread_mutex_lock(&s_lock);
    if (s_assertion) IOPMAssertionRelease(s_assertion);
    s_assertion = id;
    gen = ++s_gen;
    pthread_mutex_unlock(&s_lock);

    r = (struct rel_arg *)malloc(sizeof(*r));
    if (!r) {
        pthread_mutex_lock(&s_lock);
        if (s_gen == gen) { IOPMAssertionRelease(s_assertion); s_assertion = 0; }
        pthread_mutex_unlock(&s_lock);
        return 0;
    }
    r->gen = gen;
    r->sec = sec;
    if (pthread_create(&t, NULL, releaser, r) == 0) pthread_detach(t);
    else {
        free(r);
        pthread_mutex_lock(&s_lock);
        if (s_gen == gen) { IOPMAssertionRelease(s_assertion); s_assertion = 0; }
        pthread_mutex_unlock(&s_lock);
        return 0;
    }

    hop_log(HOP_LOG_INFO, "display awake for %d s", sec);
    return 1;
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

/* A bounded, single worker keeps m1ddc and wake polling off the control reader.
 * The captured session rejects completion after reconnect. */
static pthread_mutex_t s_switch_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t s_switch_thread;
static int s_switch_started, s_switch_busy;
static _Atomic int s_switch_stopping;
static char s_switch_status[192];
void disp_switch_status(char *out, size_t cap) {
    pthread_mutex_lock(&s_switch_lock);
    snprintf(out, cap, "%s", s_switch_status);
    pthread_mutex_unlock(&s_switch_lock);
}
typedef struct {
    hop_display_msg msg;
    hop_session_id session;
    char command[96];
} mac_display_job;

static int run_switch_command(const char *prefix, uint32_t input, const hop_session_id *session) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; %s %u", prefix, input);
    char *argv[] = {"/bin/sh", "-c", cmd, NULL};
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc) return 0;
    uint64_t deadline = hop_monotonic_ms() + 8000;
    int status = 0;
    for (;;) {
        pid_t done = waitpid(pid, &status, WNOHANG);
        if (done == pid) return WIFEXITED(status) && WEXITSTATUS(status) == 0;
        if (done < 0 && errno != EINTR) return 0;
        if (atomic_load(&s_switch_stopping) || !hop_session_matches(session) || hop_monotonic_ms() >= deadline) break;
        usleep(100000);
    }
    kill(-pid, SIGKILL);
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    return 0;
}
static int wake_ready(mac_display_job *j) {
    if (!disp_wake((int)j->msg.value)) return 0;
    uint64_t deadline = hop_monotonic_ms() + 5000;
    do {
        CGDirectDisplayID ids[16]; uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) == kCGErrorSuccess) {
            for (uint32_t i = 0; i < count; ++i)
                if (!CGDisplayIsBuiltin(ids[i]) && !CGDisplayIsAsleep(ids[i])) return 1;
        }
        usleep(100000);
    } while (!atomic_load(&s_switch_stopping) && hop_session_matches(&j->session) && hop_monotonic_ms() < deadline);
    return 0;
}
static void *switch_worker(void *arg) {
    mac_display_job *j = arg;
    int ok = 0;
    if (!atomic_load(&s_switch_stopping) && hop_session_matches(&j->session))
        ok = j->msg.op == HOP_DISPLAY_WAKE ? wake_ready(j) :
            run_switch_command(j->command, j->msg.value, &j->session);
    if (!atomic_load(&s_switch_stopping) && hop_session_matches(&j->session)) {
        uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
        hop_display_msg reply = {HOP_DISPLAY_RESULT, (uint8_t)ok, j->msg.id, 0};
        hop_display_encode(wire, &reply);
        net_send(HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
    }
    hop_log(ok ? HOP_LOG_INFO : HOP_LOG_ERROR, "display: peer operation=%u id=%u result=%d",
        j->msg.op, j->msg.id, ok);
    free(j);
    pthread_mutex_lock(&s_switch_lock); s_switch_busy = 0; pthread_mutex_unlock(&s_switch_lock);
    return NULL;
}
void disp_switch_message(const uint8_t *p, uint32_t len) {
    hop_display_msg msg;
    if (atomic_load(&s_switch_stopping) || !hop_display_decode(&msg, p, len)) return;
    if (msg.op == HOP_DISPLAY_LOCAL) {
        disp_set_input(msg.value == HOP_DISPLAY_MAC ? g_mac.cfg.disp_mac_input : g_mac.cfg.disp_win_input);
        pthread_mutex_lock(&s_switch_lock);
        snprintf(s_switch_status, sizeof(s_switch_status), "%s", "普通模式：已提交本机输入源切换；未修改 Windows 显示拓扑");
        pthread_mutex_unlock(&s_switch_lock);
        return;
    }
    if (msg.op == HOP_DISPLAY_COMPLETE) {
        pthread_mutex_lock(&s_switch_lock);
        snprintf(s_switch_status, sizeof(s_switch_status), "%s", msg.ok ?
            (msg.value == HOP_DISPLAY_MAC ? "Windows 已确认：共用屏已交给 Mac" : "Windows 已确认：扩展屏与输入源已恢复") :
            "共屏切换失败，请检查 Windows 运行日志与共屏设置");
        pthread_mutex_unlock(&s_switch_lock);
        return;
    }
    if (msg.op != HOP_DISPLAY_WAKE && msg.op != HOP_DISPLAY_INPUT) return;
    mac_display_job *j = calloc(1, sizeof(*j));
    if (!j) return;
    j->msg = msg;
    if (!hop_session_current(&j->session)) { free(j); return; }
    snprintf(j->command, sizeof(j->command), "%s", g_mac.cfg.mac_input_cmd);
    pthread_mutex_lock(&s_switch_lock);
    if (s_switch_busy) { pthread_mutex_unlock(&s_switch_lock); free(j); return; }
    if (s_switch_started) { pthread_join(s_switch_thread, NULL); s_switch_started = 0; }
    s_switch_busy = 1;
    int rc = pthread_create(&s_switch_thread, NULL, switch_worker, j);
    s_switch_started = rc == 0;
    if (rc) { s_switch_busy = 0; free(j); }
    pthread_mutex_unlock(&s_switch_lock);
}
int disp_switch_target(int target) {
    if (target != HOP_DISPLAY_MAC && target != HOP_DISPLAY_WINDOWS) return 0;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg msg = {HOP_DISPLAY_REQUEST, 0, 0, (uint32_t)target};
    hop_display_encode(wire, &msg);
    if (net_send(HOP_MSG_DISP_SWITCH, wire, sizeof(wire))) {
        pthread_mutex_lock(&s_switch_lock);
        snprintf(s_switch_status, sizeof(s_switch_status), "%s", "已提交切屏请求，等待 Windows 确认；详情见 Windows 概览与日志");
        pthread_mutex_unlock(&s_switch_lock);
        hop_log(HOP_LOG_INFO, "display: requested Windows coordinator target=%d (pending)", target);
        return 1;
    }
    hop_log(HOP_LOG_ERROR, "display: Windows peer unavailable; topology was not changed");
    return 0;
}
void disp_switch_shutdown(void) {
    atomic_store(&s_switch_stopping, 1);
    pthread_mutex_lock(&s_switch_lock);
    int join = s_switch_started; s_switch_started = 0;
    pthread_mutex_unlock(&s_switch_lock);
    if (join) pthread_join(s_switch_thread, NULL);
}
