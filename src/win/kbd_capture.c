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
/* Physical state is tracked even while events are swallowed. This lets a
 * key held across an edge handoff be released locally and restored on return. */
static BYTE s_physical_down[256];
/* Synthetic local key-up can fail (for example UIPI/elevated foreground app).
 * Keep that state until the local OS has actually received the release. */
static BYTE s_local_release_pending[256], s_local_restore_pending[256];
static int s_kbd_sharing;
static int s_local_paste_keyup_suppressed;
static int is_mouse_vk(unsigned vk) {
    return vk==VK_LBUTTON||vk==VK_RBUTTON||vk==VK_MBUTTON||
           vk==VK_XBUTTON1||vk==VK_XBUTTON2;
}

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

/* Injected transitions are ignored by the hook so they reach the local OS. */
static int inject_local_key(WORD vk,int down) {
    INPUT in;memset(&in,0,sizeof(in));in.type=INPUT_KEYBOARD;in.ki.wVk=vk;
    if(!down)in.ki.dwFlags|=KEYEVENTF_KEYUP;
    if(vk==VK_RCONTROL||vk==VK_RMENU||vk==VK_LWIN||vk==VK_RWIN||
       (vk>=VK_PRIOR&&vk<=VK_DOWN)||vk==VK_INSERT||vk==VK_DELETE||vk==VK_DIVIDE)
        in.ki.dwFlags|=KEYEVENTF_EXTENDEDKEY;
    static DWORD last_warning;static int warned;
    if(SendInput(1,&in,sizeof(in))==1)return 1;
    DWORD now=GetTickCount();
    if(!warned || (DWORD)(now-last_warning)>5000) {
        hop_log(HOP_LOG_WARN,"keyboard: local synthetic %s failed (%lu); pending release/restore will be retried",
                down?"key-down":"key-up",(unsigned long)GetLastError());
        last_warning=now;warned=1;
    }
    return 0;
}

/* Release remote keys and clear the remote-held set. */
void kbd_release_all(void) {
    while (s_nheld > 0) {
        uint16_t kc = s_held[--s_nheld];
        send_key(kc, 0);
    }
}

/* Transfer any key already physically held when the pointer crosses the edge.
 * Local key-up is synthetic (LLKHF_INJECTED), so it does not alter the physical
 * snapshot maintained below. */
void kbd_begin_sharing(void) {
    s_kbd_sharing=1;
    for(unsigned vk=1;vk<256;++vk) {
        if(is_mouse_vk(vk)||!s_physical_down[vk])continue;
        uint16_t kc=hop_vk_to_mac((int)vk);
        if(kc!=HOP_KEY_NONE){send_key(kc,1);held_add(kc);}
        s_local_restore_pending[vk]=0;
        s_local_release_pending[vk]=(BYTE)!inject_local_key((WORD)vk,0);
    }
}

/* On return, release remote keys first, then restore keys still physically held. */
void kbd_end_sharing(void) {
    kbd_release_all();
    s_kbd_sharing=0;
    for(unsigned vk=1;vk<256;++vk) {
        if(is_mouse_vk(vk)||!s_physical_down[vk])continue;
        if(s_local_release_pending[vk]) {
            if(!inject_local_key((WORD)vk,0)) { s_local_restore_pending[vk]=1; continue; }
            s_local_release_pending[vk]=0;
        }
        s_local_restore_pending[vk]=(BYTE)!inject_local_key((WORD)vk,1);
    }
}

static LRESULT CALLBACK kbd_proc(int code, WPARAM wp, LPARAM lp) {
    KBDLLHOOKSTRUCT *k;
    uint16_t kc;
    int down;

    if (code != HC_ACTION) return CallNextHookEx(s_hook, code, wp, lp);
    s_hook_tick = GetTickCount();
    k = (KBDLLHOOKSTRUCT *)lp;
    if (k->flags & LLKHF_INJECTED) return CallNextHookEx(s_hook, code, wp, lp);

    down = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    if(k->vkCode<256) {
        s_physical_down[k->vkCode]=(BYTE)down;
        if(!down) {
            if(g_app.state==HOP_LINK_SHARING && s_local_release_pending[k->vkCode]) {
                if(inject_local_key((WORD)k->vkCode,0))s_local_release_pending[k->vkCode]=0;
            } else if(g_app.state!=HOP_LINK_SHARING) {
                /* The real key-up is about to reach the local OS. */
                s_local_release_pending[k->vkCode]=0;
                s_local_restore_pending[k->vkCode]=0;
            }
        }
    }
    if (g_app.state != HOP_LINK_SHARING) {
        if (!down && k->vkCode == 'V' && s_local_paste_keyup_suppressed) {
            s_local_paste_keyup_suppressed=0; return 1;
        }
        if (down && k->vkCode == 'V' &&
            (s_physical_down[VK_LCONTROL] || s_physical_down[VK_RCONTROL]) &&
            clip_request_remote_paste()) {
            s_local_paste_keyup_suppressed=1; return 1;
        }
        return CallNextHookEx(s_hook, code, wp, lp);
    }
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
    memset(s_physical_down,0,sizeof(s_physical_down));
    memset(s_local_release_pending,0,sizeof(s_local_release_pending));
    memset(s_local_restore_pending,0,sizeof(s_local_restore_pending));
    s_kbd_sharing=0;
    for(unsigned vk=1;vk<256;++vk)
        if(!is_mouse_vk(vk))s_physical_down[vk]=(GetAsyncKeyState((int)vk)&0x8000)!=0;
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, g_app.inst, 0);
    if (s_hook) hop_log(HOP_LOG_INFO, "keyboard hook installed");
    else        hop_log(HOP_LOG_ERROR, "keyboard hook failed: %lu", GetLastError());
}

void kbd_shutdown(void) {
    kbd_release_all();
    if (s_hook) { UnhookWindowsHookEx(s_hook); s_hook = NULL; }
}

DWORD kbd_hook_tick(void) { return s_hook_tick; }
/* A lost low-level-hook callback makes the cached physical snapshot unsafe:
 * a missed key-up would otherwise be replayed as a fresh local key-down on
 * the next handoff. During hook recovery prefer requiring a fresh key press
 * over restoring a possibly stale modifier state. */
void kbd_forget_physical_state(void) {
    memset(s_physical_down, 0, sizeof(s_physical_down));
    memset(s_local_restore_pending, 0, sizeof(s_local_restore_pending));
    /* Do not clear s_local_release_pending: the local OS may still have a
     * logically-down key. Retry its key-up even after the physical snapshot
     * has been discarded. */
}

void kbd_retry_pending(void) {
    for(unsigned vk=1;vk<256;++vk) {
        if(s_local_release_pending[vk] && inject_local_key((WORD)vk,0))
            s_local_release_pending[vk]=0;
        if(s_local_restore_pending[vk]) {
            if(!s_physical_down[vk])s_local_restore_pending[vk]=0;
            else if(!s_kbd_sharing && !s_local_release_pending[vk] &&
                    inject_local_key((WORD)vk,1))s_local_restore_pending[vk]=0;
        }
    }
}

void kbd_reinstall(void) {
    kbd_release_all();
    kbd_forget_physical_state();
    if (s_hook) UnhookWindowsHookEx(s_hook);
    s_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kbd_proc, g_app.inst, 0);
    s_hook_tick = GetTickCount();
    if (!s_hook) hop_log(HOP_LOG_ERROR, "capture: keyboard hook reinstall failed: %lu", GetLastError());
}
