#!/bin/sh
# Copyright (c) 2026 J.J. Boyd. Original diagnostic source, authored with Codex.
# SPDX-License-Identifier: MIT
# Inspect only. Does not change interfaces, routes, sysctls, or radio state.
set -u
printf '\nKernel and container identity\n'
uname -a
id
printf '\nFlex-provided radio address\n'
cat /run/ssdr-radio-address 2>/dev/null || true
printf '\nInterface addresses\n'
ip -brief address
printf '\nIPv4 routes\n'
ip -4 route show table all
printf '\nIPv6 routes\n'
ip -6 route show table all
printf '\nIPv6 interface state\n'
cat /proc/net/if_inet6 2>/dev/null || true
for item in /proc/sys/net/ipv6/conf/*/disable_ipv6 /proc/sys/net/ipv6/conf/*/accept_ra /proc/sys/net/ipv6/conf/*/forwarding; do
    if [ -r "$item" ]; then
        printf '%s: ' "$item"
        cat "$item"
    fi
done
printf '\nContainer capabilities and seccomp\n'
awk '/^Cap|^Seccomp|^NoNewPrivs/ { print }' /proc/self/status
printf '\nDNS configuration\n'
cat /etc/resolv.conf
printf '\nListening TCP and UDP sockets (process visibility is container-scoped)\n'
ss -lntu
printf '\nIPv6 socket availability\n'
python3 -c 'import socket; s = socket.socket(socket.AF_INET6, socket.SOCK_STREAM); print("AF_INET6 socket creation succeeded"); s.close()' || true
