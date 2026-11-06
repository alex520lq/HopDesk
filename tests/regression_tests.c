#include "hop.h"
#include "xfer_tracking.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <errno.h>
static int fault_write, fault_read, read_error, fault_close, fault_rename;
static size_t copy_write(const void*p,size_t s,size_t n,FILE*f){
    if(fault_write){errno=ENOSPC;return n?n-1:0;}return fwrite(p,s,n,f);
}
static size_t copy_read(void*p,size_t s,size_t n,FILE*f){
    if(fault_read){read_error=1;errno=EIO;return 0;}return fread(p,s,n,f);
}
static int copy_error(FILE*f){return read_error||ferror(f);}
static int copy_close(FILE*f){int rc=fclose(f);if(fault_close){errno=EIO;return EOF;}return rc;}
static int copy_rename(const char*a,const char*b){if(fault_rename){errno=EACCES;return -1;}return rename(a,b);}
#define fwrite copy_write
#define fread copy_read
#define ferror copy_error
#define fclose copy_close
#define rename copy_rename
#include "config_migrate.h"
#undef fwrite
#undef fread
#undef ferror
#undef fclose
#undef rename
#endif
static hop_xfer_tracking send_state, receive_state;
static void put(const char*p,const char*s){FILE*f=fopen(p,"wb");assert(f);assert(fwrite(s,1,strlen(s),f)==strlen(s));assert(fclose(f)==0);}
static void check_file(const char*p,const char*s){char b[256]={0};FILE*f=fopen(p,"rb");assert(f);assert(fread(b,1,sizeof(b)-1,f)==strlen(s));assert(fclose(f)==0);assert(strcmp(b,s)==0);}
int main(void){
    assert(hop_xfer_job_total(1,0)==1);
    assert(hop_xfer_job_total(0,3)==3);
    /* Same numeric ID, two directions. Terminal updates cannot cross tasks. */
    hop_xfer_tracking_begin(&send_state,7,HOP_XFER_DIR_SEND,1,0,"out",HOP_XFER_SENDING);
    hop_xfer_tracking_begin(&receive_state,7,HOP_XFER_DIR_RECEIVE,0,0,"in",HOP_XFER_RECEIVING);
    hop_xfer_tracking_file(&send_state,7,1,"out",100,0,HOP_XFER_FILE_SENDING);
    hop_xfer_tracking_file(&receive_state,7,1,"in",1048576,0,HOP_XFER_FILE_RECEIVING);
    hop_xfer_tracking_chunk(&receive_state,7,1,0,1048576,1000);
    assert(receive_state.status.bytes_total==1048576 && receive_state.status.bytes_done==1048576);
    assert(send_state.status.bytes_done==0);
    assert(hop_xfer_tracking_end(&send_state,7,HOP_XFER_COMPLETED,"done"));
    assert(receive_state.status.active);
    assert(hop_xfer_tracking_visible(&send_state,&receive_state)==&receive_state);
    assert(!hop_xfer_tracking_end(&send_state,7,HOP_XFER_COMPLETED,"duplicate"));
    assert(!hop_xfer_tracking_end(&receive_state,8,HOP_XFER_FAILED,"stale"));
    hop_xfer_tracking_phase(&receive_state,7,0,HOP_XFER_FILE_COMPLETED,1);
    assert(receive_state.status.files_done==1);
    /* Lost ACK -> same FILE_BEGIN again; count and size must not double. */
    hop_xfer_tracking_file(&receive_state,7,1,"in",1048576,0,HOP_XFER_FILE_RECEIVING);
    assert(receive_state.status.bytes_total==1048576 && receive_state.status.bytes_done==0);
    assert(receive_state.status.files_done==0);
    hop_xfer_tracking_chunk(&receive_state,7,1,0,128,2000);
    hop_xfer_tracking_chunk(&receive_state,7,1,0,128,2000);
    assert(receive_state.status.bytes_done==128);
    hop_xfer_tracking_chunk(&receive_state,8,1,128,128,3000);
    assert(receive_state.status.bytes_done==128);
    assert(hop_xfer_tracking_end(&receive_state,7,HOP_XFER_CANCELLED,"cancel"));
    for(int phase=HOP_XFER_COMPLETED;phase<=HOP_XFER_CANCELLED;phase++){
        hop_xfer_tracking_begin(&send_state,9,HOP_XFER_DIR_SEND,1,0,"out",HOP_XFER_SENDING);
        assert(hop_xfer_tracking_end(&send_state,9,phase,"terminal"));assert(!send_state.status.active);
    }
    hop_config c;hop_config_defaults(&c);
    strcpy(c.peer_host,"unchanged");
    put("empty.conf","");assert(!hop_config_load(&c,"empty.conf"));
    put("unknown.conf","unknown=value\n");assert(!hop_config_load(&c,"unknown.conf"));
    assert(strcmp(c.peer_host,"unchanged")==0);
    put("legacy.conf","host=10.20.30.40\ntoken=\n");assert(hop_config_load(&c,"legacy.conf"));
    assert(strcmp(c.peer_host,"10.20.30.40")==0 && c.token[0]==0);
#ifndef _WIN32
    put("canonical.conf","host=old\n");fault_write=1;
    assert(!hop_config_copy_atomic("legacy.conf","canonical.conf"));check_file("canonical.conf","host=old\n");fault_write=0;
    fault_read=1;assert(!hop_config_copy_atomic("legacy.conf","canonical.conf"));fault_read=read_error=0;check_file("canonical.conf","host=old\n");
    fault_close=1;assert(!hop_config_copy_atomic("legacy.conf","canonical.conf"));fault_close=0;check_file("canonical.conf","host=old\n");
    fault_rename=1;assert(!hop_config_copy_atomic("legacy.conf","canonical.conf"));fault_rename=0;check_file("canonical.conf","host=old\n");
    assert(hop_config_copy_atomic("legacy.conf","canonical.conf"));check_file("canonical.conf","host=10.20.30.40\ntoken=\n");
    check_file("legacy.conf","host=10.20.30.40\ntoken=\n");
#endif
    puts("PASS: direction/batch isolation, idempotent progress and terminal states, config load/migration");
    return 0;
}
