/* =====================================================================
 *  HopDesk - macOS 显示控制（阶段 4，纯 C）
 *
 *  三件事：
 *    唤醒  IOPMAssertionCreateWithName（等价 caffeinate，保持 N 秒，可主动释放）
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
static IOPMAssertionID s_user_activity;
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
    char wrapped[320];
    char *argv[4];
    pid_t pid;
    pthread_t t;

    if (!cmd || !*cmd) return;
    snprintf(wrapped, sizeof(wrapped),
             "export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; %s", cmd);

    argv[0] = (char *)"/bin/sh";
    argv[1] = (char *)"-c";
    argv[2] = wrapped;
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
    pthread_mutex_lock(&s_lock);
    IOReturn act_rc = IOPMAssertionDeclareUserActivity(
        CFSTR("HopDesk remote display switch"),
        kIOPMUserActiveLocal, &s_user_activity);
    pthread_mutex_unlock(&s_lock);
    if (act_rc != kIOReturnSuccess) return 0;

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

void disp_wake_clear(void) {
    int had = 0;
    pthread_mutex_lock(&s_lock);
    if (s_assertion) {
        IOPMAssertionRelease(s_assertion);
        s_assertion = 0;
        had = 1;
    }
    s_gen++; /* invalidate any sleeping releaser */
    pthread_mutex_unlock(&s_lock);
    if (had) hop_log(HOP_LOG_INFO, "display wake assertion cleared");
}

