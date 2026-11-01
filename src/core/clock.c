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
#else
#include <time.h>
uint64_t hop_monotonic_ms(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)(ts.tv_nsec / 1000000);
}
#endif
