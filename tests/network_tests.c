#include "hop.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <winsock2.h>
typedef SOCKET test_socket;
#define BAD_SOCKET INVALID_SOCKET
#define sock_close closesocket
#define sock_send(s,p,n) send((s),(const char *)(p),(int)(n),0)
#define sock_recv(s,p,n) recv((s),(char *)(p),(int)(n),0)
static int net_init(void){WSADATA w;return WSAStartup(MAKEWORD(2,2),&w)==0;}
static void net_cleanup(void){WSACleanup();}
#else
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
typedef int test_socket;
#define BAD_SOCKET (-1)
#define sock_close close
#define sock_send(s,p,n) send((s),(p),(n),0)
#define sock_recv(s,p,n) recv((s),(p),(n),0)
static int net_init(void){return 1;}
static void net_cleanup(void){}
#endif
static int failures, received;
static uint8_t types[8], data[8][32];
static uint32_t lengths[8];
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);++failures;}}while(0)
static void on_message(void*u,uint8_t t,const uint8_t*p,uint32_t n){
 (void)u;if(received>=8||n>32){++failures;return;}
 types[received]=t;lengths[received]=n;if(n)memcpy(data[received],p,n);++received;
}
static int pair_open(test_socket*c,test_socket*s){
 test_socket l=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);if(l==BAD_SOCKET)return 0;
 struct sockaddr_in a;memset(&a,0,sizeof(a));a.sin_family=AF_INET;a.sin_addr.s_addr=htonl(INADDR_LOOPBACK);a.sin_port=0;
 if(bind(l,(struct sockaddr*)&a,sizeof(a))!=0){sock_close(l);return 0;}
#ifdef _WIN32
 int alen=sizeof(a);
#else
 socklen_t alen=sizeof(a);
#endif
 if(getsockname(l,(struct sockaddr*)&a,&alen)!=0||listen(l,1)!=0){sock_close(l);return 0;}
 *c=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
 if(*c==BAD_SOCKET){sock_close(l);return 0;}
 if(connect(*c,(struct sockaddr*)&a,sizeof(a))!=0){sock_close(*c);sock_close(l);return 0;}
 *s=accept(l,NULL,NULL);sock_close(l);
 if(*s==BAD_SOCKET){sock_close(*c);*c=BAD_SOCKET;return 0;}return 1;
}
static int send_pieces(test_socket s,const uint8_t*p,size_t n){
 static const size_t parts[]={1,2,1,3,2,5};size_t off=0,k=0;
 while(off<n){size_t take=parts[k++%6];if(take>n-off)take=n-off;
  int r=(int)sock_send(s,p+off,take);if(r<=0)return 0;off+=(size_t)r;}return 1;
}
static void test_fragmented_and_coalesced(void){
 test_socket c=BAD_SOCKET,s=BAD_SOCKET;CHECK(pair_open(&c,&s));if(c==BAD_SOCKET||s==BAD_SOCKET)return;
 uint8_t a[32],b[32],both[64],buf[3];const uint8_t pa[]={'H','o','p','D','e','s','k'},pb[]={0xA5,0x5A};
 uint32_t na=hop_encode_frame(61,pa,sizeof(pa),a),nb=hop_encode_frame(62,pb,sizeof(pb),b);
 memcpy(both,a,na);memcpy(both+na,b,nb);CHECK(send_pieces(c,both,na+nb));
 hop_decoder*d=hop_decoder_new(NULL,on_message);CHECK(d!=NULL);
 if(d){while(received<2){int n=(int)sock_recv(s,buf,sizeof(buf));if(n<=0)break;hop_decoder_feed(d,buf,(uint32_t)n);}
  CHECK(received==2);CHECK(types[0]==61&&lengths[0]==sizeof(pa));CHECK(memcmp(data[0],pa,sizeof(pa))==0);
  CHECK(types[1]==62&&lengths[1]==sizeof(pb));CHECK(memcmp(data[1],pb,sizeof(pb))==0);hop_decoder_free(d);}
 sock_close(c);sock_close(s);
}
static void test_disconnect_mid_frame(void){
 test_socket c=BAD_SOCKET,s=BAD_SOCKET;CHECK(pair_open(&c,&s));if(c==BAD_SOCKET||s==BAD_SOCKET)return;
 uint8_t h[HOP_HEADER_SIZE];hop_encode_header(h,99,12);CHECK(sock_send(c,h,HOP_HEADER_SIZE)==HOP_HEADER_SIZE);
 const uint8_t partial[]={1,2,3};CHECK(sock_send(c,partial,sizeof(partial))==(int)sizeof(partial));sock_close(c);
 hop_decoder*d=hop_decoder_new(NULL,on_message);CHECK(d!=NULL);
 if(d){uint8_t buf[32];int n;while((n=(int)sock_recv(s,buf,sizeof(buf)))>0)hop_decoder_feed(d,buf,(uint32_t)n);
  CHECK(n==0);CHECK(received==0);hop_decoder_free(d);}sock_close(s);
}
int main(void){if(!net_init())return 2;test_fragmented_and_coalesced();received=0;test_disconnect_mid_frame();net_cleanup();
 if(failures){fprintf(stderr,"network tests: %d failure(s)\n",failures);return 1;}puts("network tests: all passed");return 0;}
