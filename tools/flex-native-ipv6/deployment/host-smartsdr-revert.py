"""Original future-use native SmartSDR acceptance revert job.
Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
No radio/client/RF commands or packet capture. Native executor supplies WORKDIR and EXPECTED_SHA256.
Publication adaptation: explicit reviewed artifact digest, not a historical binary pin.
"""
import hashlib, json, pathlib, re, subprocess

def validate_expected_sha256(value):
    if not isinstance(value,str) or re.fullmatch(r'[0-9a-f]{64}',value) is None:
        raise ValueError('expected-sha256 must be an explicit 64-character lowercase hex digest')
    return value

def native_command(args,timeout=20):
    q=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
    return {'args':args,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr}

def revert(root=pathlib.Path('/'),command=native_command,expected_sha256=None):
    expected_sha256=validate_expected_sha256(expected_sha256)
    root=pathlib.Path(root);lib=root/'usr/local/lib/nereus-smartsdr-ipv6/libnereus_ipv6.so'
    dropin=root/'run/systemd/system/smartsdr.service.d/90-nereus-ipv6.conf'
    expected='[Service]\nEnvironment="LD_PRELOAD='+str(lib)+'"\nEnvironment="NEREUS_IPV6_TCP_PORT=4992"\nEnvironment="NEREUS_IPV6_UDP_PORTS=4991,4993"\n'
    report={'original_service_restored':False,'library_removed':False,'rf_commands_sent':False,'stable_library':str(lib),'expected_sha256':expected_sha256,'attempts':[]}
    def checked(args,timeout=20):
        result=command(args,timeout=timeout)
        if result['rc']!=0:raise RuntimeError(json.dumps(result))
        return result
    def state():
        r=checked(['systemctl','show','smartsdr.service','-p','MainPID','-p','ActiveState','-p','SubState','-p','DropInPaths'])
        return dict(line.split('=',1) for line in r['stdout'].splitlines() if '=' in line)
    try:
        if lib.is_symlink() or dropin.is_symlink() or hashlib.sha256(lib.read_bytes()).hexdigest()!=expected_sha256:
            raise RuntimeError('stable library ownership/SHA check failed')
        # A prior failed reload may already have removed our drop-in. Always retry
        # reload/guard/restart rather than interpreting absence as restoration.
        if dropin.exists() and dropin.read_text()!=expected:raise RuntimeError('runtime drop-in differs from exact owned content')
        boot=(root/'proc/sys/kernel/random/boot_id').read_text().strip()
        before=state();report['before']=before
        if int(before.get('MainPID','0'))<=1 or before.get('ActiveState')!='active' or before.get('SubState')!='running':
            raise RuntimeError('native SmartSDR not active before revert')
        dropin.unlink(missing_ok=True)
        for attempt in range(2):
            outcome={'attempt':attempt+1};report['attempts'].append(outcome)
            try:
                outcome['reload']=checked(['systemctl','daemon-reload'])
                outcome['guard']=checked(['systemctl','start','inhibit-power-pic.service'],timeout=15)
                outcome['restart']=checked(['systemctl','restart','smartsdr.service'],timeout=45)
                after=state();report['after']=after
                if int(after.get('MainPID','0'))<=1 or after.get('ActiveState')!='active' or after.get('SubState')!='running':raise RuntimeError('original service not running')
                if str(dropin) in after.get('DropInPaths','') or dropin.exists():raise RuntimeError('runtime drop-in remains active')
                if (root/'proc/sys/kernel/random/boot_id').read_text().strip()!=boot:raise RuntimeError('boot changed during revert')
                for process in (root/'proc').iterdir():
                    if not process.name.isdigit():continue
                    try:
                        if str(lib) in (process/'maps').read_text():raise RuntimeError('stable library still mapped; refusing unlink')
                    except FileNotFoundError:continue
                report['same_boot']=True;report['original_service_restored']=True
                if hashlib.sha256(lib.read_bytes()).hexdigest()!=expected_sha256:raise RuntimeError('library changed before cleanup')
                lib.unlink();report['library_removed']=True
                break
            except BaseException as error:outcome['error']=repr(error)
        if not report['library_removed']:report['error']='revert incomplete; library retained, inspect guarded recovery commands and retry'
    except BaseException as error:report['error']=repr(error)
    return report

if 'WORKDIR' in globals():
    result=revert(expected_sha256=globals().get('EXPECTED_SHA256'));print(json.dumps(result),flush=True)
    if not result['original_service_restored'] or not result['library_removed']:raise RuntimeError('native revert not verified')
