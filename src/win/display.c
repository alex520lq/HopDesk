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
    if (s_dll) return p_setvcp != NULL;
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

/* 取主显示器的物理监视器句柄表；调用方负责 p_destroy + free */
static DWORD open_primary(PHYS_MON **out) {
    POINT pt;
    HMONITOR mon;
    DWORD n = 0;
    PHYS_MON *pm;

    *out = NULL;
    if (!dx_load()) return 0;

    pt.x = 0; pt.y = 0;
    mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
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

static int disp_set_input_sync(uint16_t vcp) {
    PHYS_MON *pm = NULL;
    DWORD n = open_primary(&pm);
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

int disp_get_input(uint16_t *out) {
    PHYS_MON *pm = NULL;
    DWORD n = open_primary(&pm);
    DWORD cur = 0, max = 0;
    int vct = 0;
    int ok = 0;

    if (n == 0) return 0;
    if (p_getvcp &&
        p_getvcp(pm[0].hPhysicalMonitor, VCP_INPUT_SOURCE,
                 (LPDWORD)&vct, &cur, &max)) {
        *out = (uint16_t)cur;
        ok = 1;
    }
    p_destroy(n, pm);
    free(pm);
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
        disp_set_input_sync(vcp);
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
