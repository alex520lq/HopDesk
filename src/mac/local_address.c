#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdio.h>
#include <string.h>
void local_addresses(char *out,size_t cap){
    if(!cap)return;
    out[0]=0;struct ifaddrs *list=NULL;
    if(!getifaddrs(&list)){
        for(struct ifaddrs *p=list;p;p=p->ifa_next){
            if(!p->ifa_addr||p->ifa_addr->sa_family!=AF_INET||!(p->ifa_flags&IFF_UP)||(p->ifa_flags&IFF_LOOPBACK))continue;
            char ip[INET_ADDRSTRLEN];struct sockaddr_in *v=(struct sockaddr_in*)p->ifa_addr;
            if(!inet_ntop(AF_INET,&v->sin_addr,ip,sizeof(ip)))continue;
            size_t used=strlen(out);
    if(used+strlen(ip)+3<cap)snprintf(out+used,cap-used,"%s%s",used?" / ":"",ip);
        }freeifaddrs(list);
    }if(!out[0])snprintf(out,cap,"No active LAN IPv4 address");
}
