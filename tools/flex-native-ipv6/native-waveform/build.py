#!/usr/bin/env python3
"""Build an operator-private native waveform from the tested clean-room sources.
Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
"""
import argparse
import io
import os
import pathlib
import secrets
import shutil
import subprocess
import tempfile
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent
PACKAGE_FILES = ('NereusNativeProbe.cfg', 'probe.sh', 'probe-arm64', 'probe-armv7', 'LICENSE', 'LICENSE-Go')


def create_package(stage, target):
    stage, target = pathlib.Path(stage), pathlib.Path(target)
    if target.exists() or target.is_symlink():
        raise FileExistsError(target)
    for name in PACKAGE_FILES:
        path = stage / name
        if path.is_symlink() or not path.is_file():
            raise ValueError('missing or symlink package input: ' + name)
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as archive:
        for name in PACKAGE_FILES:
            info = zipfile.ZipInfo('NereusNativeProbe/' + name)
            info.create_system = 3
            info.compress_type = zipfile.ZIP_DEFLATED
            mode = 0o755 if name.startswith('probe') else 0o644
            info.external_attr = (0o100000 | mode) << 16
            archive.writestr(info, (stage / name).read_bytes())
    payload = buffer.getvalue()
    if not payload or len(payload) > 8 * 1024 * 1024:
        raise ValueError('package exceeds tested upload size bound')
    fd, temporary = tempfile.mkstemp(prefix='.native-probe-', dir=target.parent)
    try:
        with os.fdopen(fd, 'wb') as output:
            output.write(payload)
            output.flush()
            os.fsync(output.fileno())
        # Atomic publication without replacing an existing operator artifact.
        os.link(temporary, target)
    finally:
        os.unlink(temporary)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output-dir', required=True, type=pathlib.Path,
                        help='new private directory outside the Git checkout')
    args = parser.parse_args()
    output = args.output_dir.expanduser().resolve()
    repo = ROOT.parents[2]
    if output == repo or repo in output.parents:
        parser.error('private output directory must be outside the checkout')
    output.mkdir(mode=0o700, parents=True, exist_ok=False)
    token = output / 'client-token.txt'
    fd = os.open(token, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w') as file:
        file.write(secrets.token_hex(32) + '\n')
    stage = output / 'NereusNativeProbe'
    stage.mkdir(mode=0o700)
    for name in ('NereusNativeProbe.cfg', 'probe.sh', 'LICENSE', 'LICENSE-Go'):
        shutil.copyfile(ROOT / name, stage / name)
    build = '''set -eu
cp /source/*.go /source/go.mod /work/
cd /work
go test -v ./...
probe_token=$(cat /out/client-token.txt)
CGO_ENABLED=0 GOOS=linux GOARCH=arm64 go build -trimpath -ldflags "-s -w -X main.authTokenHex=$probe_token" -o /out/NereusNativeProbe/probe-arm64 .
CGO_ENABLED=0 GOOS=linux GOARCH=arm GOARM=7 go build -trimpath -ldflags "-s -w -X main.authTokenHex=$probe_token" -o /out/NereusNativeProbe/probe-armv7 .
'''
    subprocess.run(['docker', 'run', '--rm', '--network', 'none', '--read-only',
                    '--cap-drop', 'ALL', '--security-opt', 'no-new-privileges',
                    '--tmpfs', '/tmp:exec', '--tmpfs', '/work:exec',
                    '-e', 'GOCACHE=/tmp/go-cache', '-e', 'GOMODCACHE=/tmp/go-mod',
                    '-v', str(ROOT) + ':/source:ro', '-v', str(output) + ':/out',
                    'golang:1.25-bookworm', 'sh', '-c', build], check=True)
    for name in ('probe.sh', 'probe-arm64', 'probe-armv7'):
        os.chmod(stage / name, 0o755)
    create_package(stage, output / 'NereusNativeProbe.ssdr_waveform')
    print('Built private native probe package; retain this directory and token privately: ' + str(output))


if __name__ == '__main__':
    main()
