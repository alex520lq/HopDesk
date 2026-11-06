#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void local_addresses(char *out,size_t cap){
    if(!cap)return;
    out[0]=0;ULONG size=16000;IP_ADAPTER_ADDRESSES *a=malloc(size);
    if(!a)return;
    ULONG rc=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,NULL,a,&size);
    if(rc==ERROR_BUFFER_OVERFLOW){free(a);a=malloc(size);
    if(!a)return;
        rc=GetAdaptersAddresses(AF_INET,GAA_FLAG_SKIP_ANYCAST|GAA_FLAG_SKIP_MULTICAST|GAA_FLAG_SKIP_DNS_SERVER,NULL,a,&size);}
    if(rc==NO_ERROR)for(IP_ADAPTER_ADDRESSES *p=a;p;p=p->Next){
        if(p->OperStatus!=IfOperStatusUp||p->IfType==IF_TYPE_SOFTWARE_LOOPBACK)continue;
        for(IP_ADAPTER_UNICAST_ADDRESS *u=p->FirstUnicastAddress;u;u=u->Next){
            if(u->Address.lpSockaddr->sa_family!=AF_INET)continue;
            char ip[INET_ADDRSTRLEN];
            struct sockaddr_in *v=(struct sockaddr_in*)u->Address.lpSockaddr;
            if(!inet_ntop(AF_INET,&v->sin_addr,ip,sizeof(ip)))continue;
            size_t used=strlen(out);
    if(used+strlen(ip)+3<cap)snprintf(out+used,cap-used,"%s%s",used?" / ":"",ip);
        }
    }free(a);
    if(!out[0])snprintf(out,cap,"No active LAN IPv4 address");
}
