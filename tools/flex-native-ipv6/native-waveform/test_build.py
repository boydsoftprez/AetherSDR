# Copyright (c) 2026 J.J. Boyd. Authored with Codex. SPDX-License-Identifier: MIT.
import importlib.util
import pathlib
import tempfile
import unittest
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent


class PackageTests(unittest.TestCase):
    def load_builder(self):
        self.assertTrue((ROOT / 'build.py').is_file(), 'missing private native waveform builder')
        spec = importlib.util.spec_from_file_location('probe_builder', ROOT / 'build.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        return module

    def fixture(self, root):
        stage = root / 'NereusNativeProbe'
        stage.mkdir()
        for name in ('NereusNativeProbe.cfg', 'probe.sh', 'probe-arm64', 'probe-armv7', 'LICENSE', 'LICENSE-Go'):
            (stage / name).write_bytes(b'fixture')
        return stage

    def test_package_only_allowlisted_files_and_executable_bits(self):
        builder = self.load_builder()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stage = self.fixture(root)
            (stage / 'client-token.txt').write_text('private fixture value')
            target = root / 'NereusNativeProbe.ssdr_waveform'
            builder.create_package(stage, target)
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
            with zipfile.ZipFile(target) as archive:
                self.assertEqual(len(archive.namelist()), 6)
                self.assertNotIn('NereusNativeProbe/client-token.txt', archive.namelist())
                for info in archive.infolist():
                    self.assertTrue(info.filename.startswith('NereusNativeProbe/'))
                    expected = 0o755 if pathlib.PurePosixPath(info.filename).name.startswith('probe') else 0o644
                    self.assertEqual((info.external_attr >> 16) & 0o777, expected)

    def test_existing_output_is_preserved(self):
        builder = self.load_builder()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            target = root / 'NereusNativeProbe.ssdr_waveform'
            target.write_bytes(b'existing')
            with self.assertRaises(FileExistsError):
                builder.create_package(self.fixture(root), target)
            self.assertEqual(target.read_bytes(), b'existing')

    def test_symlink_source_is_rejected_before_output(self):
        builder = self.load_builder()
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            stage = self.fixture(root)
            (stage / 'probe-arm64').unlink()
            (stage / 'probe-arm64').symlink_to(stage / 'probe-armv7')
            target = root / 'NereusNativeProbe.ssdr_waveform'
            with self.assertRaises(ValueError):
                builder.create_package(stage, target)
            self.assertFalse(target.exists())


if __name__ == '__main__':
    unittest.main()
