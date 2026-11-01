/* Fail-stop containment for blocking OS calls. Never cancel a thread while it
 * owns file/allocator locks. Exit 70 lets the optional supervisor restart cleanly.
 * No logging or cleanup on the fatal path: those may be what is hung. */
#include "hop.h"
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
#include <unistd.h>
#include <time.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif
#define GUARDS 64
static struct { uint64_t deadline; unsigned timeout; const char *operation; } s_guards[GUARDS];
static int s_started;
#if defined(_WIN32)
static DWORD WINAPI monitor(LPVOID unused) {
#else
static void *monitor(void *unused) {
#endif
    (void)unused;
    uint64_t last = hop_monotonic_ms();
    for (;;) {
#if defined(_WIN32)
        Sleep(250);
#else
        struct timespec pause = {0, 250000000};
        nanosleep(&pause, NULL);
#endif
        uint64_t now = hop_monotonic_ms();
        int fatal = 0;
        LOCK();
        for (int i = 0; i < GUARDS; ++i) {
            if (!s_guards[i].deadline) continue;
            /* System sleep / debugger suspension pauses all workers together.
             * A blocked worker alone does not pause this monitor. */
            if (now - last > 2000) s_guards[i].deadline += now - last;
            if (now >= s_guards[i].deadline) fatal = 1;
        }
        UNLOCK(); last = now;
        if (fatal) {
#if defined(_WIN32)
            TerminateProcess(GetCurrentProcess(), HOP_WATCHDOG_EXIT);
#else
            _exit(HOP_WATCHDOG_EXIT);
#endif
        }
    }
    return 0;
}
int hop_watchdog_start(void) {
    LOCK();
    if (s_started) { UNLOCK(); return 1; }
#if defined(_WIN32)
    HANDLE thread = CreateThread(NULL, 0, monitor, NULL, 0, NULL);
    if (!thread) { UNLOCK(); return 0; }
    CloseHandle(thread);
#else
    pthread_t thread;
    if (pthread_create(&thread, NULL, monitor, NULL)) { UNLOCK(); return 0; }
    pthread_detach(thread);
#endif
    s_started = 1; UNLOCK(); return 1;
}
int hop_watchdog_begin(const char *operation, unsigned timeout_ms) {
    if (!hop_watchdog_start()) {
#if defined(_WIN32)
        TerminateProcess(GetCurrentProcess(), HOP_WATCHDOG_EXIT);
#else
        _exit(HOP_WATCHDOG_EXIT);
#endif
    }
    LOCK();
    for (int i = 0; i < GUARDS; ++i) if (!s_guards[i].deadline) {
        s_guards[i].operation = operation; s_guards[i].timeout = timeout_ms;
        s_guards[i].deadline = hop_monotonic_ms() + timeout_ms;
        UNLOCK(); return i + 1;
    }
    UNLOCK();
    /* Never silently run an unguarded operation on exhaustion. */
#if defined(_WIN32)
    TerminateProcess(GetCurrentProcess(), HOP_WATCHDOG_EXIT);
#else
    _exit(HOP_WATCHDOG_EXIT);
#endif
    return 0;
}
void hop_watchdog_ping(int ticket) {
    if (ticket <= 0 || ticket > GUARDS) return;
    LOCK();
    if (s_guards[ticket - 1].deadline)
        s_guards[ticket - 1].deadline = hop_monotonic_ms() + s_guards[ticket - 1].timeout;
    UNLOCK();
}
void hop_watchdog_end(int ticket) {
    if (ticket <= 0 || ticket > GUARDS) return;
    LOCK(); s_guards[ticket - 1].deadline = 0; UNLOCK();
}
