/* Native writer file-IO adapter tests. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#include "../src/win/xfer_writer_io.h"
int wmain(void) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    DWORD n=GetTempPathW(MAX_PATH,dir);
    if(!n||n>=MAX_PATH||!GetTempFileNameW(dir,L"hdw",0,path)) return 1;
    DeleteFileW(path);
    hop_writer_io io; hop_writer_io_init(&io);
    unsigned long error=0;
    const uint8_t first[]={1,2,3,4}, second[]={9,8};
    if(!hop_writer_io_open(&io,path,&error)) return 2;
    if(!hop_writer_io_write_at(&io,0,first,sizeof(first),&error)) return 3;
    if(!hop_writer_io_write_at(&io,1,second,sizeof(second),&error)) return 4;
    hop_writer_io_close(&io);
    HANDLE f=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,NULL);
    uint8_t actual[4]={0}; DWORD got=0;
    int ok=f!=INVALID_HANDLE_VALUE&&ReadFile(f,actual,4,&got,NULL)&&got==4;
    if(f!=INVALID_HANDLE_VALUE)CloseHandle(f);
    DeleteFileW(path);
    if(!ok||actual[0]!=1||actual[1]!=9||actual[2]!=8||actual[3]!=4)return 5;
    puts("PASS: Windows writer IO open/write-at/close");
    return 0;
}
#else
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include "../src/mac/xfer_writer_io.h"
int main(void) {
    char path[]="/tmp/hopdesk-writer-io-XXXXXX";
    int seed=mkstemp(path); if(seed<0)return 1; close(seed); unlink(path);
    hop_writer_io io; hop_writer_io_init(&io);
    int error=0;
    const uint8_t first[]={1,2,3,4}, second[]={9,8};
    if(!hop_writer_io_open(&io,path,&error))return 2;
    if(!hop_writer_io_write_at(&io,0,first,sizeof(first),&error))return 3;
    if(!hop_writer_io_write_at(&io,1,second,sizeof(second),&error))return 4;
    hop_writer_io_close(&io);
    int f=open(path,O_RDONLY); uint8_t actual[4]={0};
    ssize_t got=f>=0?read(f,actual,sizeof(actual)):-1;
    if(f>=0)close(f); unlink(path);
    if(got!=4||actual[0]!=1||actual[1]!=9||actual[2]!=8||actual[3]!=4)return 5;
    puts("PASS: macOS writer IO open/write-at/close");
    return 0;
}
#endif
