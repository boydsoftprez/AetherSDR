#!/usr/bin/env python3
# Copyright (c) 2026 J.J. Boyd. Original clean-room tests, authored with Codex.
# SPDX-License-Identifier: MIT
import concurrent.futures
import fcntl
import ipaddress
import os
import pathlib
import signal
import socket
import struct
import subprocess

ROOT=pathlib.Path(__file__).resolve().parent

def fields(line):
    return dict(item.split('=',1) for item in line.strip().split()[1:])

def loopback_alias():
    # Public Linux SIOCSIFADDR/SIOCSIFNETMASK; isolated Docker NET_ADMIN only.
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as control:
        name=b'lo:shim'
        for command,address in [(0x8916,'172.30.1.2'),(0x891c,'255.255.255.255')]:
            request=struct.pack('16sH2s4s8s',name,socket.AF_INET,b'\0'*2,socket.inet_aton(address),b'\0'*8)
            fcntl.ioctl(control.fileno(),command,request)

def external(host,expected_pid):
    family=socket.AF_INET6 if ':' in host else socket.AF_INET
    with socket.socket(family,socket.SOCK_STREAM) as tcp,socket.socket(family,socket.SOCK_DGRAM) as udp:
        tcp.settimeout(5); udp.settimeout(5); udp.bind((host,0)); tcp.connect((host,4992))
        with tcp.makefile('rb') as stream:
            info=fields(stream.readline().decode()); assert int(info['pid'])==expected_pid,info
            assert int(info['port'])==tcp.getsockname()[1],info
            if family==socket.AF_INET6:
                assert ipaddress.ip_address(info['peer']) in ipaddress.ip_network('198.18.0.0/15'),info
            else: assert info['peer']==tcp.getsockname()[0],info
            tcp.sendall(f'PORT {udp.getsockname()[1]}\n'.encode())
            sources=set()
            for _ in range(2):
                data,source=udp.recvfrom(512); assert fields(data.decode())['peer']==info['peer'],data
                assert source[0]==host and source[1] in (4991,4993),source; sources.add(source[1])
            assert sources=={4991,4993},sources
            for i in range(100):
                for port in (4991,4993):
                    payload=f'E{udp.getsockname()[1]}:{port}:{i}'.encode(); udp.sendto(payload,(host,port))
                    reply,source=udp.recvfrom(512); assert reply==payload and source[:2]==(host,port),(reply,source)
            assert stream.readline()==b'DONE\n'
            return f'PASS simultaneous {host} TCP token={info["peer"]} UDP4991+4993 200 packets'

def internal(port):
    with socket.socket(socket.AF_INET,socket.SOCK_DGRAM) as udp:
        udp.settimeout(5); udp.bind(('172.30.1.2',port))
        for i in range(100):
            for target in (4991,4993):
                payload=f'I{port}:{target}:{i}'.encode(); udp.sendto(payload,('127.0.0.1',target))
                reply,source=udp.recvfrom(512); assert reply==payload and source==('172.30.1.2',target),(reply,source)
        return f'PASS simultaneous internal 172.30.1.2:{port} UDP4991+4993 200 packets'

def main():
    signal.signal(signal.SIGALRM,lambda *_: (_ for _ in ()).throw(TimeoutError('45-second total deadline')))
    signal.alarm(45); loopback_alias()
    env=dict(os.environ,LD_PRELOAD=os.environ.get('NEREUS_TEST_LIBRARY',str(ROOT/'libnereus_ipv6.so')),NEREUS_IPV6_TCP_PORT='4992',NEREUS_IPV6_UDP_PORTS='4991,4993')
    proc=subprocess.Popen([str(ROOT/'mixed_fixture')],env=env,stdout=subprocess.PIPE,text=True)
    try:
        line=proc.stdout.readline().strip(); print(line,flush=True); assert line.startswith('PASS IPv4 internal'),line
        line=proc.stdout.readline().strip(); print(line,flush=True); assert line.startswith('READY '),line
        pid=int(fields(line)['pid'])
        with concurrent.futures.ThreadPoolExecutor(max_workers=7) as workers:
            futures=[workers.submit(external,host,pid) for host in ('::1','::1','127.0.0.1','127.0.0.1')]
            futures += [workers.submit(internal,port) for port in (50000,50002,50003)]
            for future in concurrent.futures.as_completed(futures,timeout=20): print(future.result(),flush=True)
        tail=proc.communicate(timeout=5)[0]; print(tail,end='',flush=True); assert proc.returncode==0,proc.returncode
    finally:
        if proc.poll() is None: proc.terminate(); proc.wait(timeout=5)
        signal.alarm(0)

if __name__=='__main__': main()
