/*
 * Protocol-level sender/receiver simulation using the same production frame
 * decoder and transfer validators used by the Windows/macOS receive paths.
 * This does not replace OS file-writer/clipboard integration tests.
 */
#include "hop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t *bytes;
    uint64_t size;
    uint32_t batch, id, file_crc;
    uint64_t received;
    int active, bad_path, protocol_error, ack, done;
} rx_sim;

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x); ++failures; } } while(0)

static void rx_message(void *user, uint8_t type, const uint8_t *p, uint32_t len) {
    rx_sim *r=(rx_sim*)user;
    if(type==HOP_MSG_FILE_BEGIN){
        hop_xfer_file_begin_msg m;
        if(!hop_xfer_decode_file_begin(p,len,&m)){r->protocol_error=1;return;}
        r->batch=m.batch;r->id=m.id;r->size=m.size;r->active=1;r->file_crc=0;r->received=0;
        r->bad_path=!hop_xfer_relpath_bytes_safe(m.relpath,m.relpath_len);
        if(!r->bad_path && r->size){
            if(r->size>4u*1024u*1024u){r->protocol_error=1;return;}
            r->bytes=(uint8_t*)calloc(1,(size_t)r->size);
            if(!r->bytes){r->protocol_error=1;return;}
        }
        return;
    }
    if(type==HOP_MSG_FILE_CHUNK){
        if(!r->active){r->protocol_error=1;return;}
        if(!hop_xfer_chunk_valid(p,len,r->bad_path?UINT64_MAX:r->size,0)){
            r->protocol_error=1;return;
        }
        uint64_t off=hop_get_be64(p+4);
        uint32_t n=hop_get_be32(p+12),crc=hop_get_be32(p+16);
        if(hop_crc32(0,p+20,n)!=crc){r->ack=HOP_FILE_ACK_CRC;return;}
        if(r->bad_path)return;
        memcpy(r->bytes+off,p+20,n);
        r->file_crc=hop_xfer_file_crc_step(r->file_crc,off,crc);
        r->received+=n;
        return;
    }
    if(type==HOP_MSG_FILE_END){
        hop_xfer_file_end_msg m;
        if(!hop_xfer_decode_file_end(p,len,&m)){r->protocol_error=1;return;}
        if(!r->active || m.id!=r->id){r->protocol_error=1;return;}
        if(r->bad_path)r->ack=HOP_FILE_ACK_BADPATH;
        else if(r->ack==HOP_FILE_ACK_CRC || m.status!=HOP_FILE_OK || m.file_crc!=r->file_crc)
            r->ack=HOP_FILE_ACK_CRC;
        else r->ack=HOP_FILE_ACK_OK;
        r->done=1;
        return;
    }
}

static void feed_frame(hop_decoder*d,uint8_t type,const uint8_t*p,uint32_t n){
    uint8_t *frame=(uint8_t*)malloc(HOP_HEADER_SIZE+n);
    if(!frame){++failures;return;}
    uint32_t total=hop_encode_frame(type,p,n,frame);
    static const uint32_t parts[]={1,2,7,3,19,5};
    uint32_t off=0,k=0;
    while(off<total){
        uint32_t take=parts[k++%6];if(take>total-off)take=total-off;
        hop_decoder_feed(d,frame+off,take);off+=take;
    }
    free(frame);
}

static uint32_t make_file_begin(uint8_t*p,uint32_t batch,uint32_t id,
                                const char*name,const char*path,uint64_t size,uint8_t flags){
    size_t nl=strlen(name),pl=strlen(path);uint32_t n=(uint32_t)(29+nl+pl);
    memset(p,0,n);hop_put_be32(p,batch);hop_put_be32(p+4,id);
    hop_put_be64(p+8,size);hop_put_be64(p+16,123456);p[24]=flags;
    hop_put_be16(p+25,(uint16_t)nl);memcpy(p+27,name,nl);
    hop_put_be16(p+27+nl,(uint16_t)pl);memcpy(p+29+nl,path,pl);return n;
}

