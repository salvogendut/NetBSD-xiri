#!/usr/bin/env python3
"""Negative tests for M4d crash recovery."""
import importlib.util
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def load_probe():
    spec = importlib.util.spec_from_file_location(
        'm4d_probe_under_test', HERE / 'probe-m4d.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


probe = load_probe()


class FsckRebootTests(unittest.TestCase):
    def test_fsck_code_4_is_rejected_before_halt(self):
        class Fake:
            def __init__(self):
                self.halt_called = False

            def command(self, lab, command, timeout=60, prompt=probe.m4c.PROMPT,
                        shell='sh'):
                if command == '/sbin/fsck -fp /dev/sd0a':
                    return 4, 'fsck: reloading repaired root failed\n'
                return 0, ''

            def halt(self, lab):
                self.halt_called = True

            recovery_commands = probe.CrashProbe.recovery_commands

        fake = Fake()
        with self.assertRaisesRegex(RuntimeError, 'fsck requires reboot'):
            fake.recovery_commands(None)
        self.assertFalse(fake.halt_called)


if __name__ == '__main__':
    unittest.main()
