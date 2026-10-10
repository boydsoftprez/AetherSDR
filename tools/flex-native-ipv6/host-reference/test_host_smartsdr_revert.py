"""Original local future-revert test. J.J. Boyd / Codex. SPDX-License-Identifier: MIT."""
import importlib.util, pathlib, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parent
spec=importlib.util.spec_from_file_location('revert',ROOT/'host-smartsdr-revert.py')
module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
class RevertTests(unittest.TestCase):
    def test_retry_and_stock_mapping_before_cleanup(self):
        self.assertTrue(callable(getattr(module,'revert',None)),'missing guarded exact-ownership revert job')
        with tempfile.TemporaryDirectory() as directory:
            root=pathlib.Path(directory); lib=root/'usr/local/lib/nereus-smartsdr-ipv6/libnereus_ipv6.so'
            lib.parent.mkdir(parents=True);lib.write_bytes((ROOT/'shim/libnereus_ipv6.so').read_bytes())
            dropin=root/'run/systemd/system/smartsdr.service.d/90-nereus-ipv6.conf'; dropin.parent.mkdir(parents=True)
            dropin.write_text('[Service]\nEnvironment="LD_PRELOAD='+str(lib)+'"\nEnvironment="NEREUS_IPV6_TCP_PORT=4992"\nEnvironment="NEREUS_IPV6_UDP_PORTS=4991,4993"\n')
            boot=root/'proc/sys/kernel/random/boot_id';boot.parent.mkdir(parents=True);boot.write_text('boot')
            maps=root/'proc/200/maps';maps.parent.mkdir();maps.write_text(str(lib))
            calls=[]; failed=False
            def command(args,timeout=20):
                nonlocal failed
                calls.append(args); text='';rc=0
                if args==['systemctl','daemon-reload'] and not failed:failed=True;rc=1
                elif args[:2]==['systemctl','restart']:
                    self.assertEqual(calls[-2],['systemctl','start','inhibit-power-pic.service']);maps.write_text('/vendor/lib.so')
                elif args[:2]==['systemctl','show']:text='MainPID=200\nActiveState=active\nSubState=running\n'
                return {'args':args,'rc':rc,'stdout':text,'stderr':''}
            result=module.revert(root=root,command=command)
            self.assertTrue(result['original_service_restored'],result);self.assertFalse(lib.exists());self.assertFalse(dropin.exists())
            self.assertEqual(calls.count(['systemctl','daemon-reload']),2)
if __name__=='__main__':unittest.main(verbosity=2)
