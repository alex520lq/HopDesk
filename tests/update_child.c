/* Native Windows test executable used by test-update-windows.ps1. */
#include "update_state.h"
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#ifndef TEST_UPDATE_MODE
#define TEST_UPDATE_MODE 0
#endif
int main(void) {
    const char *fixture=getenv("HOPDESK_TEST_CHILD_PID");
    if(fixture) {
        FILE *f=fopen(fixture,"wb");
        if(f) { fprintf(f,"%lu",GetCurrentProcessId());fclose(f); }
    }
    if(TEST_UPDATE_MODE==1) return 7; /* new process crashes before UI */
    if(TEST_UPDATE_MODE==2) return 0; /* known good old executable */
    if(TEST_UPDATE_MODE==3) SetEnvironmentVariableA("HOPDESK_UPDATE_NONCE","bad");
    hop_update_report_ready("0.5.1");
    if(TEST_UPDATE_MODE==3) return 8; /* forged/wrong nonce, no valid receipt */
    if(TEST_UPDATE_MODE==4) { Sleep(500); return 9; } /* crash after receipt */
    Sleep(60000);
    return 0;
}
