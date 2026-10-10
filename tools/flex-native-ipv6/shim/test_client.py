#!/usr/bin/env python3
# Copyright (c) 2026 J.J. Boyd. Original clean-room test code, authored with Codex.
# SPDX-License-Identifier: MIT
import argparse
import ipaddress
import os
import pathlib
import socket
import signal
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent

def fields(line):
    return dict(item.split('=', 1) for item in line.strip().split()[1:])

def session(host, mode, expected_pid=None):
    family = socket.AF_INET6 if ':' in host else socket.AF_INET
    with socket.socket(family, socket.SOCK_DGRAM) as udp, socket.socket(family, socket.SOCK_STREAM) as tcp:
        udp.settimeout(4); tcp.settimeout(4)
        udp.bind(('::' if family == socket.AF_INET6 else '0.0.0.0', 0))
        tcp.connect((host, 46092))
        with tcp.makefile('rb') as stream:
            greeting = stream.readline().decode(); info = fields(greeting)
            assert info['family'] == '2', greeting
            if expected_pid is not None: assert int(info['pid']) == expected_pid, greeting
            peer = info['peer']
            if family == socket.AF_INET6: assert ipaddress.ip_address(peer) in ipaddress.ip_network('198.18.0.0/15'), greeting
            else: assert peer == tcp.getsockname()[0], greeting
            assert int(info['port']) == tcp.getsockname()[1], greeting
            port = udp.getsockname()[1]
            tcp.sendall(f'UDPPORT {port} {mode}\n'.encode())
            data, source = udp.recvfrom(512)
            assert source[1] == 46093, source
            assert ipaddress.ip_address(source[0].split('%')[0]) == ipaddress.ip_address(host.split('%')[0]), source
            assert fields(data.decode())['token'] == peer, data
            assert fields(data.decode())['pid'] == info['pid'], data
            udp.sendto(b'ACK', (host, 46093))
            ack = stream.readline().decode(); result = fields(ack)
            assert result == {'family': '2', 'peer': peer, 'port': str(port)}, ack
            print(f'PASS {host} {mode} {greeting.strip()} {ack.strip()}', flush=True)
            return peer, int(info['pid'])

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--server', default='::1')
    ap.add_argument('--external', action='store_true')
    ap.add_argument('--red', action='store_true')
    ap.add_argument('--fork', action='store_true')
    ap.add_argument('--deadline-seconds', type=int, default=50)
    args = ap.parse_args()
    def deadline(signum, frame):
        raise TimeoutError('client total deadline exceeded')
    signal.signal(signal.SIGALRM, deadline)
    signal.alarm(args.deadline_seconds)
    proc = None
    try:
        if not args.external:
            env = dict(os.environ)
            if not args.red:
                env.update(LD_PRELOAD=str(ROOT / 'libnereus_ipv6.so'), NEREUS_IPV6_TCP_PORT='46092', NEREUS_IPV6_UDP_PORT='46093')
            else:
                env.pop('LD_PRELOAD', None)
            command = [str(ROOT / 'ipv4_fixture'), '--iterations', '1' if args.red else '6', '--timeout-ms', '5000']
            if args.fork: command.append('--fork')
            proc = subprocess.Popen(command, env=env, stdout=subprocess.PIPE, text=True)
            startup = proc.stdout.readline().strip(); print(startup, flush=True)
            assert startup.startswith('READY '), startup
            expected_pid = int(fields(startup)['pid']) if not args.fork else None
        else: expected_pid = None
        if args.red:
            # This assertion must fail because the fixture has only a real AF_INET listener.
            session(args.server, 'FROM', expected_pid)
            raise AssertionError('RED unexpectedly connected')
        peers = []
        for host, mode in [(args.server, 'FROM'), (args.server, 'MSG'), ('127.0.0.1', 'FROM'), ('127.0.0.1', 'MSG'), (args.server, 'FROM'), (args.server, 'MSG')]:
            if args.external and host == '127.0.0.1':
                # External deployment can give a reachable IPv4 address explicitly.
                host = os.environ.get('NEREUS_TEST_IPV4_SERVER', host)
            peer, session_pid = session(host, mode, expected_pid)
            peers.append(peer)
            if args.external and not args.fork and expected_pid is None:
                expected_pid = session_pid
        assert peers[0] == peers[1] == peers[4] == peers[5], peers
        if proc:
            tail = proc.communicate(timeout=6)[0]; print(tail, end='', flush=True)
            assert proc.returncode == 0, proc.returncode
        print('PASS all: actual IPv6 TCP/UDP, IPv4 coexistence, token stability, ports, sendmsg/recvmsg, truncation, fd reuse', flush=True)
    finally:
        signal.alarm(0)
        if proc and proc.poll() is None:
            proc.terminate(); proc.wait(timeout=5)

if __name__ == '__main__': main()
