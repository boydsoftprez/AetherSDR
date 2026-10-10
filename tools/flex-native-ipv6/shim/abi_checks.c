/* Copyright (c) 2026 J.J. Boyd. Original clean-room tests, authored with Codex.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL line %d: %s errno=%d\n", __LINE__, #x, errno); exit(1); } } while (0)
static struct sockaddr_in selected(int port) {
    return (struct sockaddr_in){.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
}
static void reject_lifecycle(void) {
    struct sockaddr_in a = selected(46093);
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    int copy = fcntl(fd, F_DUPFD_CLOEXEC, 20); CHECK(copy >= 0);
    errno = 0; CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EOPNOTSUPP); close(copy); close(fd);
    fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0); int value = 32768;
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)) == 0);
    errno = 0; CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EOPNOTSUPP); close(fd);
    fd = socket(AF_INET, SOCK_DGRAM, 0); int ep = epoll_create1(EPOLL_CLOEXEC); CHECK(fd >= 0 && ep >= 0);
    struct epoll_event ev = {.events = EPOLLIN, .data.fd = fd}; CHECK(epoll_ctl(ep, EPOLL_CTL_ADD, fd, &ev) == 0);
    errno = 0; CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EOPNOTSUPP); close(ep); close(fd);
    fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    copy = fcntl64(fd, F_DUPFD, 20); CHECK(copy >= 0);
    errno = 0; CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EOPNOTSUPP); close(copy); close(fd);
    fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0);
    CHECK(fcntl(fd, F_SETFL, O_NONBLOCK | O_ASYNC) == 0);
    errno = 0; CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == -1 && errno == EOPNOTSUPP); close(fd);
    /* fork-before-bind means the backing socket is already shared with another PID. */
    fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0); pid_t child = fork(); CHECK(child >= 0);
    if (child == 0) { errno = 0; int r = bind(fd, (struct sockaddr *)&a, sizeof(a)); _exit(r == -1 && errno == EOPNOTSUPP ? 0 : 1); }
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0); close(fd);
    puts("PASS unsupported duplicate, option, epoll, fork-before-bind rejected");
}
static void address_edges(void) {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); CHECK(fd >= 0);
    int one = 1; CHECK(setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0);
    struct sockaddr_in a = selected(46093); CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0);
    CHECK((fcntl(fd, F_GETFL) & O_NONBLOCK) && (fcntl(fd, F_GETFD) & FD_CLOEXEC));
    socklen_t len = sizeof(one); one = 0; CHECK(getsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, &len) == 0 && one == 1);
    struct sockaddr_storage kernel; len = sizeof(kernel);
    CHECK(syscall(SYS_getsockname, fd, &kernel, &len) == 0 && kernel.ss_family == AF_INET6);
    len = sizeof(a); CHECK(getsockname(fd, (struct sockaddr *)&a, &len) == 0 && len == sizeof(a) && a.sin_family == AF_INET && a.sin_addr.s_addr == htonl(INADDR_ANY));
    unsigned char tiny[8]; memset(tiny, 0xa5, sizeof(tiny)); len = 0;
    CHECK(getsockname(fd, (struct sockaddr *)tiny, &len) == 0 && len == sizeof(a) && tiny[0] == 0xa5);
    len = sizeof(a); errno = 0; CHECK(getsockname(fd, NULL, &len) == -1 && errno == EFAULT);
    int client = socket(AF_INET6, SOCK_DGRAM, 0); CHECK(client >= 0);
    struct sockaddr_in6 target = {.sin6_family = AF_INET6, .sin6_port = htons(46093), .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    CHECK(sendto(client, "abc", 3, 0, (struct sockaddr *)&target, sizeof(target)) == 3);
    char data[4]; memset(tiny, 0xa5, sizeof(tiny)); len = 2;
    CHECK(recvfrom(fd, data, sizeof(data), 0, (struct sockaddr *)tiny, &len) == 3 && len == sizeof(a) && tiny[0] == AF_INET && tiny[2] == 0xa5);
    CHECK(sendto(client, "abcdef", 6, 0, (struct sockaddr *)&target, sizeof(target)) == 6);
    struct iovec iov = {.iov_base = data, .iov_len = 2}; memset(tiny, 0xa5, sizeof(tiny));
    struct msghdr message = {.msg_name = tiny, .msg_namelen = 2, .msg_iov = &iov, .msg_iovlen = 1};
    CHECK(recvmsg(fd, &message, 0) == 2 && message.msg_namelen == sizeof(a) && (message.msg_flags & MSG_TRUNC) && tiny[2] == 0xa5);
    errno = 0; len = sizeof(a); CHECK(recvfrom(fd, data, sizeof(data), 0, (struct sockaddr *)&a, &len) == -1 && errno == EAGAIN);
    close(client); close(fd); puts("PASS real AF_INET6 FD, flags, reuseaddr, getsockname/recvfrom/recvmsg truncation and errno");
}
static void *worker(void *pointer) {
    int fd = *(int *)pointer; uint16_t payload = 0; struct sockaddr_in source; socklen_t len = sizeof(source);
    CHECK(recvfrom(fd, &payload, sizeof(payload), 0, (struct sockaddr *)&source, &len) == sizeof(payload));
    CHECK(len == sizeof(source) && source.sin_family == AF_INET && source.sin_port == payload);
    CHECK((ntohl(source.sin_addr.s_addr) & 0xfffe0000U) == 0xc6120000U);
    CHECK(sendto(fd, &payload, sizeof(payload), 0, (struct sockaddr *)&source, len) == sizeof(payload));
    return NULL;
}
static void *client_worker(void *unused) {
    (void)unused; int fd = socket(AF_INET6, SOCK_DGRAM, 0); CHECK(fd >= 0);
    struct timeval timeout = {.tv_sec = 3}; CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    struct sockaddr_in6 local = {.sin6_family = AF_INET6, .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    CHECK(bind(fd, (struct sockaddr *)&local, sizeof(local)) == 0); socklen_t len = sizeof(local);
    CHECK(getsockname(fd, (struct sockaddr *)&local, &len) == 0); uint16_t payload = local.sin6_port;
    struct sockaddr_in6 target = {.sin6_family = AF_INET6, .sin6_port = htons(46093), .sin6_addr = IN6ADDR_LOOPBACK_INIT};
    CHECK(sendto(fd, &payload, sizeof(payload), 0, (struct sockaddr *)&target, sizeof(target)) == sizeof(payload));
    uint16_t reply = 0; CHECK(recvfrom(fd, &reply, sizeof(reply), 0, NULL, NULL) == sizeof(reply) && reply == payload);
    close(fd); return NULL;
}
static void threaded_traffic(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0); CHECK(fd >= 0); struct sockaddr_in a = selected(46093);
    CHECK(bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0); struct timeval timeout = {.tv_sec = 3};
    CHECK(setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    pthread_t workers[8], clients[8];
    for (int i = 0; i < 8; ++i) { CHECK(pthread_create(&workers[i], NULL, worker, &fd) == 0); }
    for (int i = 0; i < 8; ++i) { CHECK(pthread_create(&clients[i], NULL, client_worker, NULL) == 0); }
    for (int i = 0; i < 8; ++i) { CHECK(pthread_join(clients[i], NULL) == 0); CHECK(pthread_join(workers[i], NULL) == 0); }
    close(fd); puts("PASS 8 concurrent IPv6 UDP endpoints: address aliases and independent ports");
}
int main(void) { reject_lifecycle(); address_edges(); threaded_traffic(); puts("PASS ABI checks"); return 0; }
