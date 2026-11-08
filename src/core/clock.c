/* =====================================================================
 *  HopDesk - 单调时钟（平台无关接口）
 *  心跳 / 握手超时都用它：墙上时钟会被 NTP、用户改时间、休眠唤醒拨动，
 *  拿它判"多久没收到数据"会误杀或永不超时。
 * ===================================================================== */
#include "hop.h"

#if defined(_WIN32)
#include <windows.h>
uint64_t hop_monotonic_ms(void) {
    return (uint64_t)GetTickCount64();
}
uint64_t hop_wallclock_ms(void) {
    FILETIME ft;
    ULARGE_INTEGER raw;
    GetSystemTimeAsFileTime(&ft);
    raw.LowPart = ft.dwLowDateTime;
    raw.HighPart = ft.dwHighDateTime;
    /* FILETIME 是 1601-01-01 起的 100ns 计数，换算成 Unix 纪元毫秒。 */
    if (raw.QuadPart < 116444736000000000ULL) return 0;
    return (raw.QuadPart - 116444736000000000ULL) / 10000ULL;
}
#else
#include <time.h>
uint64_t hop_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
uint64_t hop_wallclock_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0 || ts.tv_sec < 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif
