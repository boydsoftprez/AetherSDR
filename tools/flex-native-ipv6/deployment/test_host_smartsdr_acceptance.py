"""Original local fake-system acceptance/recovery tests. No radio access.
Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
"""
import hashlib, importlib.util, json, os, pathlib, socket, subprocess, sys, tempfile, threading, unittest
from unittest import mock

ROOT=pathlib.Path(__file__).resolve().parent
SHIM=ROOT.parent/'shim'
EXPECTED_SHA256=hashlib.sha256((SHIM/'libnereus_ipv6.so').read_bytes()).hexdigest()
spec=importlib.util.spec_from_file_location('acceptance',ROOT/'host-smartsdr-acceptance.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)

class AcceptanceTests(unittest.TestCase):
    def test_ack_requires_exact_boolean_object(self):
        self.assertTrue(callable(getattr(module,'valid_ack',None)), 'missing bounded strict acceptance parser')
        self.assertTrue(module.valid_ack(b'{"accept":true}\n'))
        for payload in (b'{"accept":1}',b'{"accept":false}',b'{"accept":true,"other":0}',b'[]',b'x',b'{}',b'{"accept":true}\n{}',b' '*513):
            self.assertFalse(module.valid_ack(payload),payload)

    def make_job(self,root,ack=True,race=False,completed_race=False):
        self.assertTrue(hasattr(module,'AcceptanceJob'), 'missing native acceptance/recovery implementation')
        class FakeJob(module.AcceptanceJob):
            ACK_TIMEOUT=.12
            def __init__(self):
                super().__init__(SHIM,root=root,adminport=0,expected_sha256=EXPECTED_SHA256)
                self.calls=[]; self.pid=200; self.loaded=False; self.race=race; self.completed_race=completed_race; self.race_polls=0; self.timer_pending=False
            def boot_id(self): return 'test-boot'
            def service(self):
                return {'MainPID':str(self.pid),'ActiveState':'active','SubState':'running','DropInPaths':str(self.dropin) if self.dropin.exists() else ''}
            def maps(self,pid): return str(self.lib) if self.loaded else '/vendor/lib.so'
            def library_in_use(self): return self.loaded
            def socket_snapshot(self,pid):
                return {'pid':pid,'tables':{'tcp6':['0: 0000:1380 0000:0000 0A'],'udp6':['0: 0000:137F 0000:0000 07','0: 0000:1381 0000:0000 07'],'udp':['0: 0000:1380 0000:0000 07','0: 0000:7530 0000:0000 07','0: 0000:7531 0000:0000 07'],'tcp':['0: 0000:1382 0000:0000 0A']},'ss':'fake full socket snapshot'}
            def command(self,args,timeout=20,check=True):
                self.calls.append(args); output=''; rc=0
                if args[0]=='systemd-run' or args==['systemctl','restart',self.unit+'.timer']:
                    self.timer_pending=True
                elif args==['systemctl','stop',self.unit+'.timer']:
                    self.timer_pending=False
                elif args==['systemctl','restart','smartsdr.service']:
                    self.assert_guard(); self.pid+=1; self.loaded=self.dropin.exists()
                elif args[0]=='/usr/bin/python3':
                    self.marker.touch(); self.dropin.unlink(missing_ok=True); self.pid+=1; self.loaded=False
                elif args[:2]==['systemctl','show'] and args[2].endswith('.timer'):
                    output=('ActiveState=active\nSubState=waiting\nNextElapseUSecMonotonic='+str(int((module.time.monotonic()+300)*1000000))+'\n') if self.timer_pending else 'ActiveState=inactive\nSubState=dead\nNextElapseUSecMonotonic=0\n'
                elif args[:2]==['systemctl','show']:
                    if self.race and self.race_polls==0:
                        self.marker.touch(); self.dropin.unlink(missing_ok=True); self.pid+=1; self.loaded=False; self.race_polls+=1
                        output='ActiveState=inactive\n' if self.completed_race else 'ActiveState=active\n'
                    else: output='ActiveState=inactive\n'
                return {'args':args,'rc':rc,'stdout':output,'stderr':''}
            def assert_guard(self):
                assert self.calls[-2]==['systemctl','start','inhibit-power-pic.service'],self.calls
            def wait_ack(self):
                if ack:
                    address=self.listener.getsockname()
                    def sender():
                        with socket.create_connection(address,timeout=1) as client:
                            client.sendall(b'{"accept":true}\n'); client.settimeout(1); client.recv(512)
                    thread=threading.Thread(target=sender); thread.start()
                    result=super().wait_ack(); thread.join(timeout=2); return result
                return super().wait_ack()
            def sleep(self,duration): pass
        return FakeJob()

    def test_ack_retains_only_stable_library_and_runtime_dropin(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); report=job.run()
            self.assertTrue(report['retained'],report); self.assertTrue(job.lib.exists()); self.assertTrue(job.dropin.exists())
            self.assertEqual(report['stable_sha256'],EXPECTED_SHA256)
            self.assertNotIn('trace',job.dropin.read_text()); self.assertFalse(job.rollback.exists())
            self.assertEqual(len([c for c in job.calls if c[:2]==['systemctl','restart']]),1)
            self.assertFalse(any(c[:2]==['systemctl','stop'] and c[2].endswith('.service') for c in job.calls))

    def test_timeout_restores_and_unlinks_library_after_maps_clear(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory),ack=False); report=job.run()
            self.assertFalse(report['retained']); self.assertTrue(report['original_service_restored'],report)
            self.assertFalse(job.dropin.exists()); self.assertFalse(job.lib.exists()); self.assertFalse(job.rollback.exists())
            self.assertIn('timeout',report['error'].lower())

    def test_active_rollback_race_never_retains(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory),race=True); report=job.run()
            self.assertFalse(report['retained']); self.assertTrue(report['original_service_restored'],report)
            self.assertTrue(report['acceptance_settle']['rollback_seen']); self.assertFalse(job.lib.exists())

    def test_completed_rollback_marker_never_retains(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory),race=True,completed_race=True); report=job.run()
            self.assertFalse(report['retained']); self.assertTrue(report['original_service_restored'],report)
            self.assertTrue(report['acceptance_settle']['rollback_seen'])

    def test_post_cancel_restore_failure_rearms_independent_retry(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory),race=True,completed_race=True)
            original=job.command
            def failing_inline(args,timeout=20,check=True):
                if args[0]=='/usr/bin/python3':
                    job.calls.append(args); return {'args':args,'rc':1,'stdout':'','stderr':'injected reload failure'}
                return original(args,timeout=timeout,check=check)
            job.command=failing_inline
            report=job.run()
            self.assertFalse(report['retained']); self.assertTrue(report['rollback_timer_still_armed'],report)
            self.assertTrue(job.rollback.exists()); self.assertIn(['systemctl','restart',job.unit+'.timer'],job.calls)

    def test_consumed_timer_and_failed_inline_restarts_fresh_timer(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); original=job.command; pending=True
            def timer_fired():
                nonlocal pending
                pending=False; job.timer_pending=False; job.marker.touch(); job.dropin.unlink()
                raise RuntimeError('independent one-shot fired then reload failed')
            job.wait_ack=timer_fired
            def failed_inline(args,timeout=20,check=True):
                nonlocal pending
                if args==['systemctl','restart',job.unit+'.timer']: pending=True
                if args[0]=='/usr/bin/python3':
                    job.calls.append(args); return {'args':args,'rc':1,'stdout':'','stderr':'inline failed'}
                if args[:2]==['systemctl','show'] and args[2].endswith('.timer') and not pending:
                    job.calls.append(args); return {'args':args,'rc':0,'stdout':'ActiveState=active\nSubState=elapsed\nNextElapseUSecMonotonic=0\n','stderr':''}
                return original(args,timeout=timeout,check=check)
            job.command=failed_inline; report=job.run()
            self.assertFalse(report['retained']); self.assertFalse(report['original_service_restored'])
            self.assertTrue(report['rollback_timer_still_armed'],report)
            restart=['systemctl','restart',job.unit+'.timer']
            self.assertGreaterEqual(job.calls.count(restart),2,job.calls)
            self.assertLess(job.calls.index(restart),job.calls.index(['/usr/bin/python3',str(job.rollback)]))
            self.assertTrue(report['rearm_rollback'][-1]['verified_future'])
            self.assertFalse(any(c[:2]==['systemctl','stop'] and c[2].endswith('.service') for c in job.calls))

    def test_unverified_timer_is_never_reported_armed(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); job.timer_armed=True
            def command(args,timeout=20,check=True):
                job.calls.append(args)
                text='ActiveState=inactive\nSubState=dead\nNextElapseUSecMonotonic=0\n'
                return {'args':args,'rc':0,'stdout':text,'stderr':''}
            job.command=command; job.arm_retry()
            self.assertFalse(job.timer_armed); self.assertFalse(job.report['rearm_rollback'][-1]['verified_future'])

    def test_slow_drip_receive_respects_global_deadline(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); job.ACK_TIMEOUT=180; job.verify_active=lambda:None
            clock=[0.0]
            class Connection:
                def __enter__(self):return self
                def __exit__(self,*args):pass
                def settimeout(self,timeout):self.timeout=timeout
                def recv(self,size):clock[0]+=1;return b'x'
                def sendall(self,payload):pass
            connection=Connection()
            class Listener:
                def settimeout(self,timeout):pass
                def accept(self):return connection,('127.0.0.1',50000)
            job.listener=Listener()
            with mock.patch.object(module.time,'monotonic',lambda:clock[0]):
                with self.assertRaises(TimeoutError):module.AcceptanceJob.wait_ack(job)
            self.assertLessEqual(clock[0],180, 'slow drip exceeded the global ACK window')

    def test_cleanup_failure_preserves_recovery_script(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); job.rollback.parent.mkdir(parents=True)
            job.rollback.write_text('recovery'); job.lock.mkdir()
            with self.assertRaises(OSError): job.remove_rollback_files()
            self.assertTrue(job.rollback.exists(), 'cleanup must remove recovery script last')

    @unittest.skipUnless(pathlib.Path('/proc').is_dir(),'generated rollback execution requires Linux /proc')
    def test_generated_rollback_retries_after_reload_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); job.dropdir.mkdir(parents=True)
            job.dropin.write_text(job.dropin_content()); job.lib.parent.mkdir(parents=True)
            job.lib.write_bytes((SHIM/'libnereus_ipv6.so').read_bytes())
            job.rollback.write_text(job.rollback_source())
            bindir=pathlib.Path(directory)/'bin'; bindir.mkdir(); fake=bindir/'systemctl'
            fake.write_text('#!'+sys.executable+'\n'+"""import os,pathlib,sys
r=pathlib.Path(os.environ['NEREUS_ACCEPT_TEST_ROOT'])
with (r/'calls').open('a') as file:file.write(' '.join(sys.argv[1:])+'\\n')
if not (r/'failed').exists():
 (r/'failed').touch();sys.exit(1)
if sys.argv[1]=='show':print('MainPID=999\\nActiveState=active\\nSubState=running')
""")
            fake.chmod(0o700)
            env=dict(os.environ,PATH=str(bindir)+os.pathsep+os.environ['PATH'],NEREUS_ACCEPT_TEST_ROOT=directory)
            first=subprocess.run([sys.executable,str(job.rollback)],env=env,capture_output=True,text=True)
            self.assertNotEqual(first.returncode,0); self.assertFalse(job.dropin.exists()); self.assertTrue(job.lib.exists())
            second=subprocess.run([sys.executable,str(job.rollback)],env=env,capture_output=True,text=True)
            self.assertEqual(second.returncode,0,second.stderr); self.assertFalse(job.lib.exists())
            self.assertEqual((pathlib.Path(directory)/'calls').read_text().splitlines(),['daemon-reload','daemon-reload','start inhibit-power-pic.service','restart smartsdr.service','show smartsdr.service -p MainPID -p ActiveState -p SubState'])

    def test_existing_stable_file_is_not_overwritten_or_restarted(self):
        with tempfile.TemporaryDirectory() as directory:
            job=self.make_job(pathlib.Path(directory)); job.lib.parent.mkdir(parents=True); job.lib.write_text('preexisting')
            report=job.run(); self.assertFalse(report['retained']); self.assertEqual(job.lib.read_text(),'preexisting')
            self.assertFalse(job.calls)

if __name__=='__main__': unittest.main(verbosity=2)
