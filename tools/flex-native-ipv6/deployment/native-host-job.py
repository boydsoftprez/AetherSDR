#!/usr/bin/env python3
"""Original local native-host job entry point; no SSH or access bootstrap.
Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
"""
import argparse
import os
import pathlib
import re
import runpy

ROOT = pathlib.Path(__file__).resolve().parent
JOBS = {
    'accept': 'host-smartsdr-acceptance.py',
    'revert': 'host-smartsdr-revert.py',
    'boot-edit': 'native-ipv6-host-edit.py',
    'network-enable': 'native-ipv6-network-enable.py',
    'host-rollback': 'native-ipv6-host-rollback.py',
}


def expected_digest(value):
    if re.fullmatch(r'[0-9a-f]{64}', value) is None:
        raise argparse.ArgumentTypeError('expected-sha256 must be 64 lowercase hex characters')
    return value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    selector = parser.add_mutually_exclusive_group(required=True)
    selector.add_argument('--job', choices=JOBS)
    selector.add_argument('--file', type=pathlib.Path, help='trusted owner-reviewed native job source')
    parser.add_argument('--workdir', required=True, type=pathlib.Path)
    parser.add_argument('--expected-sha256', required=True, type=expected_digest)
    args = parser.parse_args()
    if not __debug__:
        parser.error('Python assertions must remain enabled for firmware-specific host jobs')
    workdir = args.workdir.resolve()
    job = ROOT / JOBS[args.job] if args.job else args.file.resolve()
    if not workdir.is_dir() or not job.is_file():
        parser.error('workdir must exist and job must be a regular source file')
    if args.job and os.geteuid() != 0:
        parser.error('named native host jobs require owner-authorized host root')
    runpy.run_path(str(job), init_globals={
        'WORKDIR': str(workdir), 'EXPECTED_SHA256': args.expected_sha256,
    }, run_name='__main__')


if __name__ == '__main__':
    main()
