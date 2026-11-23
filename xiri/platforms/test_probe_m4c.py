#!/usr/bin/env python3
"""Negative tests for the M4c multi-user login probe."""
import importlib.util
import sys
import unittest
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def load_probe():
    spec = importlib.util.spec_from_file_location(
        'm4c_probe_under_test', HERE / 'probe-m4c.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


probe = load_probe()

BOOT = 'Starting cron\n'
GOOD = '''uid=0(root) gid=0(wheel)
root
NetBSD xiri-indy 11.99.9 XIRI_IP30_M4A
/dev/sd0a on / type ffs (local)
/dev/ttyC0
speed 9600 baud; 0 rows; 0 columns
XIRI_M4C_CONSOLE_CHAR_OK
XIRI_M4C_CONSOLE_WRITE
M4C_TTY_IO_OK
M4C_FILE_OK
M4C_SYNC_OK
'''


class LoginValidationTests(unittest.TestCase):
    def test_good_transcript_passes(self):
        probe.validate_logged_output(GOOD, BOOT, True)

    def test_wrong_tty_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'tty did not report'):
            probe.validate_logged_output(GOOD.replace('/dev/ttyC0', '/dev/ttyZ0'),
                                         BOOT, True)

    def test_missing_console_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'console character-device'):
            probe.validate_logged_output(
                GOOD.replace('XIRI_M4C_CONSOLE_CHAR_OK\n', ''), BOOT, True)

    def test_regular_file_console_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'not a character device'):
            probe.validate_logged_output(
                GOOD.replace('XIRI_M4C_CONSOLE_CHAR_OK',
                             'XIRI_M4C_CONSOLE_CHAR_FAILED'), BOOT, True)

    def test_missing_console_write_is_rejected(self):
        with self.assertRaisesRegex(RuntimeError, 'console write check'):
            probe.validate_logged_output(
                GOOD.replace('XIRI_M4C_CONSOLE_WRITE\n', ''), BOOT, True)

    def test_failed_stty_is_rejected(self):
        class Fake:
            def command(self, lab, command, timeout=60, prompt=probe.PROMPT, shell='sh'):
                return 1, 'simulated stty failure\n'

            run_commands = probe.MultiUserProbe.run_commands

        with self.assertRaisesRegex(RuntimeError, 'command failed'):
            Fake().run_commands(None, ('stty -a',))

    def test_failed_console_test_is_rejected(self):
        class Fake:
            def command(self, lab, command, timeout=60, prompt=probe.PROMPT, shell='sh'):
                return 1, 'simulated console failure\n'

            run_commands = probe.MultiUserProbe.run_commands

        with self.assertRaisesRegex(RuntimeError, 'command failed'):
            Fake().run_commands(None, ('test -c /dev/console',))

    def test_zero_command_is_recorded(self):
        class Fake:
            def command(self, lab, command, timeout=60, prompt=probe.PROMPT, shell='sh'):
                return 0, 'ok\n'

            run_commands = probe.MultiUserProbe.run_commands

        records = Fake().run_commands(None, ('stty -a',))
        self.assertEqual(records[0]['returncode'], 0)


if __name__ == '__main__':
    unittest.main()
