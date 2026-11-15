#!/usr/bin/env python3
"""Verify M4b persistence across a clean Octane restart.

The source disk is copied once.  Both boots use that copy and the CHD overlay
created beside it.  The first boot writes and syncs known files, then halts.
The second boot checks fsck, hashes and metadata before halting again.  The
retained M4a read-only probe is rerun against the untouched source disk.
"""
import argparse
import hashlib
import json
import re
import shutil
import subprocess
import sys
import time
import uuid
from pathlib import Path
from types import SimpleNamespace

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import probe as m4a_probe

PATTERN_PAGE_SIZE = 4096
PATTERN_PAGES = 16


def pattern_page(page):
    return bytes(33 + ((page * 97 + offset * 13) % 94)
                 for offset in range(PATTERN_PAGE_SIZE))


def pattern_bytes():
    return b''.join(pattern_page(page) for page in range(PATTERN_PAGES))


def verify_pattern(data):
    if len(data) != PATTERN_PAGE_SIZE * PATTERN_PAGES:
        raise ValueError('pattern length %d is not %d' %
                         (len(data), PATTERN_PAGE_SIZE * PATTERN_PAGES))
    for page in range(PATTERN_PAGES):
        start = page * PATTERN_PAGE_SIZE
        if data[start:start + PATTERN_PAGE_SIZE] != pattern_page(page):
            raise ValueError('pattern page %d mismatch' % page)


FILES = {
    '/m4b-persist/small.txt': b'M4B_SMALL_V1\n',
    '/m4b-persist/pages.bin': pattern_bytes(),
    '/m4b-persist/renamed.txt': b'M4B_RENAME_V1\n',
    '/m4b-persist/truncate.txt': b'\0' * 5,
}
PROMPT = 'xiri-octane# '


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def hashes_for(data):
    return {name: hashlib.sha256(value).hexdigest()
            for name, value in data.items()}


def parse_sha256(text):
    return {path: digest for path, digest in re.findall(
        r'SHA256 \((/m4b-persist/[^)]+)\) = ([0-9a-f]{64})', text)}


def parse_metadata(text):
    result = {}
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 10:
            continue
        name = fields[-1]
        full = next((path for path in FILES
                     if path.rsplit('/', 1)[-1] == name), None)
        if full is None:
            continue
        result[full] = {
            'inode': fields[0],
            'mode': fields[1],
            'links': fields[2],
            'uid': fields[3],
            'gid': fields[4],
            'size': fields[5],
            'mtime': ' '.join(fields[6:-1]),
        }
    return result


def marker_line(text, marker):
    return re.search(r'(?m)^' + re.escape(marker) + r'[ \t]*$', text) is not None


