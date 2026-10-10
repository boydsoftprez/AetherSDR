/* Copyright (c) 2026 J.J. Boyd. Original clean-room test fixture, authored with Codex.
 * SPDX-License-Identifier: MIT. Public Linux socket API only. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
enum { kClients=4,kRounds=100,kInternal=3 };
static void ready(int fd) {
    struct pollfd p={.fd=fd,.events=POLLIN}; CHECK(poll(&p,1,5000)==1 && (p.revents&POLLIN));
}
static int server(int type,int port,int opt,int requested) {
    int protocol=type==SOCK_STREAM?IPPROTO_TCP:IPPROTO_UDP;
    int fd=socket(AF_INET,type|SOCK_CLOEXEC,protocol); CHECK(fd>=0);
    CHECK(setsockopt(fd,opt==TCP_NODELAY?IPPROTO_TCP:SOL_SOCKET,opt,&requested,sizeof(requested))==0);
    if(type==SOCK_STREAM) { int one=1; CHECK(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))==0); }
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)}; CHECK(bind(fd,(struct sockaddr *)&a,sizeof(a))==0);
    struct sockaddr_storage real; socklen_t n=sizeof(real); CHECK(syscall(SYS_getsockname,fd,&real,&n)==0 && real.ss_family==AF_INET6);
    int domain=0; n=sizeof(domain); CHECK(getsockopt(fd,SOL_SOCKET,SO_DOMAIN,&domain,&n)==0 && domain==AF_INET);
    n=sizeof(a); CHECK(getsockname(fd,(struct sockaddr *)&a,&n)==0 && a.sin_family==AF_INET && ntohs(a.sin_port)==port);
    CHECK(fcntl(fd,F_GETFD)&FD_CLOEXEC); return fd;
}
int main(void) {
    setbuf(stdout,NULL);
    int tcp=server(SOCK_STREAM,4992,TCP_NODELAY,1);
    int out=server(SOCK_DGRAM,4993,SO_REUSEADDR,1),in=server(SOCK_DGRAM,4991,SO_RCVBUF,360192);
    int flags=fcntl(tcp,F_GETFL); CHECK(flags>=0 && fcntl(tcp,F_SETFL,flags|O_NONBLOCK)==0 && listen(tcp,20)==0);
    /* Same namespace loopback aliases exercise the exact internal destination.
     * This is socket transport proof, not an FPGA or separate-host simulation. */
    struct sockaddr_in internal={.sin_family=AF_INET,.sin_port=htons(4991)};
    CHECK(inet_pton(AF_INET,"172.30.1.2",&internal.sin_addr)==1);
    CHECK(sendto(out,"INTERNAL",8,0,(struct sockaddr *)&internal,sizeof(internal))==8); ready(in);
    struct sockaddr_in source; socklen_t n=sizeof(source); char data[256];
    CHECK(recvfrom(in,data,sizeof(data),0,(struct sockaddr *)&source,&n)==8);
    CHECK(!memcmp(data,"INTERNAL",8) && source.sin_family==AF_INET && source.sin_addr.s_addr==internal.sin_addr.s_addr && ntohs(source.sin_port)==4993);
    puts("PASS IPv4 internal destination 172.30.1.2:4991 from shared UDP4993");
    printf("READY pid=%d tcp=4992 udp=4991,4993\n",getpid());
    int clients[kClients]; struct sockaddr_in peers[kClients];
    for(int i=0;i<kClients;++i) {
        ready(tcp); n=sizeof(peers[i]); clients[i]=accept(tcp,(struct sockaddr *)&peers[i],&n); CHECK(clients[i]>=0 && n==sizeof(peers[i]));
        struct sockaddr_in actual; n=sizeof(actual); CHECK(getpeername(clients[i],(struct sockaddr *)&actual,&n)==0);
        CHECK(actual.sin_family==AF_INET && actual.sin_addr.s_addr==peers[i].sin_addr.s_addr && actual.sin_port==peers[i].sin_port);
        char ip[INET_ADDRSTRLEN]; CHECK(inet_ntop(AF_INET,&actual.sin_addr,ip,sizeof(ip))!=NULL);
        int count=snprintf(data,sizeof(data),"HELLO pid=%d peer=%s port=%u\n",getpid(),ip,ntohs(actual.sin_port));
        CHECK(send(clients[i],data,count,0)==count); ready(clients[i]);
        ssize_t got=recv(clients[i],data,sizeof(data)-1,0); CHECK(got>0); data[got]=0;
        unsigned port=0; CHECK(sscanf(data,"PORT %u",&port)==1 && port>0 && port<=65535); peers[i].sin_port=htons(port);
        count=snprintf(data,sizeof(data),"UDP peer=%s\n",ip);
        CHECK(sendto(out,data,count,0,(struct sockaddr *)&peers[i],sizeof(peers[i]))==count);
        struct iovec iov={.iov_base=data,.iov_len=(size_t)count};
        struct msghdr msg={.msg_name=&peers[i],.msg_namelen=sizeof(peers[i]),.msg_iov=&iov,.msg_iovlen=1};
        CHECK(sendmsg(in,&msg,0)==count);
    }
    int sockets[]={in,out},total=(kClients+kInternal)*2*kRounds,external=0,internal_count=0;
    int processed=0;
    while(processed<total) {
        fd_set readfds; FD_ZERO(&readfds); FD_SET(in,&readfds); FD_SET(out,&readfds);
        struct timeval timeout={.tv_sec=5}; int max=in>out?in:out;
        CHECK(select(max+1,&readfds,NULL,NULL,&timeout)>0);
        for(int j=0;j<2;++j) {
            if(!FD_ISSET(sockets[j],&readfds)) { continue; }
            n=sizeof(source); ssize_t got;
            if(j==0) { got=recvfrom(sockets[j],data,sizeof(data),0,(struct sockaddr *)&source,&n); }
            else {
                struct iovec iov={.iov_base=data,.iov_len=sizeof(data)};
                struct msghdr msg={.msg_name=&source,.msg_namelen=n,.msg_iov=&iov,.msg_iovlen=1};
                got=recvmsg(sockets[j],&msg,0); n=msg.msg_namelen;
            }
            CHECK(got>0 && n==sizeof(source) && source.sin_family==AF_INET);
            int matched=0;
            for(int i=0;i<kClients;++i) {
                if(source.sin_addr.s_addr==peers[i].sin_addr.s_addr && source.sin_port==peers[i].sin_port) { matched=1; }
            }
            if(matched) { ++external; }
            else {
                unsigned port=ntohs(source.sin_port);
                CHECK(source.sin_addr.s_addr==internal.sin_addr.s_addr && (port==50000 || port==50002 || port==50003)); ++internal_count;
            }
            if(j==0) {
                struct iovec iov={.iov_base=data,.iov_len=(size_t)got};
                struct msghdr msg={.msg_name=&source,.msg_namelen=n,.msg_iov=&iov,.msg_iovlen=1}; CHECK(sendmsg(sockets[j],&msg,0)==got);
            } else { CHECK(sendto(sockets[j],data,got,0,(struct sockaddr *)&source,n)==got); }
            /* One select may deliver both descriptors; count datagrams, not wakeups. */
            ++processed;
        }
    }
    CHECK(external==kClients*2*kRounds && internal_count==kInternal*2*kRounds);
    for(int i=0;i<kClients;++i) { CHECK(send(clients[i],"DONE\n",5,0)==5); close(clients[i]); }
    close(in); close(out); close(tcp);
    printf("PASS mixed concurrent traffic external=%d internal=%d TCP aliases on both UDP ports\n",external,internal_count); return 0;
}
