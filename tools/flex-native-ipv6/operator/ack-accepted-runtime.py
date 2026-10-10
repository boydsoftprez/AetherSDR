#!/usr/bin/env python3
"""Administrative acceptance only, through existing authenticated owner SSH."""
# Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT
import argparse
import json
import os
import pathlib
import shlex
import subprocess


# Original acceptance socket message and loopback endpoint; no public listener.
CODE = "import socket; c=socket.create_connection(('127.0.0.1',46861),timeout=3); c.sendall(b'{\"accept\":true}\\n'); c.settimeout(3); print(c.recv(512).decode()); c.close()"


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', required=True)
    parser.add_argument('--ssh-key', required=True, type=pathlib.Path)
    parser.add_argument('--known-hosts', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    args = parser.parse_args(argv)
    if not args.host or args.host.startswith('-') or any(c.isspace() for c in args.host) or any(c in args.host for c in '@/[]'):
        parser.error('supply an explicit host address or hostname without user or brackets')
    if not args.ssh_key.is_file() or not args.known_hosts.is_file():
        parser.error('SSH key and known-hosts file must exist')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    command = ['ssh', '-oBatchMode=yes', '-oIdentitiesOnly=yes', '-oConnectTimeout=5',
               '-oStrictHostKeyChecking=yes', '-oUserKnownHostsFile=' + str(args.known_hosts.resolve()),
               '-i', str(args.ssh_key.resolve()), '-p', '22222', 'root@' + args.host,
               'python3 -c ' + shlex.quote(CODE)]
    result = subprocess.run(command, capture_output=True, text=True, timeout=12)
    fd = os.open(args.output, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, 'w') as output:
        output.write(json.dumps({'rc': result.returncode, 'stdout': result.stdout, 'stderr': result.stderr}, indent=2) + '\n')
    print(result.stdout.strip())
    if result.returncode != 0:
        raise SystemExit('administrative ACK SSH failed; inspect saved evidence')
    try:
        acknowledged = json.loads(result.stdout).get('acknowledged') is True
    except ValueError:
        acknowledged = False
    if not acknowledged:
        raise SystemExit('administrative acceptance was not acknowledged')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