static void run_file_case(int mode,uint64_t size,uint32_t chunk_size,const char*path,int expected_ack){
    rx_sim r;memset(&r,0,sizeof(r));r.ack=HOP_FILE_ACK_OK;
    hop_decoder*d=hop_decoder_new(&r,rx_message);CHECK(d!=NULL);if(!d)return;
    uint8_t begin[1200];uint32_t bn=make_file_begin(begin,44,9,"payload.bin",path,size,0);
    feed_frame(d,HOP_MSG_FILE_BEGIN,begin,bn);
    if(size && !r.bad_path && !r.protocol_error){
        uint8_t *source=(uint8_t*)malloc((size_t)size);
        if(!source){++failures;hop_decoder_free(d);free(r.bytes);return;}
        for(uint64_t i=0;i<size;i++)source[i]=(uint8_t)((i*31u+17u)&0xFFu);
        uint32_t chunks=(uint32_t)((size+chunk_size-1)/chunk_size);
        uint32_t whole=0;
        for(uint64_t pos=0;pos<size;pos+=chunk_size){
            uint32_t count=(uint32_t)((size-pos)<chunk_size?(size-pos):chunk_size);
            uint32_t crc=hop_crc32(0,source+pos,count);
            whole=hop_xfer_file_crc_step(whole,pos,crc);
        }
        for(uint32_t step=0;step<chunks;step++){
            uint32_t index=(mode==3)?(chunks-1-step):step;
            uint64_t off=(uint64_t)index*chunk_size;
            uint32_t n=(uint32_t)((size-off)<chunk_size?(size-off):chunk_size);
            uint8_t *cp=(uint8_t*)malloc(20u+n);
            if(!cp){++failures;break;}
            hop_put_be32(cp,9);
            uint64_t wire_off=off;
            if(mode==2 && index==0)wire_off=off+1;
            hop_put_be64(cp+4,wire_off);hop_put_be32(cp+12,n);
            uint32_t crc=hop_crc32(0,source+off,n);
            hop_put_be32(cp+16,mode==1&&index==0?(crc^1u):crc);
            memcpy(cp+20,source+off,n);
            feed_frame(d,HOP_MSG_FILE_CHUNK,cp,20u+n);
            free(cp);
        }
        uint8_t end[9];hop_put_be32(end,9);end[4]=HOP_FILE_OK;hop_put_be32(end+5,whole);
        feed_frame(d,HOP_MSG_FILE_END,end,sizeof(end));
        if(mode==0){
            CHECK(r.bytes!=NULL);CHECK(r.received==size);
            CHECK(memcmp(r.bytes,source,(size_t)size)==0);
        }
        free(source);
    }else{
        uint8_t end[9];hop_put_be32(end,9);end[4]=HOP_FILE_OK;hop_put_be32(end+5,0);
        feed_frame(d,HOP_MSG_FILE_END,end,sizeof(end));
    }
    CHECK(r.done==1);
    CHECK(r.protocol_error==0);
    CHECK(r.ack==expected_ack);
    hop_decoder_free(d);free(r.bytes);
}

static void test_file_transfer_wire_roundtrip(void){
    run_file_case(0,0,4096,"empty.bin",HOP_FILE_ACK_OK);
    run_file_case(0,1,4096,"one.bin",HOP_FILE_ACK_OK);
    run_file_case(0,4095,4096,"below.bin",HOP_FILE_ACK_OK);
    run_file_case(0,4096,4096,"exact.bin",HOP_FILE_ACK_OK);
    run_file_case(0,4097,4096,"above.bin",HOP_FILE_ACK_OK);
    run_file_case(0,131071,8192,"multi.bin",HOP_FILE_ACK_OK);
    run_file_case(1,8192,4096,"crc.bin",HOP_FILE_ACK_CRC);
    run_file_case(2,8192,4096,"offset.bin",HOP_FILE_ACK_CRC);
    run_file_case(3,8192,4096,"order.bin",HOP_FILE_ACK_CRC);
    run_file_case(4,1024,512,"../escape.bin",HOP_FILE_ACK_BADPATH);
}

static void test_directory_entry(void){
    rx_sim r;memset(&r,0,sizeof(r));r.ack=HOP_FILE_ACK_OK;
    hop_decoder*d=hop_decoder_new(&r,rx_message);CHECK(d!=NULL);if(!d)return;
    uint8_t p[128];uint32_t n=make_file_begin(p,3,4,"empty-dir","folder/empty",0,HOP_FILE_FLAG_DIR);
    feed_frame(d,HOP_MSG_FILE_BEGIN,p,n);
    uint8_t end[9];hop_put_be32(end,4);end[4]=HOP_FILE_OK;hop_put_be32(end+5,0);
    feed_frame(d,HOP_MSG_FILE_END,end,sizeof(end));
    CHECK(r.done==1);CHECK(r.ack==HOP_FILE_ACK_OK);CHECK(r.size==0);
    hop_decoder_free(d);
}


