/* One control-owned session registry per process. Data sockets can only bind to
 * a published session. Invalidation compares the ID, so an old reader cannot
 * tear down a replacement. No caller may read mutable registry fields directly. */
#include "hop.h"
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif
static hop_session_id s_current;
static uint64_t s_generation;
int hop_session_generate(hop_session_id *out) {
    if (!out) return 0;
#if defined(_WIN32)
    typedef BOOLEAN (WINAPI *random_fn)(PVOID, ULONG);
    HMODULE dll = LoadLibraryW(L"advapi32.dll");
    if (!dll) return 0;
    random_fn random = (random_fn)GetProcAddress(dll, "SystemFunction036");
    int ok = random && random(out->bytes, HOP_SESSION_BYTES);
    FreeLibrary(dll);
    if (!ok) return 0;
#else
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) return 0;
    size_t have = 0;
    while (have < HOP_SESSION_BYTES) {
        ssize_t n = read(fd, out->bytes + have, HOP_SESSION_BYTES - have);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); return 0; }
        have += (size_t)n;
    }
    close(fd);
#endif
    return hop_session_id_valid(out);
}
void hop_session_publish(const hop_session_id *id) {
    if (!hop_session_id_valid(id)) return;
    LOCK(); s_current = *id; ++s_generation; UNLOCK();
}
int hop_session_current(hop_session_id *out) {
    LOCK();
    if (out) *out = s_current;
    int ready = hop_session_id_valid(&s_current);
    UNLOCK(); return ready;
}
int hop_session_matches(const hop_session_id *id) {
    LOCK(); int matches = hop_session_id_equal(id, &s_current); UNLOCK();
    return matches;
}
void hop_session_clear(const hop_session_id *expected) {
    LOCK();
    if (hop_session_id_equal(expected, &s_current)) {
        memset(&s_current, 0, sizeof(s_current)); ++s_generation;
    }
    UNLOCK();
}
uint64_t hop_session_generation(void) {
    LOCK(); uint64_t value = s_generation; UNLOCK(); return value;
}
