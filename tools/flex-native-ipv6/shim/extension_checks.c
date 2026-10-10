/* Copyright (c) 2026 J.J. Boyd. Original clean-room tests, authored with Codex.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s errno=%d\n",__LINE__,#x,errno); exit(1); } } while (0)
static int domain(int fd) {
    struct sockaddr_storage a; socklen_t n=sizeof(a);
    CHECK(syscall(SYS_getsockname,fd,&a,&n)==0); return a.ss_family;
}
static void bound(int fd,int port) {
    struct sockaddr_in a={.sin_family=AF_INET,.sin_port=htons(port)};
    CHECK(bind(fd,(struct sockaddr *)&a,sizeof(a))==0);
    socklen_t n=sizeof(a); CHECK(getsockname(fd,(struct sockaddr *)&a,&n)==0);
    CHECK(n==sizeof(a) && a.sin_family==AF_INET && ntohs(a.sin_port)==port && a.sin_addr.s_addr==0);
}
static int option(int fd,int level,int name) {
    int v=0; socklen_t n=sizeof(v); CHECK(getsockopt(fd,level,name,&v,&n)==0 && n==sizeof(v)); return v;
}
int main(int argc,char **argv) {
    CHECK(argc==2);
    if (!strcmp(argv[1],"nodelay")) {
        int fd=socket(AF_INET,SOCK_STREAM|SOCK_CLOEXEC,IPPROTO_TCP),v=1; CHECK(fd>=0);
        CHECK(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&v,sizeof(v))==0);
        CHECK(setsockopt(fd,IPPROTO_TCP,TCP_NODELAY,&v,sizeof(v))==0);
        bound(fd,4992); CHECK(domain(fd)==AF_INET6);
        CHECK(option(fd,IPPROTO_TCP,TCP_NODELAY)==1 && option(fd,SOL_SOCKET,SO_REUSEADDR)==1);
        int flags=fcntl(fd,F_GETFL); CHECK(flags>=0 && fcntl(fd,F_SETFL,flags|O_NONBLOCK)==0);
        CHECK((fcntl(fd,F_GETFL)&O_NONBLOCK) && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
        CHECK(listen(fd,20)==0); close(fd); puts("PASS TCP_NODELAY/reuse and post-bind nonblocking/listen");
    } else if (!strcmp(argv[1],"rcvbuf")) {
        /* 32768 stays below Linux's rmem_max clamp, exposing accidental redoubling. */
        const int requests[]={360192,32768};
        for (unsigned i=0;i<sizeof(requests)/sizeof(requests[0]);++i) {
            int fd=socket(AF_INET,SOCK_DGRAM|SOCK_NONBLOCK|SOCK_CLOEXEC,IPPROTO_UDP); CHECK(fd>=0);
            int old=65536; CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&old,sizeof(old))==0);
            CHECK(setsockopt(fd,SOL_SOCKET,SO_RCVBUF,&requests[i],sizeof(requests[i]))==0);
            int before=option(fd,SOL_SOCKET,SO_RCVBUF); bound(fd,4991); CHECK(domain(fd)==AF_INET6);
            CHECK(option(fd,SOL_SOCKET,SO_RCVBUF)==before);
            CHECK((fcntl(fd,F_GETFL)&O_NONBLOCK) && (fcntl(fd,F_GETFD)&FD_CLOEXEC));
            printf("PASS SO_RCVBUF request=%d kernel_before=%d kernel_after=%d\n",requests[i],before,option(fd,SOL_SOCKET,SO_RCVBUF)); close(fd);
        }
    } else if (!strcmp(argv[1],"ports") || !strcmp(argv[1],"invalid") || !strcmp(argv[1],"legacy")) {
        const int ports[]={4991,4993,4992,30000,30001,46093};
        for(unsigned i=0;i<sizeof(ports)/sizeof(ports[0]);++i) {
            int fd=socket(AF_INET,SOCK_DGRAM,0); CHECK(fd>=0);
            if(ports[i]==4992) {
                int one=1; CHECK(setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&one,sizeof(one))==0);
                CHECK(setsockopt(fd,SOL_SOCKET,SO_BROADCAST,&one,sizeof(one))==0);
                CHECK(setsockopt(fd,SOL_SOCKET,SO_BINDTODEVICE,"lo",3)==0);
            }
            bound(fd,ports[i]);
            int converted=!strcmp(argv[1],"ports")?(ports[i]==4991 || ports[i]==4993):(!strcmp(argv[1],"legacy") && ports[i]==46093);
            CHECK(domain(fd)==(converted?AF_INET6:AF_INET)); close(fd);
        }
        int tcp=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP); CHECK(tcp>=0); bound(tcp,4994); CHECK(domain(tcp)==AF_INET); close(tcp);
        puts("PASS explicit UDP selection, discovery broadcast/device options, untouched internal/SmartLink ports");
    } else { CHECK(0); }
    return 0;
}
