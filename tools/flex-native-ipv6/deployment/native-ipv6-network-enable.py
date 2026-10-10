# Copyright (c) 2026 J.J. Boyd. Original host job, authored with Codex.
# SPDX-License-Identifier: MIT
"""Original owner-authorized host Ethernet IPv6 override; JJ/Codex."""
if not __debug__:
    raise RuntimeError('Python assertions must remain enabled for firmware-specific host jobs')
import pathlib,os,json,subprocess,time,hashlib
assert os.geteuid()==0
main=pathlib.Path('/etc/systemd/network/10-cpu-main-interface.network')
expected='[Match]\nName=eth0\nKernelCommandLine=ssdr_role=cpu\n\n[Network]\nDHCP=ipv4\nLinkLocalAddressing=ipv4\nAddress=172.30.1.1/24\n\n[Route]\nDestination=172.30.1.2/24\nGateway=172.30.1.1/24\n\n[DHCP]\nRouteMetric=100\nUseMTU=true\n\n[Neighbor]\nAddress=172.30.1.2\nLinkLayerAddress=00:1c:2d:ff:ff:f2'
assert main.read_text()==expected
assert pathlib.Path('/etc/sysctl.d/95-disable-ipv6.conf').read_text()=='net.ipv6.conf.all.disable_ipv6=1\nnet.ipv6.conf.default.disable_ipv6=1\n'
files={
 '/etc/sysctl.d/99-nereus-native-ipv6.conf':'# Owner-installed native IPv6 on Ethernet and loopback only.\nnet.ipv6.conf.lo.disable_ipv6=0\nnet.ipv6.conf.eth0.disable_ipv6=0\n',
 '/etc/systemd/network/10-cpu-main-interface.network.d/90-nereus-native-ipv6.conf':'# Owner-installed native Ethernet IPv6; retain existing IPv4 settings.\n[Network]\nLinkLocalAddressing=yes\nIPv6AcceptRA=yes\n'}
assert all(not pathlib.Path(p).exists() for p in files)
backup=pathlib.Path('/var/lib/smartsdr/nereus-native-ipv6-20261009')
backup.mkdir(mode=0o700,exist_ok=True)
for p in [main,pathlib.Path('/etc/sysctl.d/95-disable-ipv6.conf')]:
 with (backup/p.name).open('xb') as f:f.write(p.read_bytes());f.flush();os.fsync(f.fileno())
for p,body in files.items():
 path=pathlib.Path(p);path.parent.mkdir(mode=0o755,exist_ok=True)
 with path.open('x') as f:f.write(body);f.flush();os.fsync(f.fileno())
 os.chmod(path,0o644)
os.sync()
for interface in ('lo','eth0'):
 pathlib.Path(f'/proc/sys/net/ipv6/conf/{interface}/disable_ipv6').write_text('0\n')
commands=[]
for cmd in [['networkctl','reload'],['networkctl','reconfigure','eth0']]:
 q=subprocess.run(cmd,capture_output=True,text=True,timeout=10);commands.append({'cmd':cmd,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr});assert q.returncode==0
for _ in range(20):
 q=subprocess.run(['ip','-6','route','show','default'],capture_output=True,text=True,timeout=3)
 if q.stdout.strip():break
 time.sleep(1)
r={'installed_files':files,'original_network_sha256':hashlib.sha256(main.read_bytes()).hexdigest(),'commands':commands,'interface_ipv6_flags':{}}
for p in pathlib.Path('/proc/sys/net/ipv6/conf').iterdir():r['interface_ipv6_flags'][p.name]=(p/'disable_ipv6').read_text().strip()
for cmd in [['ip','-6','addr','show'],['ip','-6','route','show'],['networkctl','status','eth0','--no-pager'],['ip','-4','addr','show','eth0']]:
 q=subprocess.run(cmd,capture_output=True,text=True,timeout=10);r[' '.join(cmd)]={'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
print(json.dumps(r))
