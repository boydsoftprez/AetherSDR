# Copyright (c) 2026 J.J. Boyd. Original host job, authored with Codex.
# SPDX-License-Identifier: MIT
"""Prepared restoration script; NOT RUN. Requires owner-authorized host root."""
if not __debug__:
    raise RuntimeError('Python assertions must remain enabled for firmware-specific host jobs')
import pathlib,os,subprocess,json
assert os.geteuid()==0
original=b'console=serial0,115200 elevator=deadline fsck.repair=yes spidev.bufsiz=65536 rootwait ipv6.disable=1 ssdr_role=cpu'
current=original.replace(b'ipv6.disable=1',b'ipv6.disable=0')
boot=pathlib.Path('/boot/cmdline.txt');backup=pathlib.Path('/boot/cmdline.nereus-pre-ipv6-20261009.txt')
assert boot.read_bytes()==current and backup.read_bytes()==original
files={
 '/etc/sysctl.d/99-nereus-native-ipv6.conf':'# Owner-installed native IPv6 on Ethernet and loopback only.\nnet.ipv6.conf.lo.disable_ipv6=0\nnet.ipv6.conf.eth0.disable_ipv6=0\n',
 '/etc/systemd/network/10-cpu-main-interface.network.d/90-nereus-native-ipv6.conf':'# Owner-installed native Ethernet IPv6; retain existing IPv4 settings.\n[Network]\nLinkLocalAddressing=yes\nIPv6AcceptRA=yes\n'}
assert all(pathlib.Path(p).read_text()==body for p,body in files.items())
temporary=pathlib.Path('/boot/cmdline.nereus-rollback.tmp');assert not temporary.exists()
subprocess.run(['/usr/bin/mount','-o','remount,rw','/boot'],check=True)
try:
 with temporary.open('xb') as f:f.write(original);f.flush();os.fsync(f.fileno())
 os.chmod(temporary,0o644);os.replace(temporary,boot);os.sync()
 assert boot.read_bytes()==original
finally:
 subprocess.run(['/usr/bin/mount','-o','remount,ro','/boot'],check=True)
for p in files:pathlib.Path(p).unlink()
os.sync()
print(json.dumps({'restored_boot':boot.read_text(),'removed_overrides':list(files),'reboot_required':True}))
