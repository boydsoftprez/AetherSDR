#!/usr/bin/env python3
# Copyright (c) 2026 J.J. Boyd. Original clean-room test harness, authored with Codex.
# SPDX-License-Identifier: MIT
import os
import pathlib
import subprocess
import sys

ROOT=pathlib.Path(__file__).resolve().parent
BASE={k:v for k,v in os.environ.items() if k not in ('LD_PRELOAD','NEREUS_IPV6_TCP_PORT','NEREUS_IPV6_UDP_PORT','NEREUS_IPV6_UDP_PORTS')}
LIB=str(ROOT/'libnereus_ipv6.so')

def run(command,**settings):
    print('RUN '+' '.join(command)+' '+repr(settings),flush=True)
    subprocess.run(command,cwd=ROOT,env=dict(BASE,**settings),check=True,timeout=55)

selected=dict(LD_PRELOAD=LIB,NEREUS_IPV6_TCP_PORT='4992',NEREUS_IPV6_UDP_PORTS='4991,4993')
for case in ('nodelay','rcvbuf','ports'): run(['./extension_checks',case],**selected)
for bad in ('','4991,',',4991','4991,0','4991,65536','4991,bad','4991, 4993','999999999999999999999','4991,'+','.join(['4993']*32)):
    run(['./extension_checks','invalid'],LD_PRELOAD=LIB,NEREUS_IPV6_UDP_PORTS=bad,NEREUS_IPV6_UDP_PORT='46093')
run(['./extension_checks','ports'],LD_PRELOAD=LIB,NEREUS_IPV6_UDP_PORTS='4991,4993,4991',NEREUS_IPV6_UDP_PORT='46093')
run(['./extension_checks','ports'],LD_PRELOAD=LIB,NEREUS_IPV6_UDP_PORTS='4991,'+','.join(['4993']*31))
run(['./extension_checks','legacy'],LD_PRELOAD=LIB,NEREUS_IPV6_UDP_PORT='46093')
run(['./extension_checks','legacy'],LD_PRELOAD=LIB,NEREUS_IPV6_UDP_PORTS='46093',NEREUS_IPV6_UDP_PORT='4991')
run(['./extension_checks','invalid'],LD_PRELOAD=LIB)
run(['./abi_checks'],LD_PRELOAD=LIB,NEREUS_IPV6_TCP_PORT='46092',NEREUS_IPV6_UDP_PORT='46093')
run([sys.executable,'test_client.py'])
run([sys.executable,'test_client.py','--fork'])
run([sys.executable,'mixed_client.py'])
print('PASS full shim suite',flush=True)
