#include "update_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
static SRWLOCK s_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_changed = CONDITION_VARIABLE_INIT;
#define WAIT() SleepConditionVariableSRW(&s_changed,&s_lock,INFINITE,0)
#define WAKE() WakeAllConditionVariable(&s_changed)
#define LOCK() AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#else
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_changed = PTHREAD_COND_INITIALIZER;
#define WAIT() pthread_cond_wait(&s_changed,&s_lock)
#define WAKE() pthread_cond_broadcast(&s_changed)
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#endif
static unsigned s_work;
static int s_installing; /* 0=open, 1=busy probe, 2=committed maintenance */
int hop_update_work_enter(void) {
    LOCK();
    int ok = !s_installing;
    if (ok) ++s_work;
    UNLOCK(); return ok;
}
int hop_update_receive_enter(void) {
    LOCK();
    /* A failed busy probe must not disconnect an existing active transfer.
     * Wait for its bounded queue/status inspection to commit or reopen. */
    while (s_installing == 1) WAIT();
    int ok = !s_installing;
    if (ok) ++s_work;
    UNLOCK(); return ok;
}
void hop_update_work_leave(void) {
    LOCK(); if (s_work) --s_work; UNLOCK();
}
int hop_update_install_begin(int (*busy)(void)) {
    LOCK();
    int ok = !s_installing && !s_work;
    if (ok) s_installing = 1;
    UNLOCK();
    if (!ok) return 0;
    int is_busy = !busy || busy();
    LOCK();
    s_installing = is_busy ? 0 : 2;
    WAKE();
    UNLOCK();
    return !is_busy;
}
void hop_update_install_end(void) {
    LOCK(); s_installing = 0; WAKE(); UNLOCK();
}
static int nonce_valid(const char *s) {
    if (!s || strlen(s) != 32) return 0;
    for (int i=0;i<32;i++)
        if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f'))) return 0;
    return 1;
}
void hop_update_report_ready(const char *version) {
#if defined(_WIN32)
    wchar_t path[32768], temp[32768];
    char nonce[64], expected[64], receipt[160];
    DWORD n=GetEnvironmentVariableW(L"HOPDESK_UPDATE_READY",path,32768);
    DWORD a=GetEnvironmentVariableA("HOPDESK_UPDATE_NONCE",nonce,sizeof(nonce));
    DWORD b=GetEnvironmentVariableA("HOPDESK_UPDATE_VERSION",expected,sizeof(expected));
    if (!n || n>=32760 || !a || a>=sizeof(nonce) || !b || b>=sizeof(expected) ||
        !nonce_valid(nonce) || !version || strcmp(expected,version)) return;
    _snwprintf(temp,32768,L"%ls.tmp",path);
    int len=snprintf(receipt,sizeof(receipt),"%s\n%s\n%lu\n",nonce,version,GetCurrentProcessId());
    HANDLE f=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,NULL);
    if (f==INVALID_HANDLE_VALUE) return;
    DWORD wrote=0;
    int ok=WriteFile(f,receipt,(DWORD)len,&wrote,NULL) && wrote==(DWORD)len && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok=MoveFileExW(temp,path,MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0;
    if (!ok) DeleteFileW(temp);
    else {
        SetEnvironmentVariableW(L"HOPDESK_UPDATE_READY",NULL);
        SetEnvironmentVariableW(L"HOPDESK_UPDATE_NONCE",NULL);
        SetEnvironmentVariableW(L"HOPDESK_UPDATE_VERSION",NULL);
    }
#else
    const char *path=getenv("HOPDESK_UPDATE_READY");
    const char *nonce=getenv("HOPDESK_UPDATE_NONCE");
    const char *expected=getenv("HOPDESK_UPDATE_VERSION");
    char temp[4096];
    if (!path || !nonce_valid(nonce) || !expected || !version || strcmp(expected,version) ||
        snprintf(temp,sizeof(temp),"%s.tmp",path)>=(int)sizeof(temp)) return;
    int fd=open(temp,O_WRONLY|O_CREAT|O_EXCL,0600);
    if (fd<0) return;
    FILE *f=fdopen(fd,"wb");
    if (!f) { close(fd); unlink(temp); return; }
    int ok=fprintf(f,"%s\n%s\n%ld\n",nonce,version,(long)getpid())>0;
    if (fflush(f) || fsync(fd)) ok=0;
    if (fclose(f)) ok=0;
    if (ok && rename(temp,path)==0) {
        unsetenv("HOPDESK_UPDATE_READY"); unsetenv("HOPDESK_UPDATE_NONCE"); unsetenv("HOPDESK_UPDATE_VERSION");
    } else unlink(temp);
#endif
}
