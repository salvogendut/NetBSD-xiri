#!/usr/bin/env python3
"""Bounded O2/Octane boot test using the sibling Xiri lab harness.

Every run copies mutable firmware state to a new directory and stops its
own emulator. Runs are diskless unless --disk supplies a private CHD copy;
input hashes are checked after cleanup.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import signal
import struct
import subprocess
import sys
import time
from types import SimpleNamespace


def sha256(path):
    with Path(path).open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def elf_entry(path):
    """Return e_entry from an ELF32/ELF64 image in either byte order."""
    with Path(path).open('rb') as stream:
        ident = stream.read(16)
        if len(ident) != 16 or ident[:4] != b'\x7fELF':
            raise ValueError('not an ELF image')
        endian = {1: '<', 2: '>'}.get(ident[5])
        if endian is None:
            raise ValueError('unknown ELF byte order')
        stream.seek(24)
        if ident[4] == 2:
            raw = stream.read(8)
            if len(raw) != 8:
                raise ValueError('truncated ELF64 header')
            return struct.unpack(endian + 'Q', raw)[0]
        if ident[4] == 1:
            raw = stream.read(4)
            if len(raw) != 4:
                raise ValueError('truncated ELF32 header')
            return struct.unpack(endian + 'I', raw)[0]
        raise ValueError('unknown ELF class')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('xiri', 'emulator', 'image', 'nvram', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--machine', choices=('o2', 'octane'), required=True)
    parser.add_argument('--disk', type=Path, help='copy a stopped CHD into the run directory')
    parser.add_argument('--single-user', action='store_true', help='O2 native shell/identity and clean-halt check')
    parser.add_argument('--kernel-entry', action='store_true', help='Octane kernel-entry checkpoint; no PROM return required')
    parser.add_argument('--flash', type=Path, help='Octane flash, copied before use')
    parser.add_argument('--expect', required=True, help='literal serial success marker')
    parser.add_argument('--console-input', help='Octane console input to send after the marker')
    parser.add_argument('--return-to-prom', action='store_true')
    parser.add_argument('--timeout', type=int, default=300)
    args = parser.parse_args()
    if not args.expect or args.timeout < 1 or (args.machine == 'octane' and args.flash is None):
        parser.error('nonempty marker, positive timeout and Octane flash image required')
    if args.single_user and (args.machine != 'o2' or args.disk is None):
        parser.error('--single-user requires O2 and --disk')
    if args.kernel_entry and (args.machine != 'octane' or args.return_to_prom or args.single_user):
        parser.error('--kernel-entry requires Octane and excludes --return-to-prom/--single-user')
    out, image, emu, nvram = [getattr(args, n).resolve() for n in
                              ('output', 'image', 'emulator', 'nvram')]
    flash = args.flash.resolve() if args.flash else None
    if args.console_input and args.machine != 'octane':
        parser.error('--console-input requires Octane')
    if not re.fullmatch(r'[A-Za-z0-9_.-]+', image.name):
        parser.error('image filename must use letters, digits, dot, dash or underscore')
    disk = args.disk.resolve() if args.disk else None
    if disk and not disk.is_file():
        parser.error('--disk must be a regular, stopped CHD file')
    inputs = [image, nvram, emu/'iris', emu/'iris-ci'] + ([flash] if flash else []) + ([disk] if disk else [])
    hashes = {str(p): sha256(p) for p in inputs}
    expected_entry = None
    if args.kernel_entry:
        try:
            expected_entry = elf_entry(image)
        except (OSError, ValueError) as error:
            parser.error('--kernel-entry needs a readable ELF image: ' + str(error))
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(args.xiri.resolve()/'scripts'))
    from xiri_lab import load_probe
    probe = load_probe()
    probe.TOOLBOX = []
    probe.IRIS, probe.CI = emu/'iris', emu/'iris-ci'
    out.mkdir(parents=True, exist_ok=False)
    shutil.copyfile(__file__, out/'probe-used.py')
    shutil.copyfile(nvram, out/'nvram.bin')
    if disk:
        subprocess.run(['cp', '--reflink=auto', '--sparse=always', '--', str(disk), str(out/'disk.chd')], check=True)
    if flash:
        shutil.copyfile(flash, out/'flash.bin')
    is_o2 = args.machine == 'o2'
    config = f'''nvram = "nvram.bin"
banks = [{256 if is_o2 else 512}, 0, 0, 0]
headless = true
no_audio = true
ci = true
{"scsi = {}" if disk is None else ""}
nat_subnet = "192.168.231.0/24"
[machine]
profile = "{'o2_ip32' if is_o2 else 'octane_ip30'}"
cpu = "r10000"
cpus = 1
'''
    if not is_o2:
        config += '[octane]\nflash = "flash.bin"\n[graphics]\nboard = "none"\n'
    if disk:
        config += '[scsi.1]\npath = ' + json.dumps(str(out/'disk.chd')) + '\ncdrom = false\noverlay = true\n'
    config += '[network]\nmode = "nat"\ntftp_dir = ' + json.dumps(str(image.parent)) + '\n'
    lab = probe.Lab(SimpleNamespace(mode='ci', debugger=False, boot_timeout=args.timeout),
                    out, config_text=config, working_directory=out)
    launch = lab.launch

    def configured_launch(command, name):
        if name == 'emulator':
            command = list(command)
            i = command.index('--scsi1')
            del command[i:i + 2]
        return launch(command, name)

    lab.launch = configured_launch
    result = dict(schema=1, machine=args.machine, status='incomplete',
                  inputs=hashes, attached_disks=[str(out/'disk.chd')] if disk else [], commands=[],
                  harness_sha256=sha256(Path(probe.__file__)),
                  expect=args.expect, return_to_prom=args.return_to_prom, single_user=args.single_user,
                  console_input=args.console_input, console_echo=None,
                  kernel_entry=args.kernel_entry,
                  expected_entry=None if expected_entry is None else hex(expected_entry))
    prompt = r'(?:^|\n)> ' if is_o2 else r'(?:^|\n)>> '

    def interrupted(signum, frame):
        raise RuntimeError('Interrupted by signal ' + str(signum))

    signal.signal(signal.SIGTERM, interrupted)
    started = time.monotonic()
    try:
        lab.start(boot=False)
        menu = lab.wait(r'Option\?|Press any key to continue', 120)
        if 'Press any key' in menu[0]:
            lab.send('')
            lab.wait(r'Option\?', 60)
        lab.send('5')
        lab.wait(prompt, 30)
        for command in ('setenv console d', 'setenv AutoLoad No'):
            result['commands'].append(command)
            lab.send(command)
            lab.wait(prompt, 30)
        print(args.machine + ': PROM ready; loading ' + image.name, flush=True)
        command = 'boot -f bootp():' + image.name + (' --s' if is_o2 else '')
        result['commands'].append(command)
        lab.send(command)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline:
            serial = lab.serial_text()[lab.offset:]
            if re.search(r'panic:|PANIC:|Stopped in pid|Exception:|Unable to execute|Error loading|XIRI_IP30_M2B_HEART_FAILED|XIRI_IP30_M2B_CLOCK_FAILED|XIRI_IP30_M2B_INTR_FAILED|XIRI_IP30_M3B3_FAILED', serial):
                result['status'] = 'boot-blocked'
                break
            if args.expect in serial:
                suffix = serial.split(args.expect, 1)[1]
                if not args.return_to_prom or re.search(prompt, suffix):
                    result['status'] = 'passed'
                    break
            if lab.process.poll() is not None:
                result['status'] = 'emulator-exited'
                break
            time.sleep(0.1)
        else:
            result['status'] = 'timeout'
        if args.console_input and result['status'] == 'passed':
            lab.wait(r'root device: ', 10)
            start = lab.offset = len(lab.serial_text())
            lab.send(args.console_input)
            lab.wait(re.escape(args.console_input), 10)
            echo = lab.serial_text()[start:]
            result['console_echo'] = args.console_input in echo
            result['console_output'] = echo[-1200:]
            lab.wait(r'(?:use one of:|root device: )', 10)
        if args.kernel_entry and result['status'] == 'passed':
            serial = lab.serial_text()

            def checkpoint_field(name):
                match = re.search(r'(?m)^' + re.escape(name) +
                                  r'=(0x[0-9a-fA-F]+)', serial)
                return int(match.group(1), 16) if match is not None else None

            entry = checkpoint_field('kernel.entry')
            sp = checkpoint_field('kernel.sp')
            bss_start = checkpoint_field('kernel.bss')
            bss_end = checkpoint_field('kernel.bss.end')
            witness = checkpoint_field('kernel.bss.witness')
            stack_base = checkpoint_field('kernel.stack.base')
            stack_top = checkpoint_field('kernel.stack.top')
            errors = []
            if entry is None:
                errors.append('missing kernel.entry')
            elif entry != expected_entry:
                errors.append('kernel.entry %#x != ELF e_entry %#x' %
                              (entry, expected_entry))
            if sp is None:
                errors.append('missing kernel.sp')
            elif (stack_base is None or stack_top is None or
                  not stack_base < stack_top):
                errors.append('missing or invalid kernel stack range')
            elif not stack_base <= sp < stack_top:
                errors.append('kernel.sp %#x outside [%#x, %#x)' %
                              (sp, stack_base, stack_top))
            if bss_start is None or bss_end is None or not bss_start < bss_end:
                errors.append('missing or invalid kernel BSS range')
            if witness is None:
                errors.append('missing kernel.bss.witness')
            elif witness != 0:
                errors.append('kernel.bss.witness %#x is not zero' % witness)
            result['checkpoint'] = {
                'entry': None if entry is None else hex(entry),
                'sp': None if sp is None else hex(sp),
                'bss_start': None if bss_start is None else hex(bss_start),
                'bss_end': None if bss_end is None else hex(bss_end),
                'bss_witness': None if witness is None else hex(witness),
                'stack_base': None if stack_base is None else hex(stack_base),
                'stack_top': None if stack_top is None else hex(stack_top),
            }
            if entry is not None:
                result['kernel_entry_address'] = hex(entry)
            if errors:
                result['status'] = 'entry-check-failed'
                result['error'] = '; '.join(errors)
        if result['status'] == 'passed' and args.single_user:
            # Enter the native shell before any multi-user/vendor startup.
            lab.wait(r'Enter pathname of shell', 30)
            lab.send('')
            lab.wait(r'# ', 30)
            lab.send("export PS1='xiri-indy# '")
            lab.wait(r'xiri-indy# ', 30)
            lab.logged_in = True
            result['commands'].append('uname -a && /sbin/mount && /bin/echo XIRI_NATIVE_SHELL')
            if lab.command(result['commands'][-1], 30) != 0:
                raise RuntimeError('native shell commands failed')
        lab.monitor_command('stop')
        commands = ['regs', 'cop0', 'mem 0xffffffff80000000 48']
        commands += (['scsi regs', 'scsi1 regs'] if is_o2 else
                     ['ip30 memcfg', 'mem 0xffffffffa0001000 20 d'])
        if args.kernel_entry and result['status'] == 'passed':
            commands.append('mem ' + result['kernel_entry_address'] + ' 32')
        result['monitor'] = {cmd: lab.monitor_command(cmd) for cmd in commands}
    except (Exception, KeyboardInterrupt) as error:
        result.update(status='probe-error', error=str(error))
    finally:
        try:
            lab.close()
        except Exception as error:
            result.update(status='cleanup-error', cleanup_error=str(error))
        result['clean_shutdown'] = lab.clean_shutdown
        if args.single_user and result['status'] == 'passed' and not lab.clean_shutdown:
            result['status'] = 'cleanup-error'
        result['emulator_stopped'] = lab.process is None or lab.process.poll() is not None
        result['originals_unchanged'] = all(sha256(p) == h for p, h in hashes.items())
        if not result['emulator_stopped'] or not result['originals_unchanged']:
            result['status'] = 'cleanup-error'
        result['elapsed_seconds'] = round(time.monotonic() - started, 2)
        (out/'result.json').write_text(json.dumps(result, indent=2) + '\n')
    print(args.machine + ': ' + result['status'], flush=True)
    return 0 if result['status'] == 'passed' else 1


if __name__ == '__main__':
    sys.exit(main())