class PersistenceProbe:
    def __init__(self, args):
        self.args = args
        self.xiri = args.xiri.resolve()
        self.emulator = args.emulator.resolve()
        self.image = args.image.resolve()
        self.nvram = args.nvram.resolve()
        self.flash = args.flash.resolve()
        self.disk = args.disk.resolve()
        self.out = args.output.resolve()
        self.timeout = args.timeout
        self.lab_module = None
        self.result = {
            'schema': 1,
            'date': '2026-11-15',
            'milestone': 'M4b',
            'status': 'incomplete',
            'branch': 'topic/sgimips-octane',
            'source_revision': 'eb98d281ff7',
            'inputs': {},
            'boot1': {},
            'boot2': {},
            'files': {},
            'baseline_m4a': {},
        }

    def boot_dir(self, name):
        path = self.out / name
        path.mkdir()
        shutil.copyfile(self.nvram, path / 'nvram.bin')
        shutil.copyfile(self.flash, path / 'flash.bin')
        return path

    def make_lab(self, boot_dir):
        disk = self.out / 'disk.chd'
        config = f'''nvram = "nvram.bin"
banks = [512, 0, 0, 0]
headless = true
no_audio = true
ci = true
[machine]
profile = "octane_ip30"
cpu = "r10000"
cpus = 1
[octane]
flash = "flash.bin"
[graphics]
board = "none"
[scsi.1]
path = {json.dumps(str(disk))}
cdrom = false
overlay = true
[network]
mode = "nat"
tftp_dir = {json.dumps(str(self.image.parent))}
'''
        lab = self.lab_module.Lab(
            SimpleNamespace(mode='ci', debugger=False, boot_timeout=self.timeout),
            boot_dir, config_text=config, working_directory=boot_dir)
        original = lab.launch

        def launch(command, name):
            command = list(command)
            try:
                index = command.index('--scsi1')
            except ValueError:
                pass
            else:
                del command[index:index + 2]
            return original(command, name)

        lab.launch = launch
        return lab

    def enter_shell(self, lab):
        lab.start(boot=False)
        menu = lab.wait(r'Option\?|Press any key to continue', 120)
        if 'Press any key' in menu[0]:
            lab.send('')
            lab.wait(r'Option\?', 60)
        lab.send('5')
        lab.wait(r'>> ', 30)
        for command in ('setenv console d', 'setenv AutoLoad No'):
            lab.send(command)
            lab.wait(r'>> ', 30)
        lab.send('boot -f bootp():' + self.image.name + ' --s')
        lab.wait(r'Enter pathname of shell', self.timeout)
        lab.send('')
        lab.wait(r'# ', 30)
        lab.send("export PS1='" + PROMPT + "'")
        lab.wait(re.escape(PROMPT), 30)

    def command(self, lab, command, timeout=60):
        marker = 'XIRI_M4B_' + uuid.uuid4().hex + '_RC='
        start = len(lab.serial_text())
        lab.offset = start
        lab.send(command + "; xiri_rc=$?; printf '\\n" + marker + "%s\\n' \"$xiri_rc\"")
        match = lab.wait(r'\n' + re.escape(marker) + r'(\d+)\n', timeout)
        lab.wait(re.escape(PROMPT), 10)
        return int(match.group(1)), lab.serial_text()[start:lab.offset]

    def run_commands(self, lab, commands, timeout=60):
        records = []
        for command in commands:
            rc, output = self.command(lab, command, timeout)
            records.append({'command': command, 'returncode': rc,
                            'output': output})
            if rc != 0:
                raise RuntimeError('command failed (%d): %s' % (rc, command))
        return records

    def halt(self, lab):
        start = len(lab.serial_text())
        lab.offset = start
        lab.send('shutdown -h now')
        lab.wait(r'unmounting done', 60)
        lab.wait(r'Option\?', 15)
        lab.clean_shutdown = True
        lab.logged_in = False
        return lab.serial_text()[start:lab.offset]

    def close_lab(self, lab):
        if lab is not None:
            lab.close()

    def run_boot1(self):
        boot_dir = self.boot_dir('boot1')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_shell(lab)
            records = self.run_commands(lab, (
                '/sbin/mount',
                'mount -u -w /',
                '/sbin/mount',
                'mkdir -p /m4b-persist',
                "printf 'M4B_SMALL_V1\\n' > /m4b-persist/small.txt && printf 'M4B_%s\\n' CREATE_OK",
                "awk 'BEGIN { for (p = 0; p < 16; p++) for (i = 0; i < 4096; i++) printf \"%c\", 33 + ((p * 97 + i * 13) % 94) }' > /m4b-persist/pages.bin && printf 'M4B_%s\\n' PAGES_OK",
                "printf 'M4B_RENAME_V1\\n' > /m4b-persist/rename.tmp && mv /m4b-persist/rename.tmp /m4b-persist/renamed.txt && printf 'M4B_%s\\n' RENAME_OK",
                "printf '0123456789abcdef\\n' > /m4b-persist/truncate.txt && dd if=/dev/zero of=/m4b-persist/truncate.txt bs=1 count=5 && printf 'M4B_%s\\n' TRUNCATE_OK",
                'ls -lnai /m4b-persist',
                'sha256 /m4b-persist/small.txt /m4b-persist/pages.bin /m4b-persist/renamed.txt /m4b-persist/truncate.txt',
                'df -k /',
                "sync && printf 'M4B_%s\\n' SYNC_OK",
            ))
            serial = ''.join(record['output'] for record in records)
            self.check_first_output(serial)
            shutdown = self.halt(lab)
            self.result['boot1'] = {
                'output_directory': str(boot_dir),
                'status': 'passed',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
                'mounts': {c['command']: c['output'] for c in records
                           if c['command'] in ('/sbin/mount', 'mount -u -w /')},
                'files': {
                    'hashes': parse_sha256(serial),
                    'metadata': parse_metadata(serial),
                },
                'shutdown_output': shutdown,
            }
        finally:
            self.close_lab(lab)

    def check_first_output(self, serial):
        for marker in ('M4B_CREATE_OK', 'M4B_PAGES_OK', 'M4B_RENAME_OK',
                       'M4B_TRUNCATE_OK', 'M4B_SYNC_OK'):
            if not marker_line(serial, marker):
                raise RuntimeError('missing first-boot marker: ' + marker)
        hashes = parse_sha256(serial)
        for expected in hashes_for(FILES).items():
            if hashes.get(expected[0]) != expected[1]:
                raise RuntimeError('first-boot hash mismatch: ' + expected[0])
        if 'on / type ffs (local)' not in serial:
            raise RuntimeError('first boot did not remount root writable')
        metadata = parse_metadata(serial)
        for name in FILES:
            if name not in metadata:
                raise RuntimeError('first-boot metadata missing: ' + name)
        self.result['files'] = {
            'expected_hashes': hashes_for(FILES),
            'hashes': hashes,
            'metadata': metadata,
        }

    def run_boot2(self):
        boot_dir = self.boot_dir('boot2')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_shell(lab)
            records = self.run_commands(lab, (
                '/sbin/mount',
                '/sbin/fsck -fn /dev/sd0a',
                'sha256 /m4b-persist/small.txt /m4b-persist/pages.bin /m4b-persist/renamed.txt /m4b-persist/truncate.txt',
                'ls -lnai /m4b-persist',
                "test ! -e /m4b-persist/rename.tmp && printf 'M4B_%s\\n' RENAME_OLD_ABSENT",
                'df -k /',
            ))
            serial = ''.join(record['output'] for record in records)
            self.check_second_output(serial, records)
            shutdown = self.halt(lab)
            self.result['boot2'] = {
                'output_directory': str(boot_dir),
                'status': 'passed',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
                'mounts': {c['command']: c['output'] for c in records
                           if c['command'] == '/sbin/mount'},
                'files': {
                    'hashes': parse_sha256(serial),
                    'metadata': parse_metadata(serial),
                },
                'fsck': next(c['output'] for c in records
                             if c['command'] == '/sbin/fsck -fn /dev/sd0a'),
                'shutdown_output': shutdown,
            }
        finally:
            self.close_lab(lab)

    def check_second_output(self, serial, records):
        if 'on / type ffs (read-only, local)' not in serial:
            raise RuntimeError('second boot root is not read-only')
        if not marker_line(serial, 'M4B_RENAME_OLD_ABSENT'):
            raise RuntimeError('rename source exists after restart')
        fsck = next(c['output'] for c in records
                    if c['command'] == '/sbin/fsck -fn /dev/sd0a')
        if 'UNRESOLVED INCONSISTENCIES' in fsck or 'SALVAGE?' in fsck:
            raise RuntimeError('fsck found unresolved inconsistencies')
        if 'File system is already clean' not in fsck:
            raise RuntimeError('fsck did not report a clean filesystem')
        first = self.result['boot1']['files']
        expected_hashes = self.result['files']['expected_hashes']
        hashes = parse_sha256(serial)
        for name, expected in expected_hashes.items():
            if hashes.get(name) != expected:
                raise RuntimeError('second-boot hash mismatch: ' + name)
        if first['hashes'] != hashes:
            raise RuntimeError('boot hashes changed across restart')
        metadata = parse_metadata(serial)
        if first['metadata'] != metadata:
            raise RuntimeError('boot metadata changed across restart')
        if not all(name in metadata for name in FILES):
            raise RuntimeError('second-boot metadata incomplete')

    def check_overlay_after_boot2(self):
        overlay = self.out / 'disk.chd.diff.chd'
        base = self.out / 'disk.chd'
        disk = self.result['disk']
        if not overlay.is_file():
            disk['overlay_present_after_boot2'] = False
            raise RuntimeError('persistent CHD overlay disappeared after boot2')
        after = sha256(overlay)
        disk['overlay_present_after_boot2'] = True
        disk['overlay_sha256_after_boot2'] = after
        disk['overlay_bytes_after_boot2'] = overlay.stat().st_size
        if after != disk['overlay_sha256']:
            raise RuntimeError('persistent CHD overlay changed after boot2')
        if not base.is_file():
            disk['copy_present_after_boot2'] = False
            raise RuntimeError('disposable disk base disappeared after boot2')
        base_hash = sha256(base)
        disk['copy_present_after_boot2'] = True
        disk['copy_sha256_after_boot2'] = base_hash
        if base_hash != disk['copy_sha256'] or base_hash != sha256(self.disk):
            raise RuntimeError('disposable disk base changed after boot2')

    def check_overlay(self):
        overlay = self.out / 'disk.chd.diff.chd'
        base = self.out / 'disk.chd'
        if not overlay.is_file():
            raise RuntimeError('persistent CHD overlay was not created')
        if sha256(base) != sha256(self.disk):
            raise RuntimeError('disposable disk base was modified')
        self.result['disk'] = {
            'source': str(self.disk),
            'source_sha256': sha256(self.disk),
            'copy': str(base),
            'copy_sha256': sha256(base),
            'overlay': str(overlay),
            'overlay_sha256': sha256(overlay),
            'overlay_bytes': overlay.stat().st_size,
        }

    def run_baseline(self):
        output = self.out / 'm4a-baseline'
        command = [
            sys.executable, str(HERE / 'probe.py'),
            '--xiri', str(self.xiri),
            '--emulator', str(self.emulator),
            '--image', str(self.image),
            '--nvram', str(self.nvram),
            '--flash', str(self.flash),
            '--output', str(output),
            '--machine', 'octane',
            '--disk', str(self.disk),
            '--single-user',
            '--expect', 'XIRI_IP30_M3D2_READY',
            '--timeout', str(self.timeout),
        ]
        run = subprocess.run(command, text=True, capture_output=True)
        result_file = output / 'result.json'
        baseline = {
            'output_directory': str(output),
            'command': ' '.join(command),
            'status': 'failed',
            'returncode': run.returncode,
        }
        if result_file.is_file():
            data = json.loads(result_file.read_text())
            baseline.update({
                'status': data.get('status'),
                'clean_shutdown': data.get('clean_shutdown'),
                'shell': data.get('shell'),
                'result_sha256': sha256(result_file),
                'serial_sha256': sha256(output / 'serial.log'),
            })
        self.result['baseline_m4a'] = baseline
        if baseline['status'] != 'passed':
            raise RuntimeError('M4a baseline failed: ' + run.stdout + run.stderr)
        self.result['status'] = 'passed-persistence-across-clean-restart'

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
            self.run_boot1()
            self.check_overlay()
            self.run_boot2()
            self.check_overlay_after_boot2()
            self.run_baseline()
        except Exception as error:
            self.result['status'] = 'probe-error'
            self.result['error'] = str(error)
        finally:
            try:
                self.check_inputs_unchanged()
            except Exception as error:
                if self.result['status'] == 'passed-persistence-across-clean-restart':
                    self.result['status'] = 'probe-error'
                previous = self.result.get('error')
                self.result['error'] = ((previous + '; ') if previous else '') + str(error)
            self.result['finished_utc'] = time.strftime(
                '%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            (self.out / 'result.json').write_text(
                json.dumps(self.result, indent=2) + '\n')
        return 0 if self.result['status'] == 'passed-persistence-across-clean-restart' else 1

    def load_lab_module(self):
        scripts = self.xiri / 'scripts'
        sys.path.insert(0, str(scripts))
        from xiri_lab import load_probe
        module = load_probe()
        module.TOOLBOX = []
        module.IRIS = self.emulator / 'iris'
        module.CI = self.emulator / 'iris-ci'
        return module

    def check_inputs_unchanged(self):
        current = {
            'netbsd': sha256(self.image),
            'nvram': sha256(self.nvram),
            'flash': sha256(self.flash),
            'disk': sha256(self.disk),
            'iris': sha256(self.emulator / 'iris'),
            'iris-ci': sha256(self.emulator / 'iris-ci'),
        }
        changed = sorted(name for name in current
                         if current[name] != self.result['inputs'].get(name))
        self.result['originals_unchanged'] = not changed
        self.result['changed_inputs'] = changed
        if changed:
            raise RuntimeError('original inputs changed: ' + ', '.join(changed))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('xiri', 'emulator', 'image', 'nvram', 'flash', 'disk', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=180)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('--output must not exist')
    return PersistenceProbe(args).run()


if __name__ == '__main__':
    sys.exit(main())
