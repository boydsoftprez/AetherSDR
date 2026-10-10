# Copyright (c) 2026 J.J. Boyd. Original host job, authored with Codex.
# SPDX-License-Identifier: MIT
"""Owner-authorized, bounded native IPv6 boot setting edit; original JJ/Codex."""
if not __debug__:
    raise RuntimeError('Python assertions must remain enabled for firmware-specific host jobs')
import hashlib,json,os,pathlib,subprocess
path=pathlib.Path('/boot/cmdline.txt')
backup=pathlib.Path('/boot/cmdline.nereus-pre-ipv6-20261009.txt')
expected=b'console=serial0,115200 elevator=deadline fsck.repair=yes spidev.bufsiz=65536 rootwait ipv6.disable=1 ssdr_role=cpu'
replacement=expected.replace(b'ipv6.disable=1',b'ipv6.disable=0')
assert os.geteuid()==0
assert path.read_bytes()==expected, 'boot file changed since inspection'
assert expected.count(b'ipv6.disable=1')==1
assert not backup.exists(), 'backup already exists; inspect before retry'
mounts=pathlib.Path('/proc/mounts').read_text().splitlines()
boot=[line.split() for line in mounts if line.split()[1]=='/boot']
assert len(boot)==1 and boot[0][0]=='/dev/mmcblk0p1' and 'ro' in boot[0][3].split(',')
temporary=pathlib.Path('/boot/cmdline.nereus-ipv6.tmp')
assert not temporary.exists()
changed=False
subprocess.run(['/usr/bin/mount','-o','remount,rw','/boot'],check=True)
try:
    with backup.open('xb') as file:
        file.write(expected);file.flush();os.fsync(file.fileno())
    os.sync()
    assert backup.read_bytes()==expected
    with temporary.open('xb') as file:
        file.write(replacement);file.flush();os.fsync(file.fileno())
    os.chmod(temporary,0o644)
    os.replace(temporary,path)
    changed=True
    os.sync()
    assert path.read_bytes()==replacement
except BaseException:
    if changed:
        with temporary.open('wb') as file:
            file.write(expected);file.flush();os.fsync(file.fileno())
        os.replace(temporary,path);os.sync()
    raise
finally:
    if temporary.exists():temporary.unlink()
    subprocess.run(['/usr/bin/mount','-o','remount,ro','/boot'],check=True)
assert path.read_bytes()==replacement
assert backup.read_bytes()==expected
print(json.dumps({'changed':True,'before':expected.decode(),'after':replacement.decode(),'backup':str(backup),'before_sha256':hashlib.sha256(expected).hexdigest(),'after_sha256':hashlib.sha256(replacement).hexdigest(),'boot_mount':[line for line in pathlib.Path('/proc/mounts').read_text().splitlines() if line.split()[1]=='/boot']}))
