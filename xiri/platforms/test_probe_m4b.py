#!/usr/bin/env python3
"""Negative tests for the M4b persistence probe."""
import hashlib
import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def load_probe():
    spec = importlib.util.spec_from_file_location(
        'm4b_probe_under_test', HERE / 'probe-m4b.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


probe = load_probe()


class PatternTests(unittest.TestCase):
    def test_pattern_is_nonzero_and_pages_are_distinct(self):
        data = probe.pattern_bytes()
        self.assertEqual(len(data), 65536)
        self.assertNotIn(0, data)
        pages = {data[i * 4096:(i + 1) * 4096] for i in range(16)}
        self.assertEqual(len(pages), 16)
        probe.verify_pattern(data)

    def test_zeroed_page_is_rejected(self):
        data = bytearray(probe.pattern_bytes())
        data[3 * 4096:4 * 4096] = b'\0' * 4096
        with self.assertRaisesRegex(ValueError, 'page 3'):
            probe.verify_pattern(bytes(data))

    def test_swapped_pages_are_rejected(self):
        data = bytearray(probe.pattern_bytes())
        a = data[1 * 4096:2 * 4096]
        b = data[2 * 4096:3 * 4096]
        data[1 * 4096:2 * 4096] = b
        data[2 * 4096:3 * 4096] = a
        with self.assertRaisesRegex(ValueError, 'page 1'):
            probe.verify_pattern(bytes(data))

    def test_repeated_page_is_rejected(self):
        data = bytearray(probe.pattern_bytes())
        data[4 * 4096:5 * 4096] = data[3 * 4096:4 * 4096]
        with self.assertRaisesRegex(ValueError, 'page 4'):
            probe.verify_pattern(bytes(data))


class MutatingProbe(probe.PersistenceProbe):
    def __init__(self, root, mutation=None):
        self.out = root / 'run'
        self.disk = root / 'inputs' / 'disk.chd'
        self.image = root / 'inputs' / 'netbsd'
        self.nvram = root / 'inputs' / 'nvram.bin'
        self.flash = root / 'inputs' / 'flash.bin'
        self.emulator = root / 'emulator'
        self.xiri = root / 'xiri'
        self.timeout = 1
        self.mutation = mutation
        self.lab_module = None
        self.result = {
            'status': 'incomplete',
            'inputs': {},
            'boot1': {},
            'boot2': {},
            'files': {},
            'baseline_m4a': {},
        }

    def load_lab_module(self):
        return None

    def run_boot1(self):
        (self.out / 'disk.chd.diff.chd').write_bytes(b'overlay-v1')

    def run_boot2(self):
        if self.mutation == 'overlay':
            (self.out / 'disk.chd.diff.chd').write_bytes(b'overlay-v2')
        elif self.mutation == 'base':
            with (self.out / 'disk.chd').open('ab') as stream:
                stream.write(b'x')
        elif self.mutation == 'nvram':
            self.nvram.write_bytes(b'changed-nvram')

    def run_baseline(self):
        self.result['status'] = 'passed-persistence-across-clean-restart'


class IntegrityTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        inputs = self.root / 'inputs'
        inputs.mkdir()
        (inputs / 'disk.chd').write_bytes(b'base-disk')
        (inputs / 'netbsd').write_bytes(b'kernel')
        (inputs / 'nvram.bin').write_bytes(b'nvram')
        (inputs / 'flash.bin').write_bytes(b'flash')
        (self.root / 'emulator').mkdir()
        (self.root / 'emulator' / 'iris').write_bytes(b'iris')
        (self.root / 'emulator' / 'iris-ci').write_bytes(b'iris-ci')

    def tearDown(self):
        self.temp.cleanup()

    def assert_failure(self, mutation, message, inputs_unchanged):
        instance = MutatingProbe(self.root, mutation)
        rc = instance.run()
        self.assertNotEqual(rc, 0)
        result = json.loads((instance.out / 'result.json').read_text())
        self.assertEqual(result['status'], 'probe-error')
        self.assertIn(message, result['error'])
        self.assertEqual(result['originals_unchanged'], inputs_unchanged)
        self.assertIn('changed_inputs', result)
        if inputs_unchanged:
            self.assertEqual(result['changed_inputs'], [])
        return result

    def test_overlay_change_is_a_failure(self):
        result = self.assert_failure('overlay', 'overlay changed after boot2', True)
        self.assertEqual(result['disk']['overlay_sha256_after_boot2'],
                         hashlib.sha256(b'overlay-v2').hexdigest())

    def test_base_change_after_boot2_is_a_failure(self):
        result = self.assert_failure('base', 'disk base changed after boot2', True)
        self.assertEqual(result['disk']['copy_sha256_after_boot2'],
                         hashlib.sha256(b'base-diskx').hexdigest())

    def test_original_input_change_is_a_failure(self):
        result = self.assert_failure('nvram', 'original inputs changed: nvram', False)
        self.assertFalse(result['originals_unchanged'])
        self.assertEqual(result['changed_inputs'], ['nvram'])

    def test_unchanged_inputs_pass(self):
        instance = MutatingProbe(self.root)
        self.assertEqual(instance.run(), 0)
        result = json.loads((instance.out / 'result.json').read_text())
        self.assertEqual(result['status'], 'passed-persistence-across-clean-restart')
        self.assertTrue(result['originals_unchanged'])


if __name__ == '__main__':
    unittest.main()
