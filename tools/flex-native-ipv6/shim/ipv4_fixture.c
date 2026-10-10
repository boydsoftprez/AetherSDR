/* Copyright (c) 2026 J.J. Boyd. Original clean-room test code, authored with Codex.
 * SPDX-License-Identifier: MIT */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

static int timeout_ms = 15000;
static void fail(const char *what) { perror(what); exit(1); }
static void ready(int fd) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, timeout_ms) != 1 || !(p.revents & POLLIN)) { errno = ETIMEDOUT; fail("poll"); }
}
static int bound(int type, int port) {
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    if (fd < 0) { fail("socket"); }
    int yes = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes))) { fail("setsockopt"); }
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(fd, (struct sockaddr *)&a, sizeof(a))) { fail("bind"); }
    int domain = 0; socklen_t dl = sizeof(domain);
    if (getsockopt(fd, SOL_SOCKET, SO_DOMAIN, &domain, &dl) || domain != AF_INET) { fail("SO_DOMAIN"); }
    if (!(fcntl(fd, F_GETFD) & FD_CLOEXEC)) { fail("CLOEXEC lost"); }
    return fd;
}
static void peer_text(const struct sockaddr_in *a, char *out) {
    if (a->sin_family != AF_INET) { errno = EAFNOSUPPORT; fail("application peer family"); }
    inet_ntop(AF_INET, &a->sin_addr, out, INET_ADDRSTRLEN);
}
static void check_peer(int fd, const struct sockaddr_in *peer) {
    struct sockaddr_in actual = {0}; socklen_t len = sizeof(actual);
    if (getpeername(fd, (struct sockaddr *)&actual, &len) || len != sizeof(actual)
        || actual.sin_family != AF_INET || actual.sin_addr.s_addr != peer->sin_addr.s_addr || actual.sin_port != peer->sin_port) {
        errno = EPROTO; fail("getpeername identity");
    }
    unsigned char tiny[8]; memset(tiny, 0xa5, sizeof(tiny)); len = 2;
    if (getpeername(fd, (struct sockaddr *)tiny, &len) || len != sizeof(actual)
        || memcmp(tiny, &actual, 2) || tiny[2] != 0xa5) { errno = EPROTO; fail("getpeername truncation"); }
}
static void serve(int client, int udp, struct sockaddr_in peer) {
    char ip[INET_ADDRSTRLEN]; peer_text(&peer, ip); check_peer(client, &peer);
    char line[512];
    int n = snprintf(line, sizeof(line), "HELLO pid=%d family=%d peer=%s port=%u\n", getpid(), peer.sin_family, ip, ntohs(peer.sin_port));
    if (send(client, line, n, 0) != n) { fail("send hello"); }
    ready(client); ssize_t count = recv(client, line, sizeof(line) - 1, 0);
    if (count <= 0) { fail("recv command"); } line[count] = 0;
    unsigned port = 0; char mode[32] = {0};
    if (sscanf(line, "UDPPORT %u %31s", &port, mode) < 1 || port < 1 || port > 65535) { errno = EINVAL; fail("UDPPORT command"); }
    peer.sin_port = htons(port);
    n = snprintf(line, sizeof(line), "UDP_HELLO pid=%d token=%s\n", getpid(), ip);
    if (!strcmp(mode, "MSG")) {
        struct iovec iov = {.iov_base = line, .iov_len = (size_t)n};
        struct msghdr msg = {.msg_name = &peer, .msg_namelen = sizeof(peer), .msg_iov = &iov, .msg_iovlen = 1};
        if (sendmsg(udp, &msg, 0) != n) { fail("sendmsg"); }
    } else if (sendto(udp, line, n, 0, (struct sockaddr *)&peer, sizeof(peer)) != n) { fail("sendto"); }
    ready(udp); struct sockaddr_in source = {0}; socklen_t sl = sizeof(source);
    if (!strcmp(mode, "MSG")) {
        struct iovec iov = {.iov_base = line, .iov_len = sizeof(line)};
        struct msghdr msg = {.msg_name = &source, .msg_namelen = sizeof(source), .msg_iov = &iov, .msg_iovlen = 1};
        count = recvmsg(udp, &msg, 0); sl = msg.msg_namelen;
    } else { count = recvfrom(udp, line, sizeof(line), 0, (struct sockaddr *)&source, &sl); }
    if (count != 3 || memcmp(line, "ACK", 3) || sl != sizeof(source)) { errno = EPROTO; fail("UDP ack"); }
    char source_ip[INET_ADDRSTRLEN]; peer_text(&source, source_ip);
    if (source.sin_addr.s_addr != peer.sin_addr.s_addr || source.sin_port != peer.sin_port) { errno = EPROTO; fail("UDP receive peer consistency"); }
    n = snprintf(line, sizeof(line), "UDP_ACK family=%d peer=%s port=%u\n", source.sin_family, source_ip, ntohs(source.sin_port));
    if (send(client, line, n, 0) != n) { fail("send ack"); }
}
int main(int argc, char **argv) {
    int iterations = 6; int fork_mode = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--iterations") && i + 1 < argc) { iterations = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--timeout-ms") && i + 1 < argc) { timeout_ms = atoi(argv[++i]); }
        else if (!strcmp(argv[i], "--fork")) { fork_mode = 1; }
        else { fprintf(stderr, "usage: %s [--iterations N] [--timeout-ms N] [--fork]\n", argv[0]); return 2; }
    }
    setbuf(stdout, NULL);
    int tcp = bound(SOCK_STREAM, 46092); int udp = bound(SOCK_DGRAM, 46093);
    if (listen(tcp, 8)) { fail("listen"); }
    printf("READY pid=%d tcp=46092 udp=46093\n", getpid());
    for (int i = 0; i < iterations; ++i) {
        ready(tcp); struct sockaddr_in peer = {0}; socklen_t len = sizeof(peer);
        int client = accept(tcp, (struct sockaddr *)&peer, &len);
        if (client < 0 || len != sizeof(peer)) { fail("accept"); }
        if (fork_mode) {
            pid_t child = fork(); if (child < 0) { fail("fork"); }
            if (child == 0) { serve(client, udp, peer); close(client); _exit(0); }
            close(client); int status = 0;
            if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) { errno = ECHILD; fail("child"); }
        } else { serve(client, udp, peer); close(client); }
        /* Reuse a closed descriptor for an ordinary AF_INET socket; stale shim state must not leak. */
        int reused = socket(AF_INET, SOCK_DGRAM, 0); if (reused < 0) { fail("reuse socket"); }
        struct sockaddr_in ordinary = {.sin_family = AF_INET, .sin_port = 0, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        if (bind(reused, (struct sockaddr *)&ordinary, sizeof(ordinary))) { fail("ordinary bind"); }
        socklen_t olen = sizeof(ordinary);
        if (getsockname(reused, (struct sockaddr *)&ordinary, &olen) || ordinary.sin_family != AF_INET || ordinary.sin_addr.s_addr != htonl(INADDR_LOOPBACK)) { fail("fd reuse state"); }
        close(reused);
    }
    close(udp); close(tcp); printf("DONE pid=%d iterations=%d\n", getpid(), iterations); return 0;
}
