"""Original local digest and native-runner tests; no native host operations.
Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
"""
import hashlib
import importlib.util
import pathlib
import subprocess
import sys
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name, ROOT / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


acceptance = load('host-smartsdr-acceptance')
revert = load('host-smartsdr-revert')


class DigestTests(unittest.TestCase):
    def test_acceptance_requires_explicit_digest_before_any_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for value in (None, '', 'a' * 63, 'g' * 64, 'A' * 64, 'a' * 64 + '\n'):
                with self.subTest(value=value):
                    with self.assertRaises(ValueError):
                        if value is None:
                            acceptance.AcceptanceJob(root, root=root)
                        else:
                            acceptance.AcceptanceJob(root, root=root, expected_sha256=value)
                    self.assertEqual(list(root.iterdir()), [])

    def test_revert_requires_explicit_digest_before_any_write(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for value in (None, '', 'a' * 63, 'g' * 64, 'A' * 64, 'a' * 64 + '\n'):
                with self.subTest(value=value):
                    with self.assertRaises(ValueError):
                        if value is None:
                            revert.revert(root=root)
                        else:
                            revert.revert(root=root, expected_sha256=value)
                    self.assertEqual(list(root.iterdir()), [])

    def test_acceptance_mismatch_preserves_all_files_and_never_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            work = root / 'work'
            work.mkdir()
            library = work / 'libnereus_ipv6.so'
            library.write_bytes(b'reviewed source-only fake artifact')
            job = acceptance.AcceptanceJob(work, root=root, expected_sha256='0' * 64)
            job.command = lambda *_args, **_kwargs: self.fail('mismatch executed a native command')
            result = job.run()
            self.assertFalse(result['retained'])
            self.assertIn('SHA mismatch', result['error'])
            self.assertEqual(list(root.iterdir()), [work])
            self.assertEqual(library.read_bytes(), b'reviewed source-only fake artifact')

    def test_revert_mismatch_keeps_dropin_library_and_never_commands(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            library = root / 'usr/local/lib/nereus-smartsdr-ipv6/libnereus_ipv6.so'
            dropin = root / 'run/systemd/system/smartsdr.service.d/90-nereus-ipv6.conf'
            library.parent.mkdir(parents=True)
            dropin.parent.mkdir(parents=True)
            library.write_bytes(b'not the selected artifact')
            dropin.write_text('must remain untouched')
            def command(*_args, **_kwargs):
                self.fail('mismatch executed a native command')
            result = revert.revert(root=root, command=command, expected_sha256='0' * 64)
            self.assertFalse(result['original_service_restored'])
            self.assertEqual(dropin.read_text(), 'must remain untouched')
            self.assertEqual(library.read_bytes(), b'not the selected artifact')


class RunnerTests(unittest.TestCase):
    def run_runner(self, *args):
        return subprocess.run([sys.executable, str(ROOT / 'native-host-job.py'), *args],
                              capture_output=True, text=True, timeout=5)

    def test_missing_malformed_digest_prevents_file_job_execution(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            marker = root / 'executed'
            job = root / 'fake-job.py'
            job.write_text('import pathlib\npathlib.Path(' + repr(str(marker)) + ').touch()\n')
            for value in (None, '', 'g' * 64, 'a' * 63, 'A' * 64, 'a' * 64 + '\n'):
                args = ['--file', str(job), '--workdir', str(root)]
                if value is not None:
                    args += ['--expected-sha256', value]
                result = self.run_runner(*args)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('expected-sha256', result.stderr)
                self.assertFalse(marker.exists())

    def test_runner_injects_explicit_digest_and_absolute_workdir(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            marker = root / 'executed'
            job = root / 'fake-job.py'
            digest = hashlib.sha256(b'independent reviewed artifact').hexdigest()
            job.write_text('import pathlib\nassert EXPECTED_SHA256 == ' + repr(digest) +
                           '\nassert WORKDIR == ' + repr(str(root.resolve())) +
                           '\npathlib.Path(' + repr(str(marker)) + ').touch()\n')
            result = self.run_runner('--file', str(job), '--workdir', str(root),
                                     '--expected-sha256', digest)
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertTrue(marker.exists())

    def test_optimized_execution_cannot_bypass_firmware_preconditions(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            marker = root / 'executed'
            job = root / 'fake-job.py'
            job.write_text('import pathlib\npathlib.Path(' + repr(str(marker)) + ').touch()\n')
            result = subprocess.run([sys.executable, '-O', str(ROOT / 'native-host-job.py'),
                                     '--file', str(job), '--workdir', str(root),
                                     '--expected-sha256', 'a' * 64],
                                    capture_output=True, text=True, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertFalse(marker.exists())


if __name__ == '__main__':
    unittest.main(verbosity=2)
