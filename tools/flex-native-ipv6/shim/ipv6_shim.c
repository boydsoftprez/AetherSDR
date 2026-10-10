/* Copyright (c) 2026 J.J. Boyd. Original clean-room prototype, authored with Codex.
 * SPDX-License-Identifier: MIT
 * Public Linux/glibc socket ABI only. No vendor implementation or private ABI.
 * Explicit opt-in experiment; see README.md for unsupported lifecycle and scope. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

enum { kMaxFds = 65536, kMaxPeers = 1024, kMaxUdpPorts = 32 };
static const uint32_t kTokenBase = 0xc6120000U; /* 198.18.0.0; never emitted on wire. */
struct fd_state { unsigned char candidate, converted, unsupported, shared; int type, protocol, reuse, nodelay, rcvbuf; unsigned char has_nodelay, has_rcvbuf; };
struct peer_entry { struct in6_addr address; uint32_t scope; };
static struct fd_state fd_states[kMaxFds];
static struct peer_entry peers[kMaxPeers];
static unsigned peer_count;
static int tcp_port, udp_ports[kMaxUdpPorts];
static unsigned udp_port_count;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static int (*next_socket)(int, int, int);
static int (*next_bind)(int, const struct sockaddr *, socklen_t);
static int (*next_connect)(int, const struct sockaddr *, socklen_t);
static int (*next_close)(int);
static int (*next_setsockopt)(int, int, int, const void *, socklen_t);
static int (*next_getsockopt)(int, int, int, void *, socklen_t *);
static int (*next_getsockname)(int, struct sockaddr *, socklen_t *);
static int (*next_getpeername)(int, struct sockaddr *, socklen_t *);
static int (*next_accept)(int, struct sockaddr *, socklen_t *);
static int (*next_accept4)(int, struct sockaddr *, socklen_t *, int);
static ssize_t (*next_sendto)(int, const void *, size_t, int, const struct sockaddr *, socklen_t);
static ssize_t (*next_recvfrom)(int, void *, size_t, int, struct sockaddr *, socklen_t *);
static ssize_t (*next_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*next_recvmsg)(int, struct msghdr *, int);
static int (*next_dup)(int);
static int (*next_dup2)(int, int);
static int (*next_dup3)(int, int, int);
static int (*next_fcntl)(int, int, ...);
static int (*next_epoll_ctl)(int, int, int, struct epoll_event *);
static void fork_prepare(void) {
    pthread_mutex_lock(&state_lock);
    for (int fd = 0; fd < kMaxFds; ++fd) {
        if (fd_states[fd].candidate) { fd_states[fd].shared = 1; }
    }
}
static void fork_parent(void) { pthread_mutex_unlock(&state_lock); }
static void fork_child(void) { pthread_mutex_unlock(&state_lock); }
static int parse_port(const char *key) {
    const char *value = getenv(key); if (!value || !*value) { return 0; }
    char *end = NULL; long p = strtol(value, &end, 10);
    if (*end || p < 1 || p > 65535) { return 0; } return (int)p;
}
/* A present plural setting overrides the legacy setting. An invalid list
 * disables UDP conversion as a whole; never partially select malformed input. */