typedef struct {
    uint8_t *pixels;
    size_t expected, received;
    int started, complete, error;
} img_sim;

static void image_message(void *user,uint8_t type,const uint8_t*p,uint32_t len){
    img_sim *r=(img_sim*)user;
    if(type==HOP_MSG_IMAGE_BEGIN){
        hop_xfer_image_begin_msg m;
        if(!hop_xfer_decode_image_begin(p,len,&m)){r->error=1;return;}
        if(!hop_xfer_image_valid((int32_t)m.width,(int32_t)m.height,(int32_t)m.stride)){r->error=1;return;}
        r->expected=(size_t)m.stride*(size_t)m.height;
        r->pixels=(uint8_t*)calloc(1,r->expected);
        if(!r->pixels){r->error=1;return;}
        r->started=1;return;
    }
    if(type==HOP_MSG_FILE_CHUNK){
        if(!r->started||!hop_xfer_chunk_valid(p,len,r->expected,1)){r->error=1;r->received=0;return;}
        uint64_t off=hop_get_be64(p+4);uint32_t n=hop_get_be32(p+12),crc=hop_get_be32(p+16);
        if(off!=r->received||hop_crc32(0,p+20,n)!=crc){r->error=1;r->received=0;return;}
        memcpy(r->pixels+off,p+20,n);r->received+=n;return;
    }
    if(type==HOP_MSG_FILE_END){
        hop_xfer_file_end_msg e;
        if(!hop_xfer_decode_file_end(p,len,&e)||e.status!=HOP_FILE_OK){r->error=1;return;}
        r->complete=(r->received==r->expected);return;
    }
}

static void run_image_case(int mode,int expect_complete){
    img_sim r;memset(&r,0,sizeof(r));
    hop_decoder*d=hop_decoder_new(&r,image_message);CHECK(d!=NULL);if(!d)return;
    uint8_t begin[20];hop_put_be32(begin,5);hop_put_be32(begin+4,1);
    hop_put_be32(begin+8,2);hop_put_be32(begin+12,2);hop_put_be32(begin+16,8);
    feed_frame(d,HOP_MSG_IMAGE_BEGIN,begin,sizeof(begin));
    uint8_t source[16];for(int i=0;i<16;i++)source[i]=(uint8_t)(i*7+1);
    uint8_t chunk[28];
    int count=(mode==1)?1:2;
    for(int step=0;step<count;step++){
        int index=(mode==2)?1-step:step;
        uint64_t off=(uint64_t)index*8;
        uint32_t n=8,crc=hop_crc32(0,source+off,n);
        hop_put_be32(chunk,1);hop_put_be64(chunk+4,off);
        hop_put_be32(chunk+12,n);hop_put_be32(chunk+16,mode==3&&index==0?(crc^1u):crc);
        memcpy(chunk+20,source+off,n);
        feed_frame(d,HOP_MSG_FILE_CHUNK,chunk,sizeof(chunk));
    }
    if(mode==4){
        hop_put_be32(chunk,1);hop_put_be64(chunk+4,16);
        hop_put_be32(chunk+12,1);hop_put_be32(chunk+16,hop_crc32(0,source,1));
        chunk[20]=source[0];
        feed_frame(d,HOP_MSG_FILE_CHUNK,chunk,21);
    }
    uint8_t end[9];hop_put_be32(end,1);end[4]=HOP_FILE_OK;hop_put_be32(end+5,0);
    feed_frame(d,HOP_MSG_FILE_END,end,sizeof(end));
    CHECK(r.complete==expect_complete);
    if(mode==0 && r.complete)CHECK(memcmp(r.pixels,source,sizeof(source))==0);
    if(mode!=0)CHECK(r.error || !r.complete);
    free(r.pixels);hop_decoder_free(d);
}

static void test_image_transfer_protocol(void){
    run_image_case(0,1); /* complete image */
    run_image_case(1,0); /* truncated image */
    run_image_case(2,0); /* out-of-order chunks */
    run_image_case(3,0); /* corrupted chunk */
    run_image_case(4,0); /* beyond declared image size */
}

int main(void){
    hop_crc32_init();
    test_file_transfer_wire_roundtrip();
    test_directory_entry();
    test_image_transfer_protocol();
    if(failures){fprintf(stderr,"HopDesk xfer protocol tests: %d failure(s)\n",failures);return 1;}
    puts("HopDesk xfer protocol tests: all tests passed");return 0;
}
