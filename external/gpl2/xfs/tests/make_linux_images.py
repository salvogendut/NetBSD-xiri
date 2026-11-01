#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Create real Linux XFS fixtures in disposable, networkless QEMU guests.

Needs Linux x86-64, cc, ldd, sha256sum, mkfs.xfs, and qemu-system-x86_64.
The kernel must support initramfs, devtmpfs, virtio block and XFS with ACLs.
Only freshly created images are attached; no host filesystem is mounted.
"""
import argparse
import gzip
import hashlib
import json
import lzma
import os
from pathlib import Path
import re
import selectors
import shutil
import stat
import subprocess
import time


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def digest(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def initramfs(output, helper, module_dir):
    files = {}
    provenance = {}

    def add(path, source):
        source = Path(source)
        files[path.lstrip("/")] = (source.read_bytes(), stat.S_IFREG | 0o755, 0, 0)
        provenance[str(source.resolve())] = digest(source)

    for destination, source in (("init", helper), ("sha256sum", shutil.which("sha256sum"))):
        add(destination, source)
        for dependency in re.findall(r"(/[^\s()]+)", run("ldd", str(source))):
            add(dependency, dependency)

    dependencies = {}
    for line in (module_dir / "modules.dep").read_text().splitlines():
        module, deps = line.split(":", 1)
        dependencies[module] = deps.split()
    builtins = (module_dir / "modules.builtin").read_text().splitlines()
    loaded = []

    def load(module):
        if module in loaded:
            return
        for dependency in dependencies[module]:
            load(dependency)
        source = module_dir / module
        data = source.read_bytes()
        if module.endswith(".xz"):
            data = lzma.decompress(data)
        elif module.endswith(".gz"):
            data = gzip.decompress(data)
        elif module.endswith(".zst"):
            data = subprocess.check_output(["zstd", "-dc", str(source)])
        files[f"module-{len(loaded)}.ko"] = (data, stat.S_IFREG | 0o644, 0, 0)
        provenance[str(source.resolve())] = digest(source)
        loaded.append(module)

    for name in ("virtio_pci", "virtio_blk", "xfs"):
        candidates = [m for m in dependencies if Path(m).name.split(".")[0] == name]
        if candidates:
            load(candidates[0])
        elif not any(Path(m).stem == name for m in builtins):
            raise RuntimeError(f"missing module or builtin: {name}")
    files["modules.list"] = ("".join(f"/module-{i}.ko\n" for i in range(len(loaded))).encode(), stat.S_IFREG | 0o644, 0, 0)
    files["dev/console"] = (b"", stat.S_IFCHR | 0o600, 5, 1)
    directories = {"proc", "sys", "dev", "mnt", "tmp"}
    for path in files:
        directories.update(str(p) for p in Path(path).parents if str(p) != ".")
    entries = [(d, (b"", stat.S_IFDIR | 0o755, 0, 0)) for d in sorted(directories, key=lambda d: (d.count("/"), d))]
    entries += list(files.items()) + [("TRAILER!!!", (b"", 0, 0, 0))]
    archive = bytearray()
    for ino, (name, (data, mode, major, minor)) in enumerate(entries, 1):
        encoded = name.encode() + b"\0"
        fields = (ino, mode, 0, 0, 1, 0, len(data), 0, 0, major, minor, len(encoded), 0)
        archive.extend(("070701" + "".join(f"{v:08x}" for v in fields)).encode())
        archive.extend(encoded)
        archive.extend(b"\0" * (-len(archive) % 4))
        archive.extend(data)
        archive.extend(b"\0" * (-len(archive) % 4))
    output.write_bytes(gzip.compress(archive, mtime=0))
    return provenance


def guest(directory, image, phase, timeout):
    command = ["qemu-system-x86_64", "-accel", "tcg", "-cpu", "max", "-m", "1024", "-smp", "2",
               "-kernel", str(directory / "vmlinuz"), "-initrd", str(directory / "initramfs.gz"),
               "-append", f"console=ttyS0 rdinit=/init panic=1 selinux=0 xfs_fixture={phase}",
               "-drive", f"file={image},format=raw,if=virtio,cache=directsync",
               "-nographic", "-monitor", "none", "-nic", "none", "-no-reboot"]
    # QEMU's key/value drive parser treats commas specially.
    if "," in str(image):
        raise ValueError("output directory must not contain commas")
    references, pending = [], b""
    marker = "XFS_WORKLOAD_DIRTY_READY" if phase == "dirty" else "XFS_WORKLOAD_CLEAN"
    complete = False
    deadline = time.monotonic() + timeout
    with (directory / f"{image.stem}.log").open("wb") as log:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
        try:
            with selectors.DefaultSelector() as selector:
                selector.register(process.stdout, selectors.EVENT_READ)
                while True:
                    if time.monotonic() > deadline:
                        raise TimeoutError(f"{image.name}: guest timeout; inspect serial log")
                    if not selector.select(1):
                        continue
                    chunk = os.read(process.stdout.fileno(), 65536)
                    if not chunk:
                        break
                    log.write(chunk)
                    log.flush()
                    pending += chunk
                    while b"\n" in pending:
                        line, pending = pending.split(b"\n", 1)
                        text = line.decode(errors="replace").strip()
                        if text.startswith("XFSREF "):
                            references.append(json.loads(text[7:]))
                        if text.startswith("XFS_WORKLOAD_ERROR"):
                            raise RuntimeError(text)
                        if text == marker:
                            complete = True
                    if complete and phase == "dirty":
                        # fsync has completed. Do not give Linux an unmount/shutdown.
                        process.kill()
                        break
            status = process.wait(timeout=20)
            if not complete or (phase == "clean" and status != 0):
                raise RuntimeError(f"{image.name}: missing {marker} or bad QEMU status {status}")
        finally:
            if process.poll() is None:
                process.kill()
                process.wait()
            process.stdout.close()
    return references


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--modules", type=Path, required=True)
    parser.add_argument("--timeout", type=int, default=300)
    parser.add_argument("--v1", action="store_true", help="also capture a v4 filesystem with a v1 journal")
    parser.add_argument("directory", type=Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    directory.mkdir()  # Never overwrite or attach an existing image.
    source = Path(__file__).with_name("linux_workload.c")
    helper = directory / "linux_workload"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(helper)], check=True)
    shutil.copyfile(args.kernel, directory / "vmlinuz")
    provenance = {"kernel_sha256": digest(args.kernel), "kernel_source": str(args.kernel.resolve()),
                  "helper_source_sha256": digest(source), "mkfs_version": run("mkfs.xfs", "-V").strip(),
                  "qemu_version": run("qemu-system-x86_64", "--version").splitlines()[0],
                  "initramfs_inputs": initramfs(directory / "initramfs.gz", helper, args.modules.resolve())}
    for version in ("4", "5", "4-log1") if args.v1 else ("4", "5"):
        clean = directory / f"linux-v{version}.img"
        dirty = directory / f"linux-v{version}-dirty.img"
        with clean.open("xb") as stream:
            stream.truncate(400 * 1024 * 1024)
        log_options = ["-l", "version=1"] if version == "4-log1" else []
        mkfs = ["mkfs.xfs", "-q", "-f", "-K", "-n", "parent=0", "-m", f"crc={int(version == '5')}", *log_options, str(clean)]
        provenance[f"v{version}_mkfs"] = mkfs
        subprocess.run(mkfs, check=True)
        print(f"Linux v{version}: generating clean workload", flush=True)
        references = guest(directory, clean, "clean", args.timeout)
        (directory / f"linux-v{version}.json").write_text(json.dumps(references, indent=2) + "\n")
        subprocess.run(["cp", "--sparse=always", "--reflink=auto", str(clean), str(dirty)], check=True)
        print(f"Linux v{version}: capturing forced shutdown", flush=True)
        guest(directory, dirty, "dirty", args.timeout)
        for path in (clean, dirty):
            provenance[path.name] = digest(path)
    provenance["initramfs_sha256"] = digest(directory / "initramfs.gz")
    (directory / "provenance.json").write_text(json.dumps(provenance, indent=2) + "\n")
    print(f"Linux fixtures and references: {directory}", flush=True)


if __name__ == "__main__":
    main()
