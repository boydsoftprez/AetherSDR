"""Offline contract tests: transport doubles never connect to a radio.

Copyright (c) 2026 J.J. Boyd. SPDX-License-Identifier: MIT
"""
import ast
import contextlib
import importlib.util
import io
import json
import pathlib
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

HERE = pathlib.Path(__file__).resolve().parent


def load(name):
    spec = importlib.util.spec_from_file_location(name.replace('-', '_'), HERE / (name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class OperatorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = pathlib.Path(self.temp.name)
        self.token = 'a1' * 32
        self.token_file = self.root / 'token'
        self.token_file.write_text(self.token + '\n')
        self.token_file.chmod(0o600)
        self.key = self.root / 'key'
        self.key.write_text('test-only dummy key')
        self.known = self.root / 'known-hosts'
        self.known.write_text('test-only pinned host fixture')
        self.job = self.root / 'job.py'
        self.job.write_text("print('fixture native job')\n")
        self.package = self.root / 'NereusNativeProbe.ssdr_waveform'
        with zipfile.ZipFile(self.package, 'w') as archive:
            archive.writestr('NereusNativeProbe.cfg', '[setup]\n[end]\n')
        self.package.chmod(0o600)
        self.output = self.root / 'evidence' / 'report.json'
        self.state = self.root / 'private state'
        self.expected_sha = 'b2' * 32
        self.argv = [str(self.job), str(self.output), '--host', 'radio-owner.example',
                     '--ssh-key', str(self.key), '--known-hosts', str(self.known),
                     '--token-file', str(self.token_file), '--native-package', str(self.package),
                     '--state-dir', str(self.state), '--expected-sha256', self.expected_sha]

    def tearDown(self):
        self.temp.cleanup()

    def test_missing_configuration_fails_before_transport(self):
        """Removing required operator args must never dial a default host."""
        runner = load('native-run')
        with patch.object(runner.subprocess, 'run', side_effect=AssertionError('transport called')):
            with patch.object(runner.socket, 'create_connection', side_effect=AssertionError('dialed')):
                with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                    runner.main([str(self.job), str(self.output)])
        self.assertEqual(caught.exception.code, 2)

    def test_invalid_private_inputs_fail_before_transport(self):
        """Malformed token and public package cannot reach the SSH boundary."""
        runner = load('native-run')
        cases = [('hash', lambda: self.argv.__setitem__(-1, 'bad-digest')),
                 ('token', lambda: self.token_file.write_text('invalid')),
                 ('package', lambda: self.package.chmod(0o644))]
        for label, mutate in cases:
            with self.subTest(label=label):
                self.token_file.write_text(self.token + '\n')
                self.package.chmod(0o600)
                self.argv[-1] = self.expected_sha
                mutate()
                with patch.object(runner.subprocess, 'run', side_effect=AssertionError('transport called')):
                    with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                        runner.main(self.argv)
                self.assertEqual(caught.exception.code, 2)

    def test_generated_receiver_and_secret_transport_with_fake_ssh(self):
        """Receiver must compile, and authentication must stay out of argv and evidence."""
        runner = load('native-run')
        uploaded = self.root / 'payload with spaces.bin'
        uploaded.write_bytes(b'test fixture')
        calls = []
        inputs = []
        report = {'wait': {'StatusCode': 0}, 'logs': 'fixture job result',
                  'job_container_deleted': True, 'temporary_host_directory_deleted': True}

        class Input(io.StringIO):
            def close(self):
                inputs.append(self.getvalue())
                super().close()

        class Process:
            def __init__(self, argv, **kwargs):
                calls.append(argv)
                self.stdin = Input() if kwargs.get('stdin') == subprocess.PIPE else None
                self.log = kwargs['stdout']
                self.receiver = argv[0] == 'ssh'
                self.returncode = None
                if self.receiver:
                    self.log.write('READY: fixture receiver\n')
                    self.log.flush()
            def poll(self):
                return self.returncode
            def wait(self, timeout):
                if self.receiver:
                    self.log.write(json.dumps(report) + '\n')
                    self.log.flush()
                self.returncode = 0
                return 0
            def terminate(self):
                pass
            def kill(self):
                pass

        class Channel:
            def __enter__(self):
                return self
            def __exit__(self, *args):
                return False
            def sendall(self, data):
                inputs.append(data.decode())
            def settimeout(self, value):
                pass
            def recv(self, bound):
                return b'{"bridge_sent":true,"peer_uid":0}'

        def run(argv, **kwargs):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, b'', b'')

        displayed = io.StringIO()
        with patch.object(runner.subprocess, 'run', side_effect=run), \
             patch.object(runner.subprocess, 'Popen', side_effect=Process), \
             patch.object(runner.socket, 'create_connection', return_value=Channel()), \
             patch.object(runner.time, 'sleep'), contextlib.redirect_stdout(displayed):
            self.assertEqual(runner.main(self.argv + ['--file', str(uploaded)]), 0)
        receivers = list(self.state.glob('receiver-*.py'))
        self.assertEqual(len(receivers), 1)
        generated = receivers[0].read_text()
        compile(generated, 'receiver', 'exec')
        self.assertNotIn(self.token, generated)
        tree = ast.parse(generated)
        assignment = next(node for node in ast.walk(tree) if isinstance(node, ast.Assign)
                          and any(isinstance(target, ast.Name) and target.id == 'code' for target in node.targets))
        native_code = eval(compile(ast.Expression(assignment.value), 'native-code-expression', 'eval'),
                           {'work': 'var/tmp/offline-fixture'})
        native_globals = {}
        with contextlib.redirect_stdout(io.StringIO()) as job_output:
            exec(compile(native_code, 'native-host-job', 'exec'), native_globals)
        self.assertEqual(native_globals['WORKDIR'], '/var/tmp/offline-fixture')
        self.assertEqual(native_globals['EXPECTED_SHA256'], self.expected_sha)
        self.assertEqual(job_output.getvalue(), 'fixture native job\n')
        for forbidden in ('/Users/', '192.168.', '2600:'):
            self.assertNotIn(forbidden, generated)
        self.assertNotIn(self.token, repr(calls))
        self.assertNotIn(self.token, displayed.getvalue() + self.output.read_text())
        self.assertEqual(inputs, [self.token + '\n', self.token + ':host-root-fd\n'])
        self.assertEqual(stat.S_IMODE(self.state.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(receivers[0].stat().st_mode), 0o600)
        self.assertEqual(json.loads(self.output.read_text())['wait']['StatusCode'], 0)
        self.assertTrue(any('--host' in call and 'radio-owner.example' in call for call in calls))
        self.assertTrue(any('UserKnownHostsFile=' + str(self.known.resolve()) in call for call in calls))
        self.assertTrue(any("payload with spaces.bin'" in ' '.join(call) for call in calls))



    def test_ack_requires_configuration_and_uses_only_owner_ssh(self):
        """ACK must run a localhost socket operation behind pinned owner SSH."""
        module = load('ack-accepted-runtime')
        with patch.object(module.subprocess, 'run', side_effect=AssertionError('transport called')):
            with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                module.main([])
        self.assertEqual(caught.exception.code, 2)
        calls = []
        def run(argv, **kwargs):
            calls.append(argv)
            return subprocess.CompletedProcess(argv, 0, '{"acknowledged":true}\n', '')
        with patch.object(module.subprocess, 'run', side_effect=run), contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(module.main(['--host', 'radio-owner.example', '--ssh-key', str(self.key),
                                         '--known-hosts', str(self.known), '--output', str(self.output)]), 0)
        self.assertEqual(json.loads(self.output.read_text())['rc'], 0)
        self.assertEqual(calls[0][0], 'ssh')
        self.assertIn('root@radio-owner.example', calls[0])
        self.assertIn('127.0.0.1', calls[0][-1])
        self.assertIn('46861', calls[0][-1])

    def test_uploader_rejects_public_package_before_dialing(self):
        """Standalone upload must protect the token-bearing package too."""
        module = load('native-waveform-upload')
        self.package.chmod(0o644)
        with patch.object(module.socket, 'create_connection', side_effect=AssertionError('dialed')):
            with self.assertRaises(ValueError):
                module.main(['--host', 'radio-owner.example', '--package', str(self.package)])

    def test_uploader_and_control_probe_require_host_without_dialing(self):
        """Missing --host cannot use the original personal LAN or IPv6 address."""
        for name in ['native-waveform-upload', 'control-meter-client']:
            with self.subTest(name=name):
                module = load(name)
                with patch.object(module.socket, 'create_connection', side_effect=AssertionError('dialed')):
                    with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as caught:
                        module.main([])
                self.assertEqual(caught.exception.code, 2)


if __name__ == '__main__':
    unittest.main()
