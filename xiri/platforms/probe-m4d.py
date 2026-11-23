#!/usr/bin/env python3
"""Verify M4d recovery after an abrupt Octane emulator stop.

The disposable CHD is prepared once for ttyC0 multi-user login.  A synced
baseline is created, then additional writes are left unsynced and the
emulator is stopped without a guest shutdown.  A single-user recovery boot
runs fsck; a later multi-user boot logs in, verifies the synced baseline,
and halts cleanly.  Unsynced writes are not required to survive.
"""
import argparse
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

spec = importlib.util.spec_from_file_location('m4c_probe', HERE / 'probe-m4c.py')
m4c = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m4c)

BASELINE = '/m4d-persist/baseline.txt'
BASELINE_DATA = b'M4D_BASELINE_V1\n'
UNSYNCED_TEXT = '/m4d-persist/unsynced.txt'
UNSYNCED_BIN = '/m4d-persist/unsynced.bin'


def sha256(path):
    return m4c.sha256(path)


class CrashProbe(m4c.MultiUserProbe):
    def __init__(self, args):
        super().__init__(args)
        self.result = {
            'schema': 1,
            'date': '2026-11-16',
            'milestone': 'M4d',
            'status': 'incomplete',
            'branch': 'topic/sgimips-octane',
            'source_revision': 'bc1ab70174b',
            'inputs': {},
            'prep': {},
            'baseline': {},
            'recovery': {},
            'login': {},
            'disk': {},
        }

    def run_baseline(self):
        boot_dir = self.boot_dir('baseline')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_multi(lab)
            serial_before = lab.serial_text()
            records = self.run_commands(lab, (
                'id; whoami',
                '/sbin/mount',
                'mkdir -p /m4d-persist',
                "printf 'M4D_BASELINE_V1\\n' > " + BASELINE,
                'sha256 ' + BASELINE,
                'ls -lnai /m4d-persist',
                'sync',
                'echo M4D_SYNC_OK',
                "printf 'M4D_UNSYNCED_V1\\n' > " + UNSYNCED_TEXT,
                'dd if=/dev/zero of=' + UNSYNCED_BIN + ' bs=4096 count=64',
                'echo M4D_UNSYNCED_OK',
            ))
            serial = ''.join(record['output'] for record in records)
            if 'uid=0(root)' not in serial:
                raise RuntimeError('baseline boot did not identify root')
            if 'on / type ffs (local)' not in serial:
                raise RuntimeError('baseline root is not writable')
            if not m4c.marker_line(serial, 'M4D_SYNC_OK'):
                raise RuntimeError('baseline sync marker missing')
            if not m4c.marker_line(serial, 'M4D_UNSYNCED_OK'):
                raise RuntimeError('unsynced write marker missing')
            digest = m4c.sha_for_path(serial, BASELINE)
            if digest != hashlib.sha256(BASELINE_DATA).hexdigest():
                raise RuntimeError('baseline file hash mismatch')
            metadata = m4c.metadata_for(serial, 'baseline.txt')
            if metadata is None or metadata['size'] != str(len(BASELINE_DATA)):
                raise RuntimeError('baseline metadata mismatch')
            self.result['baseline'] = {
                'output_directory': str(boot_dir),
                'status': 'synced-before-abrupt-stop',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': False,
                'abrupt_stop': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
                'file_sha256': digest,
                'file_metadata': metadata,
                'sync_marker': True,
                'unsynced_marker': True,
                'unsynced_writes': [UNSYNCED_TEXT, UNSYNCED_BIN],
            }
        finally:
            lab.logged_in = False
            lab.clean_shutdown = False
            self.close(lab)

    def recovery_commands(self, lab):
        records = []
        allowed = {
            '/sbin/fsck -fp /dev/sd0a': {0},
            '/sbin/fsck -fn /dev/sd0a': {0},
        }
        for command in (
            '/sbin/mount',
            '/sbin/fsck -fp /dev/sd0a',
            '/sbin/fsck -fn /dev/sd0a',
            'sha256 ' + BASELINE,
            'ls -lnai /m4d-persist',
            'cat ' + BASELINE,
            'echo M4D_RECOVERY_OK',
        ):
            rc, output = self.command(lab, command, prompt='xiri-octane# ')
            records.append({'command': command, 'returncode': rc,
                            'output': output})
            if command == '/sbin/fsck -fp /dev/sd0a' and rc == 4:
                raise RuntimeError('fsck requires reboot (4); unsupported')
            if rc not in allowed.get(command, {0}):
                raise RuntimeError('recovery command failed (%d): %s' %
                                   (rc, command))
        return records

    def run_recovery(self):
        boot_dir = self.boot_dir('recovery')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_single(lab)
            records = self.recovery_commands(lab)
            serial = ''.join(record['output'] for record in records)
            if 'on / type ffs (read-only' not in serial:
                raise RuntimeError('recovery root is not read-only')
            preen = next(record['output'] for record in records
                         if record['command'] == '/sbin/fsck -fp /dev/sd0a')
            check = next(record['output'] for record in records
                         if record['command'] == '/sbin/fsck -fn /dev/sd0a')
            if 'UNRESOLVED INCONSISTENCIES' in check or 'SALVAGE?' in check:
                raise RuntimeError('fsck still reports inconsistencies')
            if 'File system is already clean' not in check:
                raise RuntimeError('fsck did not report a clean filesystem')
            digest = m4c.sha_for_path(serial, BASELINE)
            if digest != self.result['baseline']['file_sha256']:
                raise RuntimeError('synced baseline changed during recovery')
            metadata = m4c.metadata_for(serial, 'baseline.txt')
            if metadata != self.result['baseline']['file_metadata']:
                raise RuntimeError('baseline metadata changed during recovery')
            shutdown = self.halt(lab)
            self.result['recovery'] = {
                'output_directory': str(boot_dir),
                'status': 'fsck-recovered',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
                'fsck_preen': preen,
                'fsck_check': check,
                'fsck_clean': True,
                'file_sha256': digest,
                'file_metadata': metadata,
                'shutdown_output': shutdown,
            }
        finally:
            self.close(lab)

    def run_login(self):
        boot_dir = self.boot_dir('login')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_multi(lab)
            records = self.run_commands(lab, (
                'id; whoami',
                '/sbin/mount',
                'tty',
                'stty -a',
                'sha256 ' + BASELINE,
                'ls -lnai /m4d-persist',
                'cat ' + BASELINE,
                'echo M4D_LOGIN_OK',
            ))
            serial = ''.join(record['output'] for record in records)
            if 'uid=0(root)' not in serial:
                raise RuntimeError('recovery login did not identify root')
            if 'on / type ffs (local)' not in serial:
                raise RuntimeError('recovery login root is not writable')
            if not m4c.exact_line(serial, '/dev/ttyC0'):
                raise RuntimeError('recovery login tty is not ttyC0')
            if not m4c.marker_line(serial, 'M4D_LOGIN_OK'):
                raise RuntimeError('recovery login marker missing')
            digest = m4c.sha_for_path(serial, BASELINE)
            if digest != self.result['baseline']['file_sha256']:
                raise RuntimeError('synced baseline changed after recovery')
            metadata = m4c.metadata_for(serial, 'baseline.txt')
            if metadata != self.result['baseline']['file_metadata']:
                raise RuntimeError('baseline metadata changed after recovery')
            shutdown = self.halt(lab)
            self.result['login'] = {
                'output_directory': str(boot_dir),
                'status': 'passed',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
                'identity': 'root',
                'tty': '/dev/ttyC0',
                'file_sha256': digest,
                'file_metadata': metadata,
                'shutdown_output': shutdown,
            }
        finally:
            self.close(lab)

    def check_disk(self):
        base = self.out / 'disk.chd'
        overlay = self.out / 'disk.chd.diff.chd'
        if sha256(base) != sha256(self.disk):
            raise RuntimeError('disposable disk base changed')
        if not overlay.is_file():
            raise RuntimeError('persistent CHD overlay missing')
        self.result['disk'] = {
            'source': str(self.disk),
            'source_sha256': sha256(self.disk),
            'copy': str(base),
            'copy_sha256': sha256(base),
            'overlay': str(overlay),
            'overlay_sha256': sha256(overlay),
            'overlay_bytes': overlay.stat().st_size,
        }

    def run(self):
        self.out.mkdir(parents=True, exist_ok=False)
        self.result['inputs'] = {
            'netbsd': sha256(self.image),
            'nvram': sha256(self.nvram),
            'flash': sha256(self.flash),
            'disk': sha256(self.disk),
            'iris': sha256(self.emulator / 'iris'),
            'iris-ci': sha256(self.emulator / 'iris-ci'),
        }
        self.lab_module = self.load_lab_module()
        shutil.copyfile(self.disk, self.out / 'disk.chd')
        try:
            self.run_prep()
            self.run_baseline()
            self.run_recovery()
            self.run_login()
            self.check_disk()
            self.result['status'] = 'passed-crash-recovery'
        except Exception as error:
            self.result['status'] = 'probe-error'
            self.result['error'] = str(error)
        finally:
            try:
                self.check_inputs()
            except Exception as error:
                if self.result['status'] == 'passed-crash-recovery':
                    self.result['status'] = 'probe-error'
                previous = self.result.get('error')
                self.result['error'] = ((previous + '; ') if previous else '') + str(error)
            self.result['finished_utc'] = time.strftime(
                '%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            (self.out / 'result.json').write_text(
                json.dumps(self.result, indent=2) + '\n')
        print('octane: ' + self.result['status'], flush=True)
        return 0 if self.result['status'] == 'passed-crash-recovery' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('xiri', 'emulator', 'image', 'nvram', 'flash', 'disk', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('--output must not exist')
    return CrashProbe(args).run()


if __name__ == '__main__':
    sys.exit(main())
