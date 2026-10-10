#!/bin/sh
# Copyright (c) 2026 J.J. Boyd. Original diagnostic source, authored with Codex.
# SPDX-License-Identifier: MIT
set -eu
mkdir -p /run/sshd
if [ ! -f /etc/ssh/ssh_host_ed25519_key ]; then
    ssh-keygen -q -t ed25519 -N '' -f /etc/ssh/ssh_host_ed25519_key
fi
printf 'Nereus network probe: SSH port 22222, key authentication only.\n'
ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub
if [ -n "${SSDR_RADIO_ADDRESS:-}" ]; then
    printf 'Flex-provided radio address: %s\n' "$SSDR_RADIO_ADDRESS"
    printf '%s\n' "$SSDR_RADIO_ADDRESS" > /run/ssdr-radio-address
fi
net-report > /run/network-startup.txt 2>&1
cat /run/network-startup.txt
/usr/sbin/sshd -t
exec /usr/sbin/sshd -D -e