static void parse_udp_ports(void) {
    const char *value = getenv("NEREUS_IPV6_UDP_PORTS");
    if (!value) {
        int port = parse_port("NEREUS_IPV6_UDP_PORT");
        if (port) { udp_ports[udp_port_count++] = port; }
        return;
    }
    while (*value) {
        if (*value < '0' || *value > '9' || udp_port_count == kMaxUdpPorts) { udp_port_count = 0; return; }
        char *end = NULL; long port = strtol(value, &end, 10);
        if (port < 1 || port > 65535 || (*end && *end != ',')) { udp_port_count = 0; return; }
        udp_ports[udp_port_count++] = (int)port;
        if (!*end) { return; }
        value = end + 1;
        if (!*value) { udp_port_count = 0; return; }
    }
}
static int selected_udp_port(int port) {
    for (unsigned i = 0; i < udp_port_count; ++i) { if (udp_ports[i] == port) { return 1; } }
    return 0;
}
static void initialize(void) {
    /* RTLD_NEXT resolves the next preload layer (or libc), avoiding our wrappers. */
    next_socket = dlsym(RTLD_NEXT, "socket"); next_bind = dlsym(RTLD_NEXT, "bind");
    next_connect = dlsym(RTLD_NEXT, "connect"); next_close = dlsym(RTLD_NEXT, "close");
    next_setsockopt = dlsym(RTLD_NEXT, "setsockopt"); next_getsockopt = dlsym(RTLD_NEXT, "getsockopt");
    next_getsockname = dlsym(RTLD_NEXT, "getsockname"); next_getpeername = dlsym(RTLD_NEXT, "getpeername");
    next_accept = dlsym(RTLD_NEXT, "accept"); next_accept4 = dlsym(RTLD_NEXT, "accept4");
    next_sendto = dlsym(RTLD_NEXT, "sendto"); next_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
    next_sendmsg = dlsym(RTLD_NEXT, "sendmsg"); next_recvmsg = dlsym(RTLD_NEXT, "recvmsg");
    next_dup = dlsym(RTLD_NEXT, "dup"); next_dup2 = dlsym(RTLD_NEXT, "dup2"); next_dup3 = dlsym(RTLD_NEXT, "dup3");
    next_fcntl = dlsym(RTLD_NEXT, "fcntl"); next_epoll_ctl = dlsym(RTLD_NEXT, "epoll_ctl");
    tcp_port = parse_port("NEREUS_IPV6_TCP_PORT"); parse_udp_ports();
    pthread_atfork(fork_prepare, fork_parent, fork_child);
}
static void init(void) { pthread_once(&once, initialize); }
static int in_range(int fd) { return fd >= 0 && fd < kMaxFds; }
static struct fd_state state(int fd) {
    struct fd_state result = {0}; pthread_mutex_lock(&state_lock);
    if (in_range(fd)) { result = fd_states[fd]; } pthread_mutex_unlock(&state_lock); return result;
}
static int reject(const char *why) {
    dprintf(STDERR_FILENO, "nereus_ipv6: selected bind rejected: %s\n", why);
    errno = EOPNOTSUPP; return -1;
}
static int token_range(uint32_t address) { return (address & 0xfffe0000U) == kTokenBase; }
/* Address aliases deliberately exclude transport ports: a TCP peer can advertise
 * a different UDP port. IPv6 address AND scope identify an alias; port stays in
 * the ordinary sockaddr_in field and distinguishes every transport endpoint. */