void disp_sleep(void) {
    disp_wake_clear();
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

static int clamp_int(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}
void disp_switch_status(char *out, size_t cap) {
    pthread_mutex_lock(&s_switch_lock);
    snprintf(out, cap, "%s", s_switch_status);
    pthread_mutex_unlock(&s_switch_lock);
}
static void set_switch_status(const char *text) {
    pthread_mutex_lock(&s_switch_lock);
    snprintf(s_switch_status, sizeof(s_switch_status), "%s", text);
    pthread_mutex_unlock(&s_switch_lock);
}
typedef struct {
    hop_display_msg msg;
    hop_session_id session;
    char command[96];
    char sleep_cmd[64];
    int timeout_ms;
    int poll_ms;
} mac_display_job;

static int run_shell_command(const char *raw_cmd, const hop_session_id *session, int timeout_ms) {
    if (!raw_cmd || !*raw_cmd) return 0;
    char cmd[320];
    snprintf(cmd, sizeof(cmd), "export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH; %s", raw_cmd);
    char *argv[] = {"/bin/sh", "-c", cmd, NULL};
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    pid_t pid;
    int rc = posix_spawn(&pid, "/bin/sh", NULL, &attr, argv, environ);
    posix_spawnattr_destroy(&attr);
    if (rc) return 0;
    uint64_t deadline = hop_monotonic_ms() + (uint64_t)clamp_int(timeout_ms, 1000, 30000);
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
static int run_switch_command(const char *prefix, uint32_t input, const hop_session_id *session) {
    char cmd[224];
    snprintf(cmd, sizeof(cmd), "%s %u", prefix, input);
    return run_shell_command(cmd, session, 8000);
}
static void send_result(const hop_session_id *session, uint32_t id, int ok) {
    if (!id) return;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg reply = {HOP_DISPLAY_RESULT, (uint8_t)(ok ? 1 : 0), id, 0};
    if (hop_display_encode(wire, &reply))
        (void)net_send_session(session, HOP_MSG_DISP_SWITCH, wire, sizeof(wire));
}
static int wake_ready(mac_display_job *j) {
    if (!disp_wake((int)j->msg.value)) return 0;
    int wait_ms = clamp_int(j->timeout_ms > 1000 ? j->timeout_ms - 1000 : 5000, 3000, 15000);
    unsigned poll_us = (unsigned)clamp_int(j->poll_ms, 50, 2000) * 1000u;
    uint64_t deadline = hop_monotonic_ms() + (uint64_t)wait_ms;
    do {
        CGDirectDisplayID ids[16]; uint32_t count = 0;
        if (CGGetActiveDisplayList(16, ids, &count) == kCGErrorSuccess) {
            for (uint32_t i = 0; i < count; ++i)
                if (!CGDisplayIsBuiltin(ids[i]) && !CGDisplayIsAsleep(ids[i])) return 1;
        }
        usleep(poll_us);
    } while (!atomic_load(&s_switch_stopping) && hop_session_matches(&j->session) && hop_monotonic_ms() < deadline);
    return 0;
}
static void *switch_worker(void *arg) {
    mac_display_job *j = arg;
    int ok = 0;
    if (!atomic_load(&s_switch_stopping) && hop_session_matches(&j->session)) {
        if (j->msg.op == HOP_DISPLAY_WAKE) {
            ok = wake_ready(j);
        } else if (j->msg.op == HOP_DISPLAY_INPUT) {
            char cmd[224];
            snprintf(cmd, sizeof(cmd), "%s %u", j->command, j->msg.value);
            ok = run_shell_command(cmd, &j->session, j->timeout_ms);
        } else if (j->msg.op == HOP_DISPLAY_SLEEP) {
            disp_wake_clear();
            ok = run_shell_command(j->sleep_cmd, &j->session, j->timeout_ms);
        }
    }
    if (!atomic_load(&s_switch_stopping) && hop_session_matches(&j->session))
        send_result(&j->session, j->msg.id, ok);
    hop_log(ok ? HOP_LOG_INFO : HOP_LOG_ERROR, "display: peer operation=%u id=%u result=%d",
        j->msg.op, j->msg.id, ok);
    free(j);
    pthread_mutex_lock(&s_switch_lock); s_switch_busy = 0; pthread_mutex_unlock(&s_switch_lock);
    return NULL;
}
void disp_switch_message(const uint8_t *p, uint32_t len) {
    hop_display_msg msg;
    if (atomic_load(&s_switch_stopping) || !hop_display_decode(&msg, p, len)) return;
    if (msg.op == HOP_DISPLAY_WAKE_CLEAR) {
        disp_wake_clear();
        return;
    }
    if (msg.op == HOP_DISPLAY_LOCAL) {
        disp_set_input(msg.value == HOP_DISPLAY_MAC ? g_mac.cfg.disp_mac_input : g_mac.cfg.disp_win_input);
        set_switch_status("普通模式：已提交本机输入源切换；未修改 Windows 显示拓扑");
        return;
    }
    if (msg.op == HOP_DISPLAY_COMPLETE) {
        if (msg.ok) {
            set_switch_status(msg.value == HOP_DISPLAY_MAC
                ? "Windows 已确认：共用屏已交给 Mac"
                : "Windows 已确认：扩展屏与输入源已恢复");
        } else if (msg.value == HOP_DISPLAY_AUTO) {
            set_switch_status("一键共屏切换未执行：请先在 Windows 设置中开启共屏模式并检查连接");
        } else {
            set_switch_status("共屏切换失败，请检查 Windows 运行日志与共屏设置");
        }
        return;
    }
    if (msg.op != HOP_DISPLAY_WAKE && msg.op != HOP_DISPLAY_INPUT && msg.op != HOP_DISPLAY_SLEEP) return;
    hop_session_id session;
    if (!hop_session_current(&session)) return;
    mac_display_job *j = calloc(1, sizeof(*j));
    if (!j) { send_result(&session, msg.id, 0); return; }
    j->msg = msg;
    j->session = session;
    j->timeout_ms = clamp_int(g_mac.cfg.disp_wake_timeout_ms, 1000, 30000);
    j->poll_ms = clamp_int(g_mac.cfg.disp_poll_ms, 50, 2000);
    snprintf(j->command, sizeof(j->command), "%s", g_mac.cfg.mac_input_cmd);
    snprintf(j->sleep_cmd, sizeof(j->sleep_cmd), "%s", g_mac.cfg.mac_sleep_cmd);
    pthread_mutex_lock(&s_switch_lock);
    if (s_switch_busy) {
        pthread_mutex_unlock(&s_switch_lock);
        free(j);
        send_result(&session, msg.id, 0);
        return;
    }
    if (s_switch_started) { pthread_join(s_switch_thread, NULL); s_switch_started = 0; }
    s_switch_busy = 1;
    int rc = pthread_create(&s_switch_thread, NULL, switch_worker, j);
    s_switch_started = rc == 0;
    if (rc) { s_switch_busy = 0; free(j); }
    pthread_mutex_unlock(&s_switch_lock);
    if (rc) send_result(&session, msg.id, 0);
}
int disp_switch_target(int target) {
    if (target != HOP_DISPLAY_MAC && target != HOP_DISPLAY_WINDOWS && target != HOP_DISPLAY_AUTO) return 0;
    uint8_t wire[HOP_DISPLAY_WIRE_SIZE];
    hop_display_msg msg = {HOP_DISPLAY_REQUEST, 0, 0, (uint32_t)target};
    hop_display_encode(wire, &msg);
    if (net_send(HOP_MSG_DISP_SWITCH, wire, sizeof(wire))) {
        set_switch_status("已提交切屏请求，等待 Windows 确认；详情见 Windows 概览与日志");
        hop_log(HOP_LOG_INFO, "display: requested Windows coordinator target=%d (pending)", target);
        return 1;
    }
    if (target == HOP_DISPLAY_MAC || target == HOP_DISPLAY_WINDOWS) {
        disp_set_input(target == HOP_DISPLAY_MAC ? g_mac.cfg.disp_mac_input : g_mac.cfg.disp_win_input);
        set_switch_status("Windows 未连接：已执行本机输入源切换（未修改 Windows 显示拓扑）");
        hop_log(HOP_LOG_WARN, "display: Windows peer unavailable; fell back to local Mac input switch (target=%d)", target);
        return 1;
    }
    set_switch_status("Windows 未连接：无法自动判定共屏状态，请先连接或直接点击目标输入源");
    hop_log(HOP_LOG_ERROR, "display: Windows peer unavailable; cannot resolve auto display toggle");
    return 0;
}
void disp_switch_shutdown(void) {
    atomic_store(&s_switch_stopping, 1);
    pthread_mutex_lock(&s_switch_lock);
    int join = s_switch_started; s_switch_started = 0;
    pthread_mutex_unlock(&s_switch_lock);
    if (join) pthread_join(s_switch_thread, NULL);
}
