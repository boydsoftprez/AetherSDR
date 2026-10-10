"""Original bounded native SmartSDR IPv6 acceptance and audited recovery job.
Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
Public socket/process metadata only. No capture, proxy, client/RF commands.
Executed by native-run.py with WORKDIR; imported by local fake tests safely.
"""
import hashlib, json, os, pathlib, socket, subprocess, time, uuid

SHA='75c4cf13d407c037b430dd5d8639c6d0bb12a8598fba0c74e11afef7c16cd355'

def valid_ack(payload):
    if len(payload)>512: return False
    try: value=json.loads(payload)
    except (ValueError,UnicodeError): return False
    return isinstance(value,dict) and set(value)=={'accept'} and value['accept'] is True

class AcceptanceJob:
    ACK_TIMEOUT=180
    def __init__(self,work,root=pathlib.Path('/'),adminport=46861):
        self.root=pathlib.Path(root); self.work=pathlib.Path(work); self.adminport=adminport
        self.tag=uuid.uuid4().hex[:12]; self.unit='nereus-smartsdr-accept-rollback-'+self.tag
        self.lib=self.root/'usr/local/lib/nereus-smartsdr-ipv6/libnereus_ipv6.so'
        self.dropdir=self.root/'run/systemd/system/smartsdr.service.d'
        self.dropin=self.dropdir/'90-nereus-ipv6.conf'
        self.rollback=self.root/('run/'+self.unit+'.py'); self.lock=pathlib.Path(str(self.rollback)+'.lock')
        self.marker=pathlib.Path(str(self.rollback)+'.started')
        self.listener=None; self.timer_attempted=False; self.timer_armed=False
        self.own_lib=False; self.own_dropin=False; self.own_rollback=False; self.pid=0
        self.report={'mode':'native SmartSDR lib-only current-boot IPv6 acceptance','retained':False,'original_service_restored':False,'rf_commands_sent':False,
                     'stable_library':str(self.lib),'stable_sha256':SHA,'runtime_dropin':str(self.dropin),
                     'rollback_unit':self.unit,'rollback_seconds':300,'ack_timeout_seconds':self.ACK_TIMEOUT,
                     'admin_address':['127.0.0.1',adminport],'selected_socket_snapshots':[]}
    def sleep(self,duration): time.sleep(duration)
    def command(self,args,timeout=20,check=True):
        q=subprocess.run(args,capture_output=True,text=True,timeout=timeout)
        r={'args':args,'rc':q.returncode,'stdout':q.stdout,'stderr':q.stderr}
        if check and q.returncode: raise RuntimeError(json.dumps(r))
        return r
    def boot_id(self): return (self.root/'proc/sys/kernel/random/boot_id').read_text().strip()
    def service(self):
        r=self.command(['systemctl','show','smartsdr.service','-p','MainPID','-p','ActiveState','-p','SubState','-p','NRestarts','-p','DropInPaths'])
        return dict(line.split('=',1) for line in r['stdout'].splitlines() if '=' in line)
    def maps(self,pid): return (self.root/f'proc/{pid}/maps').read_text()
    def library_in_use(self):
        for process in (self.root/'proc').iterdir():
            if not process.name.isdigit(): continue
            try:
                if str(self.lib) in (process/'maps').read_text(): return True
            except FileNotFoundError: continue
        return False
    def socket_snapshot(self,pid):
        owned=set()
        for fd in (self.root/f'proc/{pid}/fd').iterdir():
            try: target=os.readlink(fd)
            except FileNotFoundError: continue
            if target.startswith('socket:['): owned.add(target[8:-1])
        tables={}
        for table in ('tcp','tcp6','udp','udp6'):
            tables[table]=[line for line in (self.root/('proc/net/'+table)).read_text().splitlines()[1:]
                           if len(line.split())>9 and line.split()[9] in owned]
        return {'pid':pid,'tables':tables,'ss':self.command(['ss','-ntup'],check=False)['stdout']}
    def validate_snapshot(self,snapshot):
        tables=snapshot['tables']
        def present(table,port,listen=False):
            return any(line.split()[1].endswith(':'+port) and (not listen or line.split()[3]=='0A') for line in tables[table])
        if not present('tcp6','1380',True) or not all(present('udp6',p) for p in ('137F','1381')):
            raise RuntimeError('selected native IPv6 sockets absent')
        if not all(present('udp',p) for p in ('1380','7530','7531')) or not present('tcp','1382',True):
            raise RuntimeError('untouched IPv4 discovery/internal/SmartLink sockets absent')
        if present('tcp','1380',True) or any(present('udp',p) for p in ('137F','1381')):
            raise RuntimeError('selected sockets still IPv4')
    def dropin_content(self):
        return '[Service]\nEnvironment="LD_PRELOAD='+str(self.lib)+'"\nEnvironment="NEREUS_IPV6_TCP_PORT=4992"\nEnvironment="NEREUS_IPV6_UDP_PORTS=4991,4993"\n'
    def verify_active(self):
        current=self.service()
        if current.get('MainPID')!=str(self.pid) or current.get('ActiveState')!='active' or current.get('SubState')!='running':
            raise RuntimeError('SmartSDR changed PID/state during acceptance')
        if self.boot_id()!=self.report['boot_id_before']: raise RuntimeError('boot changed during acceptance')
        maps=self.maps(self.pid)
        if str(self.lib) not in maps or 'libnereus_socket_trace.so' in maps: raise RuntimeError('lib-only process maps check failed')
        if self.dropin.read_text()!=self.dropin_content() or str(self.dropin) not in current.get('DropInPaths',''):
            raise RuntimeError('runtime drop-in absent or changed')
        if hashlib.sha256(self.lib.read_bytes()).hexdigest()!=SHA: raise RuntimeError('stable library hash changed')
        return current
    def rollback_source(self):
        # Same unconditional reload/guard/restart and flock serialization as the
        # reviewed streams job. The started marker makes completed timer races visible.
        return '''import os,pathlib,subprocess,fcntl,hashlib
LIB=LIB_VALUE
DROPIN=DROPIN_VALUE
LOCK=LOCK_VALUE
MARKER=MARKER_VALUE
SHA=SHA_VALUE
pathlib.Path(MARKER).touch(exist_ok=True)
lock=os.open(LOCK,os.O_CREAT|os.O_RDWR,0o600)
try:
 fcntl.flock(lock,fcntl.LOCK_EX)
 pathlib.Path(DROPIN).unlink(missing_ok=True)
 # Retry both operations even when an earlier attempt already unlinked the file.
 subprocess.run(['systemctl','daemon-reload'],check=True,timeout=20)
 subprocess.run(['systemctl','start','inhibit-power-pic.service'],check=True,timeout=15)
 subprocess.run(['systemctl','restart','smartsdr.service'],check=True,timeout=45)
 q=subprocess.run(['systemctl','show','smartsdr.service','-p','MainPID','-p','ActiveState','-p','SubState'],capture_output=True,text=True,check=True,timeout=20)
 state=dict(line.split('=',1) for line in q.stdout.splitlines() if '=' in line)
 if int(state.get('MainPID','0'))<=1 or state.get('ActiveState')!='active' or state.get('SubState')!='running':
  raise RuntimeError('original service not active after rollback')
 # Never unlink a library still mapped by any native process.
 for process in pathlib.Path('/proc').iterdir():
  if not process.name.isdigit():continue
  try:
   if LIB in (process/'maps').read_text():raise RuntimeError('stable shim remains mapped after rollback')
  except FileNotFoundError:continue
 library=pathlib.Path(LIB)
 if library.exists():
  if hashlib.sha256(library.read_bytes()).hexdigest()!=SHA:raise RuntimeError('stable library ownership/hash changed')
  library.unlink()
finally:os.close(lock)
'''.replace('LIB_VALUE',repr(str(self.lib))).replace('DROPIN_VALUE',repr(str(self.dropin))).replace('LOCK_VALUE',repr(str(self.lock))).replace('MARKER_VALUE',repr(str(self.marker))).replace('SHA_VALUE',repr(SHA))
    def wait_ack(self):
        deadline=time.monotonic()+self.ACK_TIMEOUT; next_check=0
        while time.monotonic()<deadline:
            if time.monotonic()>=next_check:
                self.verify_active(); next_check=time.monotonic()+2
            self.listener.settimeout(min(.5,max(.001,deadline-time.monotonic())))
            try: conn,_=self.listener.accept()
            except socket.timeout: continue
            with conn:
                conn.settimeout(min(2,max(.001,deadline-time.monotonic())))
                payload=b''
                try:
                    while len(payload)<=512 and b'\n' not in payload:
                        remaining=deadline-time.monotonic()
                        if remaining<=0: raise TimeoutError('administrative acceptance timeout')
                        conn.settimeout(min(2,remaining))
                        part=conn.recv(min(513-len(payload),513))
                        if not part: break
                        payload+=part
                    remaining=deadline-time.monotonic()
                    if remaining<=0: raise TimeoutError('administrative acceptance timeout')
                    conn.settimeout(min(2,remaining))
                    if valid_ack(payload):
                        conn.sendall(b'{"acknowledged":true}\n'); return True
                    conn.sendall(b'{"acknowledged":false}\n')
                except (OSError,TimeoutError): pass
                self.report['rejected_ack_count']=self.report.get('rejected_ack_count',0)+1
        raise TimeoutError('administrative acceptance timeout')
    def settle_rollback(self):
        # Stop future timer activation only; never kill an already running service.
        result={'settled':False,'rollback_seen':self.marker.exists()}
        stopped=self.command(['systemctl','stop',self.unit+'.timer'],check=False)
        result['stop_timer']=stopped
        if stopped['rc']!=0: return result
        self.timer_armed=False; deadline=time.monotonic()+100
        while time.monotonic()<deadline:
            state=self.command(['systemctl','show',self.unit+'.service','-p','ActiveState'],timeout=5,check=False)
            jobs=self.command(['systemctl','list-jobs','--no-pager','--no-legend'],timeout=5,check=False)
            busy=any('ActiveState='+name in state['stdout'] for name in ('active','activating','deactivating','reloading'))
            queued=self.unit+'.service' in jobs['stdout']
            result['rollback_seen']=result['rollback_seen'] or busy or queued or self.marker.exists()
            if state['rc']==0 and jobs['rc']==0 and not busy and not queued:
                result['settled']=True; return result
            self.sleep(.5)
        return result
    def remove_rollback_files(self):
        # Preserve the executable recovery path until all earlier cleanup succeeds.
        for path in (self.lock,self.marker,self.rollback): path.unlink(missing_ok=True)
    def timer_state(self):
        response=self.command(['systemctl','show',self.unit+'.timer','-p','ActiveState','-p','SubState','-p','NextElapseUSecMonotonic'],timeout=5,check=False)
        properties=dict(line.split('=',1) for line in response['stdout'].splitlines() if '=' in line)
        # systemctl can format usec properties as time spans. Do not guess that
        # representation. Active/waiting is the native pending-timer state;
        # additionally validate next-elapse numerically when raw digits are shown.
        next_elapse=properties.get('NextElapseUSecMonotonic','')
        numeric_future=int(next_elapse)>time.monotonic()*1000000 if next_elapse.isdigit() else None
        verified=response['rc']==0 and properties.get('ActiveState')=='active' and properties.get('SubState')=='waiting' and numeric_future is not False
        return {'query':response,'properties':properties,'next_elapse_numeric_future':numeric_future,'verified_future':verified}
    def arm_retry(self):
        # A successful earlier arm can already be consumed. Restart only the
        # timer unconditionally; never stop/restart its rollback service.
        self.timer_armed=False
        result={'verified_future':False}
        self.report.setdefault('rearm_rollback',[]).append(result)
        try:
            result['restart_timer']=self.command(['systemctl','restart',self.unit+'.timer'],check=False)
            result['state']=self.timer_state()
            result['verified_future']=result['restart_timer']['rc']==0 and result['state']['verified_future']
            self.timer_armed=result['verified_future']
        except BaseException as error:
            result['error']=repr(error)
            self.report.setdefault('rearm_errors',[]).append(repr(error))
    def restore(self):
        # Each diagnostic is isolated so an incidental exception cannot skip recovery.
        restored=False
        try:
            # The previous one-shot may already have fired or been canceled.
            # Give inline restoration a fresh independently verified backup.
            self.arm_retry()
            inline=self.command(['/usr/bin/python3',str(self.rollback)],timeout=180,check=False)
            self.report['restore']=inline
            # A failed inline attempt leaves the independent retry armed.
            if inline['rc']!=0: raise RuntimeError('inline recovery failed; preserve independent retry')
            settled=self.settle_rollback(); self.report['restore_settle']=settled
            after=self.service(); self.report['after']=after
            same_boot=self.boot_id()==self.report['boot_id_before']; self.report['same_boot']=same_boot
            restored=settled['settled'] and not self.dropin.exists() and not self.library_in_use() and same_boot and int(after.get('MainPID','0'))>1 and after.get('ActiveState')=='active' and after.get('SubState')=='running'
            if restored:
                if self.own_lib and self.lib.exists():
                    if hashlib.sha256(self.lib.read_bytes()).hexdigest()!=SHA: raise RuntimeError('refusing cleanup of changed stable library')
                    self.lib.unlink()
                self.remove_rollback_files()
        except BaseException as error: self.report['restore_error']=repr(error)
        if not restored: self.arm_retry()
        self.report['original_service_restored']=restored
        return restored
    def run(self):
        try:
            if self.lib.exists() or self.lib.is_symlink() or self.dropin.exists() or self.dropin.is_symlink():
                raise RuntimeError('stable library or runtime drop-in already exists; refusing overwrite')
            data=(self.work/'libnereus_ipv6.so').read_bytes()
            if hashlib.sha256(data).hexdigest()!=SHA: raise RuntimeError('input library SHA mismatch')
            self.report['boot_id_before']=self.boot_id(); self.report['before']=self.service()
            before=self.report['before']
            if before.get('ActiveState')!='active' or before.get('SubState')!='running' or int(before.get('MainPID','0'))<=1:
                raise RuntimeError('original SmartSDR service not running')
            self.listener=socket.socket(socket.AF_INET,socket.SOCK_STREAM)
            self.listener.bind(('127.0.0.1',self.adminport))
            self.report['admin_address']=list(self.listener.getsockname())
            self.lib.parent.mkdir(parents=True,mode=0o755,exist_ok=True)
            with self.lib.open('xb') as file: self.own_lib=True; file.write(data)
            os.chmod(self.lib,0o755)
            source=self.rollback_source(); compile(source,str(self.rollback),'exec')
            self.rollback.parent.mkdir(parents=True,exist_ok=True)
            with self.rollback.open('x') as file: self.own_rollback=True; file.write(source)
            os.chmod(self.rollback,0o600)
            self.timer_attempted=True
            self.report['arm_rollback']=self.command(['systemd-run','--unit='+self.unit,'--on-active=300s','--timer-property=AccuracySec=1s','/usr/bin/python3',str(self.rollback)])
            timer=self.timer_state(); self.report['initial_timer_state']=timer
            self.timer_armed=timer['verified_future']
            if not self.timer_armed: raise RuntimeError('independent rollback timer has no verified future activation')
            self.dropdir.mkdir(parents=True,mode=0o755,exist_ok=True)
            with self.dropin.open('x') as file: self.own_dropin=True; file.write(self.dropin_content())
            os.chmod(self.dropin,0o644)
            self.command(['systemctl','daemon-reload'])
            self.report['power_controller_guard']=self.command(['systemctl','start','inhibit-power-pic.service'],timeout=15)
            self.report['activation_restart']=self.command(['systemctl','restart','smartsdr.service'],timeout=35)
            current=self.service(); self.pid=int(current.get('MainPID','0'))
            if self.pid<=1: raise RuntimeError('activated SmartSDR has no main PID')
            self.report['during']=self.verify_active()
            snapshot=self.socket_snapshot(self.pid); self.validate_snapshot(snapshot); self.report['selected_socket_snapshots'].append(snapshot)
            self.listener.listen(1)  # Administrative ACK is available only after activation proof.
            print(json.dumps({'acceptance_ready':True,'pid':self.pid,'boot_id':self.report['boot_id_before'],'admin_address':self.report['admin_address'],'stable_sha256':SHA}),flush=True)
            self.wait_ack(); self.report['acknowledged']=True
            settled=self.settle_rollback(); self.report['acceptance_settle']=settled
            if not settled['settled'] or settled['rollback_seen']: raise RuntimeError('rollback started/queued or did not settle; refusing retention')
            self.report['after']=self.verify_active()
            snapshot=self.socket_snapshot(self.pid); self.validate_snapshot(snapshot); self.report['selected_socket_snapshots'].append(snapshot)
            # Recheck process after snapshot collection, before destroying recovery files.
            self.verify_active()
            if self.marker.exists(): raise RuntimeError('rollback started during final acceptance checks')
            self.remove_rollback_files()
            self.report['same_boot']=True; self.report['retained']=True
            self.report['revert_commands']=[['rm','-f',str(self.dropin)],['systemctl','daemon-reload'],['systemctl','start','inhibit-power-pic.service'],['systemctl','restart','smartsdr.service']]
            self.report['revert_library_cleanup']='After successful original-service restart, verify no /proc/*/maps contains '+str(self.lib)+' before removing it.'
        except BaseException as error: self.report['error']=repr(error)
        finally:
            if self.listener is not None:
                try: self.listener.close()
                except OSError as error: self.report['admin_close_error']=repr(error)
            if not self.report['retained']:
                if self.own_rollback and self.timer_attempted: self.restore()
                elif self.own_lib:
                    try:
                        if not self.library_in_use(): self.lib.unlink(missing_ok=True)
                        if self.own_rollback: self.remove_rollback_files()
                    except BaseException as error: self.report['preactivation_cleanup_error']=repr(error)
            if self.timer_attempted:
                try:
                    final_timer=self.timer_state(); self.report['final_timer_state']=final_timer
                    self.timer_armed=final_timer['verified_future']
                except BaseException as error:
                    self.report['final_timer_state_error']=repr(error); self.timer_armed=False
            self.report['rollback_timer_still_armed']=self.timer_armed
        return self.report

if 'WORKDIR' in globals():
    result=AcceptanceJob(WORKDIR).run()
    print(json.dumps(result),flush=True)
    if not result['retained']: raise RuntimeError('acceptance not retained; inspect restoration evidence and independent rollback')