static int encode(const struct sockaddr_storage *source, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out)); out->sin_family = AF_INET;
    if (source->ss_family == AF_INET) { memcpy(out, source, sizeof(*out)); return 0; }
    if (source->ss_family != AF_INET6) { errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)source; out->sin_port = v6->sin6_port;
    if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) {
        memcpy(&out->sin_addr.s_addr, &v6->sin6_addr.s6_addr[12], 4);
        if (token_range(ntohl(out->sin_addr.s_addr))) { errno = EADDRNOTAVAIL; return -1; }
        return 0;
    }
    pthread_mutex_lock(&state_lock);
    unsigned index;
    for (index = 0; index < peer_count; ++index) {
        if (peers[index].scope == v6->sin6_scope_id && !memcmp(&peers[index].address, &v6->sin6_addr, 16)) { break; }
    }
    if (index == peer_count) {
        if (peer_count == kMaxPeers) { pthread_mutex_unlock(&state_lock); errno = ENOSPC; return -1; }
        peers[index].address = v6->sin6_addr; peers[index].scope = v6->sin6_scope_id; ++peer_count;
    }
    out->sin_addr.s_addr = htonl(kTokenBase + index + 1);
    pthread_mutex_unlock(&state_lock); return 0;
}
static int decode(const struct sockaddr *source, socklen_t len, struct sockaddr_in6 *out) {
    if (!source || len < sizeof(struct sockaddr_in) || source->sa_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)source;
    memset(out, 0, sizeof(*out)); out->sin6_family = AF_INET6; out->sin6_port = v4->sin_port;
    uint32_t address = ntohl(v4->sin_addr.s_addr);
    if (token_range(address)) {
        unsigned index = address - kTokenBase;
        pthread_mutex_lock(&state_lock);
        if (!index || index > peer_count) { pthread_mutex_unlock(&state_lock); errno = EADDRNOTAVAIL; return -1; }
        out->sin6_addr = peers[index - 1].address; out->sin6_scope_id = peers[index - 1].scope;
        pthread_mutex_unlock(&state_lock);
    } else {
        out->sin6_addr.s6_addr[10] = 0xff; out->sin6_addr.s6_addr[11] = 0xff;
        memcpy(&out->sin6_addr.s6_addr[12], &v4->sin_addr.s_addr, 4);
    }
    return 0;
}
static int copy_address(struct sockaddr *out, socklen_t *len, const struct sockaddr_in *v4) {
    if (!len) { errno = EFAULT; return -1; }
    socklen_t n = *len < sizeof(*v4) ? *len : (socklen_t)sizeof(*v4);
    if (!out && n) { errno = EFAULT; return -1; }
    if (out && n) { memcpy(out, v4, n); } *len = sizeof(*v4); return 0;
}
int socket(int domain, int type, int protocol) {
    init(); pthread_mutex_lock(&state_lock); int fd = next_socket(domain, type, protocol); int saved = errno;
    if (in_range(fd)) {
        fd_states[fd] = (struct fd_state){0};
        int base = type & ~(SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (domain == AF_INET && (base == SOCK_STREAM || base == SOCK_DGRAM)) {
            fd_states[fd] = (struct fd_state){.candidate = 1, .type = base, .protocol = protocol};
        }
    }
    pthread_mutex_unlock(&state_lock); errno = saved; return fd;
}
int setsockopt(int fd, int level, int option, const void *value, socklen_t len) {
    init(); pthread_mutex_lock(&state_lock);
    int result = next_setsockopt(fd, level, option, value, len); int saved = errno;
    if (!result && in_range(fd) && fd_states[fd].candidate && !fd_states[fd].converted) {
        if (level == SOL_SOCKET && option == SO_REUSEADDR && value && len >= sizeof(int)) { memcpy(&fd_states[fd].reuse, value, sizeof(int)); }
        else if (level == IPPROTO_TCP && option == TCP_NODELAY && value && len >= sizeof(int)) {
            memcpy(&fd_states[fd].nodelay, value, sizeof(int)); fd_states[fd].has_nodelay = 1;
        } else if (level == SOL_SOCKET && option == SO_RCVBUF && value && len >= sizeof(int)) {
            /* Linux getsockopt returns doubled/clamped SO_RCVBUF. Replay the
             * application's last request, never that kernel result. */
            memcpy(&fd_states[fd].rcvbuf, value, sizeof(int)); fd_states[fd].has_rcvbuf = 1;
        } else { fd_states[fd].unsupported = 1; }
    }
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
int bind(int fd, const struct sockaddr *address, socklen_t len) {
    init();
    if (!address || len < sizeof(struct sockaddr_in) || address->sa_family != AF_INET) { return next_bind(fd, address, len); }
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)address;
    struct fd_state s = state(fd); int port = ntohs(v4->sin_port);
    if (v4->sin_addr.s_addr != htonl(INADDR_ANY) || !port ||
        !((s.type == SOCK_STREAM && port == tcp_port) || (s.type == SOCK_DGRAM && selected_udp_port(port)))) {
        int result = next_bind(fd, address, len); int saved = errno;
        if (!result && in_range(fd)) { pthread_mutex_lock(&state_lock); fd_states[fd].candidate = 0; pthread_mutex_unlock(&state_lock); }
        errno = saved; return result;
    }
    if (!s.candidate || s.converted || s.shared || s.unsupported) { return reject("requires fresh, unshared socket with only SO_REUSEADDR/TCP_NODELAY/SO_RCVBUF before bind"); }
    pthread_mutex_lock(&state_lock);
    int status = next_fcntl(fd, F_GETFL); int descriptor_flags = next_fcntl(fd, F_GETFD);
    if (status < 0 || descriptor_flags < 0) { int saved = errno; pthread_mutex_unlock(&state_lock); errno = saved; return -1; }
    int type = s.type | ((status & O_NONBLOCK) ? SOCK_NONBLOCK : 0) | ((descriptor_flags & FD_CLOEXEC) ? SOCK_CLOEXEC : 0);
    int replacement = next_socket(AF_INET6, type, s.protocol);
    if (replacement < 0) { int saved = errno; pthread_mutex_unlock(&state_lock); errno = saved; return -1; }
    int zero = 0;
    struct sockaddr_in6 v6 = {.sin6_family = AF_INET6, .sin6_port = v4->sin_port, .sin6_addr = IN6ADDR_ANY_INIT};
    int result = next_setsockopt(replacement, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));
    if (!result) { result = next_setsockopt(replacement, SOL_SOCKET, SO_REUSEADDR, &s.reuse, sizeof(s.reuse)); }
    if (!result && s.has_nodelay) { result = next_setsockopt(replacement, IPPROTO_TCP, TCP_NODELAY, &s.nodelay, sizeof(s.nodelay)); }
    if (!result && s.has_rcvbuf) { result = next_setsockopt(replacement, SOL_SOCKET, SO_RCVBUF, &s.rcvbuf, sizeof(s.rcvbuf)); }
    if (!result) { result = next_bind(replacement, (const struct sockaddr *)&v6, sizeof(v6)); }
    if (!result) { result = next_dup3(replacement, fd, (descriptor_flags & FD_CLOEXEC) ? O_CLOEXEC : 0); }
    int saved = errno; next_close(replacement);
    if (result >= 0) { fd_states[fd].converted = 1; fd_states[fd].candidate = 0; result = 0; }
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
int getsockopt(int fd, int level, int option, void *value, socklen_t *len) {
    init(); if (state(fd).converted && level == SOL_SOCKET && option == SO_DOMAIN) {
        if (!len || !value) { errno = EFAULT; return -1; }
        int domain = AF_INET; socklen_t n = *len < sizeof(domain) ? *len : (socklen_t)sizeof(domain);
        memcpy(value, &domain, n); *len = n; return 0;
    } return next_getsockopt(fd, level, option, value, len);
}
int getsockname(int fd, struct sockaddr *address, socklen_t *len) {
    init(); if (!state(fd).converted) { return next_getsockname(fd, address, len); }
    if (!len) { errno = EFAULT; return -1; }
    struct sockaddr_storage real; socklen_t rl = sizeof(real);
    int result = next_getsockname(fd, (struct sockaddr *)&real, &rl); int saved = errno;
    if (result) { return result; }
    struct sockaddr_in v4 = {.sin_family = AF_INET};
    if (real.ss_family == AF_INET6) {
        const struct sockaddr_in6 *v6 = (const struct sockaddr_in6 *)&real; v4.sin_port = v6->sin6_port;
        if (IN6_IS_ADDR_V4MAPPED(&v6->sin6_addr)) { memcpy(&v4.sin_addr.s_addr, &v6->sin6_addr.s6_addr[12], 4); }
    } else { memcpy(&v4, &real, sizeof(v4)); }
    result = copy_address(address, len, &v4); if (!result) { errno = saved; } return result;
}
int getpeername(int fd, struct sockaddr *address, socklen_t *len) {
    init(); if (!state(fd).converted) { return next_getpeername(fd, address, len); }
    if (!len) { errno = EFAULT; return -1; }
    struct sockaddr_storage real; socklen_t rl = sizeof(real);
    int result = next_getpeername(fd, (struct sockaddr *)&real, &rl); int saved = errno;
    if (result) { return result; } struct sockaddr_in v4;
    if (encode(&real, &v4)) { return -1; }
    result = copy_address(address, len, &v4); if (!result) { errno = saved; } return result;
}
static int accept_common(int fd, struct sockaddr *address, socklen_t *len, int flags, int use4) {
    init(); if (!state(fd).converted) { return use4 ? next_accept4(fd, address, len, flags) : next_accept(fd, address, len); }
    if (address && !len) { errno = EFAULT; return -1; }
    struct sockaddr_storage real; socklen_t rl = sizeof(real);
    int client = use4 ? next_accept4(fd, (struct sockaddr *)&real, &rl, flags) : next_accept(fd, (struct sockaddr *)&real, &rl);
    int saved = errno; if (client < 0) { return client; }
    struct sockaddr_in v4;
    if (!in_range(client) || encode(&real, &v4)) { int err = in_range(client) ? errno : EMFILE; next_close(client); errno = err; return -1; }
    pthread_mutex_lock(&state_lock); fd_states[client] = (struct fd_state){.converted = 1, .type = SOCK_STREAM}; pthread_mutex_unlock(&state_lock);
    if (address) { copy_address(address, len, &v4); }
    errno = saved; return client;
}
int accept(int fd, struct sockaddr *address, socklen_t *len) { return accept_common(fd, address, len, 0, 0); }
int accept4(int fd, struct sockaddr *address, socklen_t *len, int flags) { return accept_common(fd, address, len, flags, 1); }
int connect(int fd, const struct sockaddr *address, socklen_t len) {
    init(); struct fd_state s = state(fd);
    if (s.converted && address && address->sa_family == AF_INET) {
        struct sockaddr_in6 v6; if (decode(address, len, &v6)) { return -1; }
        return next_connect(fd, (const struct sockaddr *)&v6, sizeof(v6));
    }
    int result = next_connect(fd, address, len); int saved = errno;
    if (s.candidate) { pthread_mutex_lock(&state_lock); fd_states[fd].unsupported = 1; pthread_mutex_unlock(&state_lock); }
    errno = saved; return result;
}
ssize_t sendto(int fd, const void *data, size_t size, int flags, const struct sockaddr *address, socklen_t len) {
    init(); if (!state(fd).converted || !address) { return next_sendto(fd, data, size, flags, address, len); }
    struct sockaddr_in6 v6; if (decode(address, len, &v6)) { return -1; }
    return next_sendto(fd, data, size, flags, (const struct sockaddr *)&v6, sizeof(v6));
}
ssize_t recvfrom(int fd, void *data, size_t size, int flags, struct sockaddr *address, socklen_t *len) {
    init(); if (!state(fd).converted || !address) { return next_recvfrom(fd, data, size, flags, address, len); }
    if (!len) { errno = EFAULT; return -1; }
    struct sockaddr_storage real; socklen_t rl = sizeof(real);
    ssize_t result = next_recvfrom(fd, data, size, flags, (struct sockaddr *)&real, &rl); int saved = errno;
    if (result < 0) { return result; } struct sockaddr_in v4;
    if (encode(&real, &v4) || copy_address(address, len, &v4)) { return -1; } errno = saved; return result;
}
ssize_t sendmsg(int fd, const struct msghdr *message, int flags) {
    init(); if (!state(fd).converted || !message || !message->msg_name) { return next_sendmsg(fd, message, flags); }
    struct sockaddr_in6 v6; if (decode(message->msg_name, message->msg_namelen, &v6)) { return -1; }
    struct msghdr translated = *message; translated.msg_name = &v6; translated.msg_namelen = sizeof(v6);
    return next_sendmsg(fd, &translated, flags);
}
ssize_t recvmsg(int fd, struct msghdr *message, int flags) {
    init(); if (!state(fd).converted || !message || !message->msg_name) { return next_recvmsg(fd, message, flags); }
    struct sockaddr_storage real; struct msghdr translated = *message;
    translated.msg_name = &real; translated.msg_namelen = sizeof(real);
    ssize_t result = next_recvmsg(fd, &translated, flags); int saved = errno;
    if (result < 0) { return result; }
    message->msg_flags = translated.msg_flags; message->msg_controllen = translated.msg_controllen;
    if (translated.msg_namelen) {
        struct sockaddr_in v4; if (encode(&real, &v4)) { return -1; }
        socklen_t len = message->msg_namelen; copy_address(message->msg_name, &len, &v4); message->msg_namelen = len;
    } else { message->msg_namelen = 0; }
    errno = saved; return result;
}
int close(int fd) {
    init(); pthread_mutex_lock(&state_lock); int result = next_close(fd); int saved = errno;
    if (in_range(fd)) { fd_states[fd] = (struct fd_state){0}; } pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
static void duplicated(int source, int target) {
    if (target < 0 || source == target) { return; }
    if (in_range(target)) { fd_states[target] = in_range(source) ? fd_states[source] : (struct fd_state){0}; }
    if (in_range(source) && fd_states[source].candidate) {
        fd_states[source].shared = 1; if (in_range(target)) { fd_states[target].shared = 1; }
    }
}
int dup(int fd) {
    init(); pthread_mutex_lock(&state_lock); int result = next_dup(fd); int saved = errno; duplicated(fd, result);
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
int dup2(int fd, int target) {
    init(); pthread_mutex_lock(&state_lock); int result = next_dup2(fd, target); int saved = errno; duplicated(fd, result);
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
int dup3(int fd, int target, int flags) {
    init(); pthread_mutex_lock(&state_lock); int result = next_dup3(fd, target, flags); int saved = errno; duplicated(fd, result);
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
static unsigned long fcntl_argument(int command, va_list arguments) {
    switch (command) {
        case F_GETFD: case F_GETFL: case F_GETOWN: case F_GETSIG:
        case F_GETLEASE: case F_GETPIPE_SZ: case F_GET_SEALS:
            return 0;
        case F_DUPFD: case F_DUPFD_CLOEXEC: case F_SETFD: case F_SETFL:
        case F_SETOWN: case F_SETSIG: case F_SETLEASE: case F_SETPIPE_SZ:
        case F_ADD_SEALS:
            return (unsigned long)va_arg(arguments, int);
        case F_NOTIFY:
            return va_arg(arguments, unsigned long);
        default:
            return (unsigned long)va_arg(arguments, void *);
    }
}
static int fcntl_common(int fd, int command, unsigned long argument) {
    init(); pthread_mutex_lock(&state_lock); int result = next_fcntl(fd, command, argument); int saved = errno;
    if (command == F_DUPFD || command == F_DUPFD_CLOEXEC) { duplicated(fd, result); }
    if (result >= 0 && in_range(fd) && fd_states[fd].candidate) {
        if (command == F_SETOWN || command == F_SETOWN_EX || command == F_SETSIG || command == F_SETLEASE || command == F_NOTIFY
            || (command == F_SETFL && (next_fcntl(fd, F_GETFL) & O_ASYNC))) { fd_states[fd].unsupported = 1; }
    }
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
int fcntl(int fd, int command, ...) {
    va_list arguments; va_start(arguments, command); unsigned long argument = fcntl_argument(command, arguments); va_end(arguments);
    return fcntl_common(fd, command, argument);
}
int fcntl64(int fd, int command, ...) {
    va_list arguments; va_start(arguments, command); unsigned long argument = fcntl_argument(command, arguments); va_end(arguments);
    return fcntl_common(fd, command, argument);
}
int epoll_ctl(int epoll, int operation, int fd, struct epoll_event *event) {
    init(); pthread_mutex_lock(&state_lock); int result = next_epoll_ctl(epoll, operation, fd, event); int saved = errno;
    if (!result && operation == EPOLL_CTL_ADD && in_range(fd) && fd_states[fd].candidate) { fd_states[fd].unsupported = 1; }
    pthread_mutex_unlock(&state_lock); errno = saved; return result;
}
