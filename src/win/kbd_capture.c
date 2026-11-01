/* =====================================================================
 *  HopDesk - Windows 键盘捕获（阶段 2）
 *
 *  WH_KEYBOARD_LL 全局低级钩子：
 *    仅当 g_app.state == HOP_LINK_SHARING 时生效：
 *      - 一切按键都吞掉（含 Win 键、Alt 菜单、Alt+Tab，绝不漏到本机）
 *      - VK -> kVK 映射后转发；映射不出的键直接丢弃
 *
 *  为什么修饰键也当普通键转发：
 *    物理 Ctrl 映射成 Mac 的 Command 键码，按下去就是 Command 按下。
 *    这样 Ctrl+C 天然变成 Cmd+C，不需要维护一套额外的 flags 状态机，
 *    也不需要 KEY_MODS —— 特殊情况被数据结构消掉了。
 *
 *  唯一需要额外处理的是"交还键鼠时还有键按着"：
 *    Windows 那边按下的键，Mac 侧可能只收到了 keydown 没收到 keyup。
 *    所以这里记录已发送的按下键，handback 时补发 keyup。
 * ===================================================================== */
#include "app.h"

#define KBD_HELD_MAX 256

static HHOOK    s_hook;
static DWORD s_hook_tick;
static uint16_t s_held[KBD_HELD_MAX];
static int      s_nheld;

static void send_key(uint16_t kc, int down) {
    uint8_t p[2];
    hop_put_be16(p, kc);
    net_server_send(g_app.net, down ? HOP_MSG_KEY_DOWN : HOP_MSG_KEY_UP, p, 2);
}

static void held_add(uint16_t kc) {
    int i;
    for (i = 0; i < s_nheld; ++i)
        if (s_held[i] == kc) return;          /* 自动重复：已经在册 */
    if (s_nheld < KBD_HELD_MAX) s_held[s_nheld++] = kc;
}

static void held_del(uint16_t kc) {
    int i;
    for (i = 0; i < s_nheld; ++i) {
        if (s_held[i] == kc) {
            s_held[i] = s_held[--s_nheld];
            return;
        }
    }
}

/* 交还键鼠时调用：把仍然按着的键在对端松开，避免 Mac 上卡住 Cmd/Shift */
void kbd_release_all(void) {
    while (s_nheld > 0) {
        uint16_t kc = s_held[--s_nheld];
        send_key(kc, 0);
    }
}

static LRESULT CALLBACK kbd_proc(int code, WPARAM wp, LPARAM lp) {
    KBDLLHOOKSTRUCT *k;
    uint16_t kc;
    int down;

    if (code != HC_ACTION) return CallNextHookEx(s_hook, code, wp, lp);
    s_hook_tick = GetTickCount();
    if (g_app.state != HOP_LINK_SHARING) return CallNextHookEx(s_hook, code, wp, lp);

    k = (KBDLLHOOKSTRUCT *)lp;
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(s_hook, code, wp, lp);

    down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    if(down && k->vkCode==VK_ESCAPE && (GetAsyncKeyState(VK_CONTROL)&0x8000) && (GetAsyncKeyState(VK_MENU)&0x8000)){capture_set_sharing(0);return 1;}
    kc = hop_vk_to_mac((int)k->vkCode);
    if (kc != HOP_KEY_NONE) {
        send_key(kc, down);
        if (down) held_add(kc);
        else      held_del(kc);
    }
    return 1;   /* 共享期间吞掉一切按键 */
}

void kbd_init(win_app *app) {
    (void)app;
    s_nheld = 0;
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, g_app.inst, 0);
    if (s_hook) hop_log(HOP_LOG_INFO, "keyboard hook installed");
    else        hop_log(HOP_LOG_ERROR, "keyboard hook failed: %lu", GetLastError());
}

void kbd_shutdown(void) {
    kbd_release_all();
    if (s_hook) { UnhookWindowsHookEx(s_hook); s_hook = NULL; }
}

DWORD kbd_hook_tick(void) { return s_hook_tick; }
void kbd_reinstall(void) {
    kbd_release_all();
    if (s_hook) UnhookWindowsHookEx(s_hook);
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, g_app.inst, 0);
    s_hook_tick = GetTickCount();
    if (!s_hook) hop_log(HOP_LOG_ERROR, "capture: keyboard hook reinstall failed: %lu", GetLastError());
}
