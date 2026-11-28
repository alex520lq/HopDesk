/* =====================================================================
 *  HopDesk - Windows 显示切换（阶段 4）
 *
 *  DDC/CI VCP 0x60（Input Source）切显示器输入源：
 *    取代原来"SSH 到 Mac 跑脚本"的链路 —— 现在由常驻进程直接调 dxva2。
 *
 *  dxva2.dll 用 GetProcAddress 动态加载，不依赖 SDK 头文件：
 *    physicalmonitorenumerationapi.h / highlevelmonitorconfigurationapi.h
 *    在部分 MinGW 发行版里缺失，静态链接会直接编译不过。
 *    动态加载还顺带解决了"机器没有 dxva2 时不要链接期失败"。
 *
 *  只操作主显示器（含坐标 0,0 的那台）。多显示器场景下"哪台接了两路输入"
 *  是人的知识，不是程序能猜的 —— 真要支持再加配置项，别现在过度设计。
 * ===================================================================== */
#include "app.h"

#include <stdlib.h>

/* ---- dxva2 需要的最小类型（照 dxva2api.h 抄，避免头文件依赖） ---- */
typedef struct {
    HANDLE hPhysicalMonitor;
    WCHAR  szPhysicalMonitorDescription[128];
} PHYS_MON;

typedef BOOL (WINAPI *fn_getnum)(HMONITOR, LPDWORD);
typedef BOOL (WINAPI *fn_getphys)(HMONITOR, DWORD, PHYS_MON *);
typedef BOOL (WINAPI *fn_destroy)(DWORD, PHYS_MON *);
typedef BOOL (WINAPI *fn_setvcp)(HANDLE, BYTE, DWORD);
typedef BOOL (WINAPI *fn_getvcp)(HANDLE, BYTE, LPDWORD, LPDWORD, LPDWORD);

#define VCP_INPUT_SOURCE 0x60

static HMODULE   s_dll;
static fn_getnum  p_getnum;
static fn_getphys p_getphys;
static fn_destroy p_destroy;
static fn_setvcp  p_setvcp;
static fn_getvcp  p_getvcp;

static int dx_load(void) {
    if (s_dll) return p_getnum && p_getphys && p_destroy && p_setvcp;
    s_dll = LoadLibraryW(L"dxva2.dll");
    if (!s_dll) {
        hop_log(HOP_LOG_ERROR, "dxva2.dll not found, DDC unavailable");
        return 0;
    }
    p_getnum  = (fn_getnum) GetProcAddress(s_dll, "GetNumberOfPhysicalMonitorsFromHMONITOR");
    p_getphys = (fn_getphys)GetProcAddress(s_dll, "GetPhysicalMonitorsFromHMONITOR");
    p_destroy = (fn_destroy)GetProcAddress(s_dll, "DestroyPhysicalMonitors");
    p_setvcp  = (fn_setvcp) GetProcAddress(s_dll, "SetVCPFeature");
    p_getvcp  = (fn_getvcp) GetProcAddress(s_dll, "GetVCPFeatureAndVCPFeatureReply");
    return p_getnum && p_getphys && p_destroy && p_setvcp;
}

/* 取目标显示器的物理监视器句柄表；disp_monitor_index=0 为主屏，1..N 为枚举序号。调用方负责 p_destroy + free */
struct disp_pick_ctx {
    int target;
    int current;
    HMONITOR picked;
};

static BOOL CALLBACK disp_pick_proc(HMONITOR mon, HDC hdc, LPRECT rc, LPARAM lp) {
    (void)hdc; (void)rc;
    struct disp_pick_ctx *ctx = (struct disp_pick_ctx *)lp;
    ++ctx->current;
    if (ctx->current == ctx->target) {
        ctx->picked = mon;
        return FALSE;
    }
    return TRUE;
}

struct device_pick { const wchar_t *name; HMONITOR mon; };
static BOOL CALLBACK device_pick_proc(HMONITOR mon, HDC dc, LPRECT rc, LPARAM arg) {
    (void)dc; (void)rc;
    struct device_pick *p = (struct device_pick *)arg;
    MONITORINFOEXW info = {0}; info.cbSize = sizeof(info);
    if (GetMonitorInfoW(mon, (MONITORINFO *)&info) && !wcscmp(info.szDevice, p->name)) {
        p->mon = mon; return FALSE;
    }
    return TRUE;
}
static DWORD open_target(const wchar_t *device, PHYS_MON **out) {
    POINT pt;
    HMONITOR mon = NULL;
    DWORD n = 0;
    PHYS_MON *pm;

    *out = NULL;
    if (!dx_load()) return 0;

    if (device) {
        struct device_pick pick = {device, NULL};
        EnumDisplayMonitors(NULL, NULL, device_pick_proc, (LPARAM)&pick);
        mon = pick.mon;
        if (!mon) return 0; /* Never silently operate on the primary instead. */
    } else if (g_app.cfg.disp_monitor_index > 0) {
        struct disp_pick_ctx ctx = { g_app.cfg.disp_monitor_index, 0, NULL };
        EnumDisplayMonitors(NULL, NULL, disp_pick_proc, (LPARAM)&ctx);
        mon = ctx.picked;
    }
    if (!mon) {
        pt.x = 0; pt.y = 0;
        mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    }
    if (!mon || !p_getnum(mon, &n) || n == 0) return 0;

    pm = (PHYS_MON *)calloc(n, sizeof(*pm));
    if (!pm) return 0;
    if (!p_getphys(mon, n, pm)) {
        free(pm);
        return 0;
    }
    *out = pm;
    return n;
}

