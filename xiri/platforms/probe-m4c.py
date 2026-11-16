#!/usr/bin/env python3
"""Verify M4c multi-user serial login and clean-restart persistence.

The disposable CHD is prepared once in single-user mode to enable the
existing ttyC0 getty (the image defaults to Indy's ttyZ0).  Two multi-user
boots then complete /etc/rc, log in as root, exercise ttyC0, create and
verify a persistence file, and halt cleanly.  M4a and M4b are rerun after
the M4c session.
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

PROMPT = 'xiri-indy# '
PERSIST = '/m4c-persist/persist.txt'
PERSIST_DATA = b'M4C_PERSIST_V1\n'
TTYS_EDIT = ("awk '{ if ($1==\"ttyC0\") "
             "sub(/[[:space:]]off[[:space:]]secure/, \"\\ton\\tsecure\"); "
             "else if ($1==\"ttyZ0\") "
             "sub(/[[:space:]]on[[:space:]]secure/, \"\\toff\\tsecure\"); "
             "print }' /etc/ttys > /etc/ttys.m4c")


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def marker_line(text, marker):
    return re.search(r'(?m)^' + re.escape(marker) + r'[ \t]*\r?$',
                     text) is not None


def sha_for_path(text, path):
    match = re.search(r'SHA256 \(' + re.escape(path) + r'\) = ([0-9a-f]{64})',
                      text)
    return match.group(1) if match else None


def exact_line(text, line):
    return any(candidate == line for candidate in text.splitlines())


def validate_logged_output(serial, boot_serial, first):
    if 'uid=0(root)' not in serial or '\nroot\n' not in serial:
        raise RuntimeError('serial login did not identify root')
    if 'XIRI_IP30_M4A' not in serial:
        raise RuntimeError('M4A kernel identity missing')
    if 'on / type ffs (local)' not in serial:
        raise RuntimeError('root is not mounted writable')
    if not exact_line(serial, '/dev/ttyC0'):
        raise RuntimeError('tty did not report /dev/ttyC0')
    if exact_line(serial, '/dev/ttyZ0'):
        raise RuntimeError('tty reported /dev/ttyZ0')
    if 'speed' not in serial:
        raise RuntimeError('stty output missing')
    if marker_line(serial, 'XIRI_M4C_CONSOLE_CHAR_FAILED'):
        raise RuntimeError('console is not a character device')
    if not marker_line(serial, 'XIRI_M4C_CONSOLE_CHAR_OK'):
        raise RuntimeError('console character-device check failed')
    if not marker_line(serial, 'XIRI_M4C_CONSOLE_WRITE'):
        raise RuntimeError('console write check failed')
    if not marker_line(serial, 'M4C_TTY_IO_OK'):
        raise RuntimeError('interactive TTY output marker missing')
    if 'Starting cron' not in boot_serial:
        raise RuntimeError('/etc/rc did not complete')
    if first and not marker_line(serial, 'M4C_FILE_OK'):
        raise RuntimeError('file creation marker missing')
    if first and not marker_line(serial, 'M4C_SYNC_OK'):
        raise RuntimeError('sync marker missing')


def metadata_for(text, name):
    for line in text.splitlines():
        fields = line.split()
        if len(fields) < 10 or fields[-1] != name:
            continue
        return {
            'inode': fields[0],
            'mode': fields[1],
            'links': fields[2],
            'uid': fields[3],
            'gid': fields[4],
            'size': fields[5],
            'mtime': ' '.join(fields[6:-1]),
        }
    return None


class MultiUserProbe:
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
            'date': '2026-11-16',
            'milestone': 'M4c',
            'status': 'incomplete',
            'branch': 'topic/sgimips-octane',
            'source_revision': '4521ae38692',
            'inputs': {},
            'prep': {},
            'boot1': {},
            'boot2': {},
            'persistence': {},
            'regressions': {},
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

    def boot_menu(self, lab):
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

    def enter_single(self, lab):
        self.boot_menu(lab)
        lab.send('boot -f bootp():' + self.image.name + ' --s')
        lab.wait(r'Enter pathname of shell', self.timeout)
        lab.send('')
        lab.wait(r'# ', 30)
        lab.send("export PS1='xiri-octane# '")
        lab.wait(r'xiri-octane# ', 30)

    def enter_multi(self, lab):
        self.boot_menu(lab)
        lab.send('boot -f bootp():' + self.image.name)
        lab.wait(r'login: ', self.timeout)
        serial = lab.serial_text()
        if 'Starting cron' not in serial:
            raise RuntimeError('/etc/rc did not reach Starting cron')
        lab.send('root')
        lab.wait(re.escape(PROMPT), 60)

    def command(self, lab, command, timeout=60, prompt=PROMPT, shell='sh'):
        marker = 'XIRI_M4C_' + uuid.uuid4().hex + '_RC='
        start = len(lab.serial_text())
        lab.offset = start
        lab.send(command + "; printf '\\n" + marker + "%s\\n' \"$?\"")
        match = lab.wait(r'\n' + re.escape(marker) + r'(\d+)\n', timeout)
        lab.wait(re.escape(prompt), 10)
        return int(match.group(1)), lab.serial_text()[start:lab.offset]

    def run_commands(self, lab, commands, prompt=PROMPT, shell='sh'):
        records = []
        for command in commands:
            returncode, output = self.command(
                lab, command, prompt=prompt, shell=shell)
            records.append({'command': command, 'returncode': returncode,
                            'output': output})
            if returncode != 0:
                raise RuntimeError('command failed (%d): %s' %
                                   (returncode, command))
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

    def close(self, lab):
        if lab is not None:
            lab.close()

    def run_prep(self):
        boot_dir = self.boot_dir('prep')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_single(lab)
            records = self.run_commands(lab, (
                'mount -u -w /',
                TTYS_EDIT,
                'mv /etc/ttys.m4c /etc/ttys',
                "grep -E '^ttyC0|^ttyZ0' /etc/ttys",
                'sync',
                'echo M4C_PREP_SYNC_OK',
            ), prompt='xiri-octane# ')
            serial = ''.join(record['output'] for record in records)
            if 'on' not in serial or 'ttyC0' not in serial:
                raise RuntimeError('ttyC0 was not enabled')
            if not re.search(r'(?m)^ttyC0[ \t]+.*[ \t]on[ \t]+secure', serial):
                raise RuntimeError('ttyC0 ttys entry is not on')
            if not re.search(r'(?m)^ttyZ0[ \t]+.*[ \t]off[ \t]+secure', serial):
                raise RuntimeError('ttyZ0 ttys entry is not off')
            if not marker_line(serial, 'M4C_PREP_SYNC_OK'):
                raise RuntimeError('prep sync marker missing')
            shutdown = self.halt(lab)
            self.result['prep'] = {
                'output_directory': str(boot_dir),
                'status': 'passed',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
                'shutdown_output': shutdown,
            }
        finally:
            self.close(lab)

    def logged_commands(self):
        return (
            'id; whoami',
            'uname -a',
            '/sbin/mount',
            'tty',
            'stty -a',
            'test -c /dev/console',
            "printf 'XIRI_M4C_CONSOLE_%s\\n' CHAR_OK",
            "printf 'XIRI_M4C_CONSOLE_%s\\n' WRITE >/dev/console",
            'echo M4C_TTY_IO_OK',
        )

    def run_boot1(self):
        boot_dir = self.boot_dir('boot1')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_multi(lab)
            first = lab.serial_text()
            records = self.run_commands(lab, self.logged_commands() + (
                'mkdir -p /m4c-persist',
                "printf 'M4C_PERSIST_V1\\n' > " + PERSIST,
                'echo M4C_FILE_OK',
                'sha256 ' + PERSIST,
                'ls -lnai /m4c-persist',
                'sync',
                'echo M4C_SYNC_OK',
            ))
            serial = ''.join(record['output'] for record in records)
            self.check_logged_output(serial, first, True)
            digest = sha_for_path(serial, PERSIST)
            if digest != hashlib.sha256(PERSIST_DATA).hexdigest():
                raise RuntimeError('persistence file hash mismatch')
            metadata = metadata_for(serial, 'persist.txt')
            if metadata is None or metadata['size'] != str(len(PERSIST_DATA)):
                raise RuntimeError('persistence file metadata mismatch')
            shutdown = self.halt(lab)
            self.result['boot1'] = {
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

    def check_logged_output(self, serial, boot_serial, first):
        validate_logged_output(serial, boot_serial, first)

    def run_boot2(self):
        boot_dir = self.boot_dir('boot2')
        lab = self.make_lab(boot_dir)
        started = time.monotonic()
        try:
            self.enter_multi(lab)
            first = lab.serial_text()
            records = self.run_commands(lab, self.logged_commands() + (
                'sha256 ' + PERSIST,
                'cat ' + PERSIST,
                'ls -lnai /m4c-persist',
            ))
            serial = ''.join(record['output'] for record in records)
            self.check_logged_output(serial, first, False)
            digest = sha_for_path(serial, PERSIST)
            if digest != self.result['boot1']['file_sha256']:
                raise RuntimeError('persistence file changed across restart')
            if 'M4C_PERSIST_V1' not in serial:
                raise RuntimeError('persistence file content missing')
            metadata = metadata_for(serial, 'persist.txt')
            if metadata != self.result['boot1']['file_metadata']:
                raise RuntimeError('persistence metadata changed across restart')
            shutdown = self.halt(lab)
            self.result['persistence'] = {
                'path': PERSIST,
                'sha256': digest,
                'metadata': metadata,
                'content_ok': True,
            }
            self.result['boot2'] = {
                'output_directory': str(boot_dir),
                'status': 'passed',
                'elapsed_seconds': round(time.monotonic() - started, 2),
                'clean_shutdown': True,
                'serial_sha256': sha256(boot_dir / 'serial.log'),
                'commands': records,
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

    def run_m4a_regression(self):
        output = self.out / 'm4a-regression'
        command = [sys.executable, str(HERE / 'probe.py'),
                   '--xiri', str(self.xiri), '--emulator', str(self.emulator),
                   '--image', str(self.image), '--nvram', str(self.nvram),
                   '--flash', str(self.flash), '--output', str(output),
                   '--machine', 'octane', '--disk', str(self.disk),
                   '--single-user', '--expect', 'XIRI_IP30_M3D2_READY',
                   '--timeout', str(self.timeout)]
        run = subprocess.run(command, text=True, capture_output=True)
        data = self.read_regression(output, command, run.returncode)
        self.result['regressions']['m4a'] = data
        if data['status'] != 'passed':
            raise RuntimeError('M4a regression failed')

    def run_m4b_regression(self):
        output = self.out / 'm4b-regression'
        command = [sys.executable, str(HERE / 'probe-m4b.py'),
                   '--xiri', str(self.xiri), '--emulator', str(self.emulator),
                   '--image', str(self.image), '--nvram', str(self.nvram),
                   '--flash', str(self.flash), '--disk', str(self.disk),
                   '--output', str(output), '--timeout', str(self.timeout)]
        run = subprocess.run(command, text=True, capture_output=True)
        data = self.read_regression(output, command, run.returncode)
        self.result['regressions']['m4b'] = data
        if data['status'] != 'passed-persistence-across-clean-restart':
            raise RuntimeError('M4b regression failed')

    def read_regression(self, output, command, returncode):
        data = {
            'output_directory': str(output),
            'command': ' '.join(command),
            'returncode': returncode,
            'status': 'failed',
        }
        result_file = output / 'result.json'
        if result_file.is_file():
            inner = json.loads(result_file.read_text())
            data.update({
                'status': inner.get('status'),
                'clean_shutdown': inner.get('clean_shutdown'),
                'result_sha256': sha256(result_file),
            })
            if (output / 'serial.log').is_file():
                data['serial_sha256'] = sha256(output / 'serial.log')
            if (output / 'boot1/serial.log').is_file():
                data['boot1_serial_sha256'] = sha256(output / 'boot1/serial.log')
                data['boot2_serial_sha256'] = sha256(output / 'boot2/serial.log')
        return data

    def check_inputs(self):
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
            self.run_boot1()
            self.run_boot2()
            self.check_disk()
            self.run_m4a_regression()
            self.run_m4b_regression()
            self.result['status'] = 'passed-multi-user-login-and-persistence'
        except Exception as error:
            self.result['status'] = 'probe-error'
            self.result['error'] = str(error)
        finally:
            try:
                self.check_inputs()
            except Exception as error:
                if self.result['status'] == 'passed-multi-user-login-and-persistence':
                    self.result['status'] = 'probe-error'
                previous = self.result.get('error')
                self.result['error'] = ((previous + '; ') if previous else '') + str(error)
            self.result['finished_utc'] = time.strftime(
                '%Y-%m-%dT%H:%M:%SZ', time.gmtime())
            (self.out / 'result.json').write_text(
                json.dumps(self.result, indent=2) + '\n')
        print('octane: ' + self.result['status'], flush=True)
        return 0 if self.result['status'] == 'passed-multi-user-login-and-persistence' else 1

    def load_lab_module(self):
        sys.path.insert(0, str(self.xiri / 'scripts'))
        from xiri_lab import load_probe
        module = load_probe()
        module.TOOLBOX = []
        module.IRIS = self.emulator / 'iris'
        module.CI = self.emulator / 'iris-ci'
        return module


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('xiri', 'emulator', 'image', 'nvram', 'flash', 'disk', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--timeout', type=int, default=240)
    args = parser.parse_args()
    if args.output.exists():
        parser.error('--output must not exist')
    return MultiUserProbe(args).run()


if __name__ == '__main__':
    sys.exit(main())
