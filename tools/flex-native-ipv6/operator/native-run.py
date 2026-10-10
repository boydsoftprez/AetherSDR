#!/usr/bin/env python3
"""Owner-authorized bounded native jobs via the authenticated root-FD helper.

Original J.J.Boyd / Codex. All copied host files and job containers are temporary.
No credentials are included in diagnostic output. See README.md for prerequisites.
"""
# Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT
import argparse
import json
import os
import pathlib
import re
import shlex
import socket
import stat
import subprocess
import sys
import time
import uuid
import zipfile

HERE = pathlib.Path(__file__).resolve().parent


def private_file(path):
    if not path.is_file() or path.is_symlink():
        raise ValueError('expected a regular private input file')
    if stat.S_IMODE(path.stat().st_mode) & 0o077:
        raise ValueError('token and native package must be private (chmod 600)')


def private_output(path):
    """Open operator evidence without following symlinks or inheriting public mode."""
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC | os.O_NOFOLLOW, 0o600)
    os.fchmod(fd, 0o600)
    return os.fdopen(fd, 'w')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('job', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    parser.add_argument('--host', required=True, help='Owner-controlled radio hostname or address')
    parser.add_argument('--ssh-key', required=True, type=pathlib.Path)
    parser.add_argument('--known-hosts', required=True, type=pathlib.Path)
    parser.add_argument('--token-file', required=True, type=pathlib.Path)
    parser.add_argument('--native-package', required=True, type=pathlib.Path,
                        help='Fresh private NereusNativeProbe.ssdr_waveform built with the same token')
    parser.add_argument('--state-dir', required=True, type=pathlib.Path)
    parser.add_argument('--expected-sha256', help='Explicit hash of independently built shim for accept/revert jobs')
    parser.add_argument('--file', action='append', default=[], type=pathlib.Path)
    args = parser.parse_args(argv)
    try:
        if not args.host or args.host.startswith('-') or any(c.isspace() for c in args.host) or any(c in args.host for c in '@/[]'):
            raise ValueError('invalid operator host; supply an address or hostname without user or brackets')
        if args.expected_sha256 is not None and not re.fullmatch('[0-9a-f]{64}', args.expected_sha256):
            raise ValueError('--expected-sha256 must contain exactly 64 lowercase hexadecimal characters')
        for path in (args.ssh_key, args.known_hosts):
            if not path.is_file():
                raise ValueError('SSH key and known-hosts file must exist')
        private_file(args.token_file)
        token = args.token_file.read_text().strip()
        if not re.fullmatch('[0-9a-f]{64}', token):
            raise ValueError('invalid receiver authentication configuration')
        private_file(args.native_package)
        if args.native_package.name != 'NereusNativeProbe.ssdr_waveform':
            raise ValueError('expected freshly built NereusNativeProbe.ssdr_waveform')
        if not 0 < args.native_package.stat().st_size <= 8 * 1024 * 1024 or not zipfile.is_zipfile(args.native_package):
            raise ValueError('native package must be a bounded waveform ZIP archive')
        job = args.job.read_text()
        compile(job, str(args.job), 'exec')
        sources = {}
        for source in args.file:
            path = source.resolve()
            if not path.is_file() or path.name in sources:
                raise ValueError('missing or duplicate job file')
            sources[path.name] = path
        args.state_dir.mkdir(parents=True, exist_ok=True, mode=0o700)
        if args.state_dir.is_symlink() or stat.S_IMODE(args.state_dir.stat().st_mode) & 0o077:
            raise ValueError('state directory must be private (chmod 700)')
        args.output.parent.mkdir(parents=True, exist_ok=True)
        tag = uuid.uuid4().hex[:12]
        files = {name: '/tmp/nereus-' + tag + '-' + name for name in sources}
        prefix = (HERE / 'native-rootfd-prefix.py.inc').read_text()
        tail = (HERE / 'native-job-tail.py.inc').read_text()
        # Substitute the tail placeholders once; uploaded job text is opaque.
        # A job containing a placeholder name must not be modified by replacement.
        tail = re.sub('FILES_LITERAL|EXPECTED_SHA_LITERAL|JOB_LITERAL',
                      lambda match: {'FILES_LITERAL': repr(files),
                                     'EXPECTED_SHA_LITERAL': repr(args.expected_sha256),
                                     'JOB_LITERAL': repr(job)}[match.group()], tail)
        receiver_source = prefix + tail
        compile(receiver_source, 'receiver', 'exec')
    except (OSError, ValueError, SyntaxError) as error:
        parser.error(str(error))

    common = ['-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'ConnectTimeout=5',
              '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile=' + str(args.known_hosts.resolve()),
              '-i', str(args.ssh_key.resolve())]
    ssh = ['ssh', *common, '-p', '22222', 'root@' + args.host]
    scp = ['scp', *common, '-P', '22222']
    remote_host = '[' + args.host + ']' if ':' in args.host else args.host
    remote = 'root@' + remote_host + ':'
    local_receiver = args.state_dir / ('receiver-' + tag + '.py')
    with private_output(local_receiver) as script:
        script.write(receiver_source)
    remote_receiver = '/tmp/nereus-receiver-' + tag + '.py'
    receiver = None
    upload = None
    try:
        for name, path in sources.items():
            subprocess.run(scp + [str(path), remote + files[name]], check=True, capture_output=True)
        subprocess.run(scp + [str(local_receiver), remote + remote_receiver], check=True, capture_output=True)
        with private_output(args.output) as log, private_output(args.state_dir / ('upload-' + tag + '.log')) as upload_log:
            receiver = subprocess.Popen(ssh + ['python3 ' + shlex.quote(remote_receiver)],
                                        stdin=subprocess.PIPE, stdout=log, stderr=subprocess.STDOUT, text=True)
            receiver.stdin.write(token + '\n')
            receiver.stdin.close()
            for _ in range(50):
                if 'READY:' in args.output.read_text():
                    break
                if receiver.poll() is not None:
                    raise RuntimeError('native receiver exited before ready')
                time.sleep(.2)
            else:
                raise RuntimeError('receiver not ready')
            upload = subprocess.Popen([sys.executable, str(HERE / 'native-waveform-upload.py'),
                                       '--host', args.host, '--package', str(args.native_package.resolve())],
                                      stdout=upload_log, stderr=subprocess.STDOUT)
            for _ in range(80):
                try:
                    channel = socket.create_connection((args.host, 22223), timeout=.5)
                    break
                except OSError:
                    time.sleep(.25)
            else:
                raise RuntimeError('native descriptor helper not reachable')
            with channel:
                channel.sendall(token.encode() + b':host-root-fd\n')
                channel.settimeout(8)
                reply = json.loads(channel.recv(16384))
                if not reply.get('bridge_sent') or reply.get('peer_uid') != 0:
                    raise RuntimeError('native root-FD delegation failed')
            result = receiver.wait(timeout=660)
            upload_rc = upload.wait(timeout=20)
        try:
            report = json.loads(args.output.read_text().split('\n', 1)[1])
        except (ValueError, IndexError) as error:
            raise RuntimeError('Native receiver exited ' + str(result) +
                               ' before a JSON report; inspect saved output and radio boot state') from error
        report['orchestration'] = {'receiver_exit': result, 'waveform_upload_exit': upload_rc,
                                   'authenticated_native_bridge_delivered': True}
        # Once the authenticated descriptor bridge and native job have succeeded, a
        # service restart may disconnect the original waveform upload connection.
        # Preserve that diagnostic separately from the actual native job outcome.
        with private_output(args.output) as output:
            output.write(json.dumps(report, indent=2) + '\n')
        print(json.dumps(report, indent=2))
        if result or report['wait'].get('StatusCode'):
            raise SystemExit('native job failed; inspect saved evidence')
        return 0
    finally:
        # Reap local SSH/upload children on failed orchestration as well as success.
        # Native jobs still have their independent 600-second deadline and cleanup.
        for process in (receiver, upload):
            if process is not None and process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
        subprocess.run(ssh + ['rm -f -- ' + ' '.join(shlex.quote(path) for path in [remote_receiver, *files.values()])],
                       capture_output=True)


if __name__ == '__main__':
    raise SystemExit(main())