static int set_target_input_unlocked(const wchar_t *device, uint16_t vcp) {
    PHYS_MON *pm = NULL;
    DWORD n = open_target(device, &pm);
    DWORD i;
    int ok = 0;

    if (n == 0) {
        hop_log(HOP_LOG_WARN, "DDC: no physical monitor");
        return 0;
    }
    for (i = 0; i < n; ++i) {
        if (p_setvcp(pm[i].hPhysicalMonitor, VCP_INPUT_SOURCE, (DWORD)vcp))
            ok = 1;
        else
            hop_log(HOP_LOG_WARN, "DDC: SetVCPFeature failed on monitor %lu (err %lu)",
                    (unsigned long)i, (unsigned long)GetLastError());
    }
    p_destroy(n, pm);
    free(pm);
    hop_log(ok ? HOP_LOG_INFO : HOP_LOG_ERROR,
            "DDC: input source -> 0x%02x (%s)", (unsigned)vcp, ok ? "ok" : "failed");
    return ok;
}

static int get_target_input_unlocked(const wchar_t *device, uint16_t *out) {
    PHYS_MON *pm = NULL;
    DWORD n = open_target(device, &pm);
    DWORD cur = 0, max = 0, vct = 0;
    int ok = 0;

    if (n == 0) return 0;
    if (p_getvcp &&
        p_getvcp(pm[0].hPhysicalMonitor, VCP_INPUT_SOURCE,
                 &vct, &cur, &max)) {
        *out = (uint16_t)cur;
        ok = 1;
    }
    p_destroy(n, pm);
    free(pm);
    return ok;
}

static SRWLOCK s_ddc_lock = SRWLOCK_INIT;
static int set_target_input(const wchar_t *device, uint16_t vcp) {
    AcquireSRWLockExclusive(&s_ddc_lock);
    int ok = set_target_input_unlocked(device, vcp);
    ReleaseSRWLockExclusive(&s_ddc_lock);
    return ok;
}
static int get_target_input(const wchar_t *device, uint16_t *out) {
    AcquireSRWLockExclusive(&s_ddc_lock);
    int ok = get_target_input_unlocked(device, out);
    ReleaseSRWLockExclusive(&s_ddc_lock);
    return ok;
}

/* Only one DDC worker. A blocked monitor must never stall low-level hooks.
 * Coalesce pending requests to the most recent desired input. */
static SRWLOCK s_work_lock = SRWLOCK_INIT;
static int s_working, s_pending;
static uint16_t s_desired;
static DWORD WINAPI display_worker(LPVOID arg) {
    (void)arg;
    for (;;) {
        AcquireSRWLockExclusive(&s_work_lock);
        if (!s_pending) {
            s_working = 0;
            ReleaseSRWLockExclusive(&s_work_lock);
            return 0;
        }
        uint16_t vcp = s_desired; s_pending = 0;
        ReleaseSRWLockExclusive(&s_work_lock);
        int guard = hop_watchdog_begin("DDC operation", HOP_IO_DEADLINE_MS);
        set_target_input(NULL, vcp);
        hop_watchdog_end(guard);
    }
}
int disp_set_input(uint16_t vcp) {
    AcquireSRWLockExclusive(&s_work_lock);
    s_desired = vcp; s_pending = 1;
    if (!s_working) {
        s_working = 1;
        HANDLE t = CreateThread(NULL, 0, display_worker, NULL, 0, NULL);
        if (!t) { s_working = s_pending = 0; ReleaseSRWLockExclusive(&s_work_lock); return 0; }
        CloseHandle(t);
    }
    ReleaseSRWLockExclusive(&s_work_lock);
    return 1; /* accepted, not proof that the monitor switched */
}

int disp_get_input(uint16_t *out) { return get_target_input(NULL, out); }
int disp_input_read(const wchar_t *device, uint16_t *out) {
    return out ? get_target_input(device, out) : 0;
}
static int vcp_matches(uint16_t current, uint16_t want) {
    return current == want || (want <= 0xFFu && (current & 0xFFu) == want);
}
int disp_input_matches(const wchar_t *device, uint16_t vcp) {
    uint16_t current;
    return get_target_input(device, &current) && vcp_matches(current, vcp);
}
/* Worker only. Re-enumerate handles on every read after a source/topology change. */
int disp_input_verified(const wchar_t *device, uint16_t vcp,
                        int retries, int timeout_ms, int poll_ms) {
    if (retries < 1) retries = 1;
    if (retries > 10) retries = 10;
    if (timeout_ms < 1000) timeout_ms = 1000;
    if (timeout_ms > 30000) timeout_ms = 30000;
    if (poll_ms < 50) poll_ms = 50;
    if (poll_ms > 2000) poll_ms = 2000;

    if (disp_input_matches(device, vcp)) return 1;
    int wrote = 0;
    for (int attempt = 0; attempt < retries; ++attempt) {
        if (set_target_input(device, vcp)) { wrote = 1; break; }
        if (attempt + 1 < retries) Sleep(800);
    }
    if (!wrote) return 0;
    DWORD start = GetTickCount();
    do {
        if (disp_input_matches(device, vcp)) return 1;
        Sleep((DWORD)poll_ms);
    } while ((DWORD)(GetTickCount() - start) < (DWORD)timeout_ms);
    return 0;
}

int disp_input_busy(void) {
    AcquireSRWLockExclusive(&s_work_lock);
    int busy = s_working;
    ReleaseSRWLockExclusive(&s_work_lock);
    return busy;
}
