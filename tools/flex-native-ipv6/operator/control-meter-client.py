#!/usr/bin/env python3
"""Original receive-only control/meter exercise from the actual LAN client."""
# Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT
import argparse, collections, ipaddress, json, pathlib, select, socket, sys, time

def main(argv=None):

    parser=argparse.ArgumentParser(description='Receive-only control and meter probe')
    parser.add_argument('--host', required=True, help='Explicit IPv4 or IPv6 radio address')
    args=parser.parse_args(argv)
    server=args.host
    try: ipaddress.ip_address(server)
    except ValueError: parser.error('--host must be an IPv4 or IPv6 address')
    family=socket.AF_INET6 if ipaddress.ip_address(server).version==6 else socket.AF_INET
    report={'radio':server,'family':int(family),'commands':[],'rf_commands_sent':False}

    deadline=time.monotonic()+18
    while time.monotonic()<deadline:
        try:
            with socket.create_connection((report['radio'],4992),timeout=2) as tcp, socket.socket(family,socket.SOCK_DGRAM) as udp:
                tcp.setblocking(False);udp.bind(('::' if family==socket.AF_INET6 else '0.0.0.0',0));udp.setblocking(False)
                report['tcp_client_endpoint']=tcp.getsockname();report['udp_client_port']=udp.getsockname()[1]
                commands=['client program NereusSocketTraceExternal','client udpport '+str(udp.getsockname()[1]),'sub meter all','sub radio all']
                report['commands']=commands
                for i,text in enumerate(commands,1):tcp.sendall(f'C{i}|{text}\n'.encode())
                buffer=b'';codes={};tcpbytes=0;udpcount=0;udpbytes=0;sources=set();classes=collections.Counter()
                end=time.monotonic()+5
                while time.monotonic()<end:
                    ready,_,_=select.select([tcp,udp],[],[],.2)
                    if tcp in ready:
                        data=tcp.recv(65536)
                        if not data:raise ConnectionResetError('service restarted during trace client')
                        tcpbytes+=len(data);buffer+=data
                        while b'\n' in buffer:
                            line,buffer=buffer.split(b'\n',1)
                            if line.startswith(b'R'):
                                fields=line[1:].split(b'|',2)
                                if len(fields)>=2:codes[fields[0].decode(errors='replace')]=fields[1].decode(errors='replace')
                    if udp in ready:
                        data,source=udp.recvfrom(65536);udpcount+=1;udpbytes+=len(data);sources.add(source)
                        # Public Aether PanadapterStream.cpp:75-80: 28-byte Flex
                        # VITA header, word3 low16 packet class (meter 0x8002).
                        if len(data)>=28 and (data[0]>>4)==3:
                            classes[hex(int.from_bytes(data[14:16],'big'))]+=1
                report.update(response_codes=codes,tcp_bytes=tcpbytes,udp_packets=udpcount,udp_bytes=udpbytes,udp_sources=sorted(sources),vita_packet_classes=dict(classes))
                # Official TCPIP-client API: 10000002 means an unknown program name,
                # an informational response; UDP registration/subscriptions must succeed.
                assert codes.get('1') in ('0','10000002'),codes
                assert all(codes.get(str(i))=='0' for i in range(2,5)),codes
                assert classes['0x8002']>0,'no actual meter-class VITA UDP packets received'
                assert all(ipaddress.ip_address(source[0])==ipaddress.ip_address(server) and source[1]==4993 for source in sources),sources
                break
        except (OSError,AssertionError) as error:
            report.setdefault('retries',[]).append(str(error));time.sleep(.5)
    else:report['failed']=True
    print(json.dumps(report,indent=2))
    return 1 if report.get('failed') else 0

if __name__ == "__main__":
    raise SystemExit(main())
