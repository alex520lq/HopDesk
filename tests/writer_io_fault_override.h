/* Force-included only while compiling the real writer IO adapter for queue tests. */
#ifdef _WIN32
#include <windows.h>
HANDLE WINAPI hop_test_CreateFileW(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
BOOL WINAPI hop_test_WriteFile(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
#define CreateFileW hop_test_CreateFileW
#define WriteFile hop_test_WriteFile
#else
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
int hop_test_open(const char *, int, mode_t);
ssize_t hop_test_write(int, const void *, size_t);
#define open hop_test_open
#define write hop_test_write
#endif
