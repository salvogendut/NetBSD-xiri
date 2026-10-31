#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""V1 mount-guard fixtures. IRIX headers are synthetic, never repaired images."""
import argparse
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

from test_images import digest, require, run, values
from test_log import compare, journal


def record(block, cycle, *, clean=True, old_string=False):
    payload = (b"Unmount filesystem--" if old_string else b"Un" + bytes(6)) if clean else b""
    body = struct.pack(">IIBBH", 0x12345678, len(payload), 0xaa if clean else 0x69,
                       0x20 if clean else 2, 0) + payload
    header = bytearray(512)
    struct.pack_into(">IIIIQQIII", header, 0, 0xfeedbabe, cycle, 1, len(body),
                     cycle << 32 | block, cycle << 32 | block, 0, 0xffffffff, 1)
    header[44:48] = body[:4]
    return header + struct.pack(">I", cycle) + body[4:] + bytes(512 - len(body))


def exercise(probe, root):
    root.mkdir()  # Refuse to overwrite or mutate any pre-existing image.
    source = root / "contents"
    content = bytes(range(256)) * 17 + b"IRIX journal fixture\n"
    source.write_bytes(content)
    proto = root / "prototype"
    proto.write_text(f"dummy\n0 0\nd--755 0 0\nhello ---644 12 34 {source}\n"
                     f"link l--777 0 0 hello\nsub d--755 12 34\nchild ---644 12 34 {source}\n$\n$\n")
    initial = root / "mkfs-v1.img"
    with initial.open("xb") as stream:
        stream.truncate(400 * 1024 * 1024)
    run("mkfs.xfs", "-q", "-f", "-K", "-m", "crc=0", "-l", "version=1",
        "-n", "ftype=0,parent=0", "-p", str(proto), str(initial))
    # mkfs copies source labels. Remove them only inside this disposable
    # fixture, leaving host labels and the server's security policy intact.
    if "security.selinux" in os.listxattr(source):
        for line in run("xfs_db", "-r", "-c", "blockget -n", "-c", "ncheck", str(initial)).splitlines():
            number, name = line.split(maxsplit=1)
            if name in ("hello", "sub/child"):
                run("xfs_db", "-x", "-c", f"inode {int(number)}", "-c",
                    "attr_remove -s selinux", str(initial))
    sb = values(run("xfs_db", "-r", "-c", "sb 0", "-c",
                    "p logstart logblocks blocksize agblocks agblklog", str(initial)))
    base = ((sb["logstart"] >> sb["agblklog"]) * sb["agblocks"] +
            (sb["logstart"] & ((1 << sb["agblklog"]) - 1))) * sb["blocksize"]
    logbytes = sb["logblocks"] * sb["blocksize"]
    blocks = logbytes // 512
    expected = {"mkfs-v1": ("clean", 2, 2, False)}
    variants = ("irix-modern", "irix-string", "irix-wrapped", "irix-dirty",
                "irix-torn", "irix-bad-unmount", "irix-bad-frame", "irix-identity")
    for name in variants:
        path = root / (name + ".img")
        # Preserve holes; no source image comes from a user or a live mount.
        subprocess.run(["cp", "--sparse=always", str(initial), str(path)], check=True)
        # The initial log is zero beyond its first two blocks. Keep those
        # holes in variants which do not need cycle stamps across the ring.
        log = bytearray(logbytes if name == "irix-wrapped" else 2048)
        if name == "irix-wrapped":
            log[:] = (struct.pack(">I", 7) + bytes(508)) * blocks
            rec = record(blocks - 1, 7, old_string=True)
            struct.pack_into(">I", rec, 512, 8)
            log[-512:], log[:512] = rec[:512], rec[512:]
            expected[name] = ("clean", 1, 1, True)
        else:
            log[:1024] = record(0, 1, old_string=name == "irix-string")
            expected[name] = ("clean", 2, 2, True)
            if name in ("irix-dirty", "irix-bad-frame", "irix-torn"):
                log[1024:2048] = record(2, 1, clean=False)
                expected[name] = ("needs-recovery", 4, 2, True)
                if name == "irix-bad-frame":
                    struct.pack_into(">I", log, 1024 + 40, 2)
                elif name == "irix-torn":
                    struct.pack_into(">I", log, 1024 + 12, 516)
            elif name == "irix-bad-unmount":
                log[512 + 12] ^= 1
            elif name == "irix-identity":
                log[304] = 1
        with path.open("r+b") as stream:
            stream.seek(base)
            stream.write(log)
    hashes = {}
    for name, (state, head, tail, legacy) in expected.items():
        path = root / (name + ".img")
        before = digest(path)
        if name in ("irix-torn", "irix-bad-unmount", "irix-bad-frame", "irix-identity"):
            for args in (("-j",), ("-p", "/"), ("-l", "/"), ("-R", "/hello", "0", "20")):
                result = subprocess.run([probe, *args, str(path)], capture_output=True)
                require(result.returncode == 1 and not result.stdout, f"{name}: malformed journal accepted")
        else:
            info = compare(probe, path) if state == "clean" else journal(probe, path)
            require(info["state"] == state and int(info["head"]) == head and int(info["tail"]) == tail and
                    info["version"] == "1" and int(info["legacy"]) == legacy and info["checksummed"] == "0", str(info))
            if state == "clean":
                for entry in ("/", "/hello", "/sub", "/sub/child", "/link"):
                    run(probe, "-X", entry, str(path))
                require(subprocess.check_output([probe, "-R", "/hello", "0", str(len(content) + 1), str(path)]) == content, name)
                require(subprocess.check_output([probe, "-R", "/sub/child", "0", str(len(content)), str(path)]) == content, name)
                require(subprocess.check_output([probe, "-s", "/link", str(path)]) == b"hello", name)
                listing = [json.loads(line)["name"] for line in run(probe, "-l", "/", str(path)).splitlines()]
                require(set(listing) == {".", "..", "hello", "sub", "link"} and len(listing) == 5, name)
            else:
                for args in (("-p", "/"), ("-l", "/"), ("-R", "/hello", "0", "20"), ("-X", "/hello")):
                    result = subprocess.run([probe, *args, str(path)], capture_output=True)
                    require(result.returncode == 1 and not result.stdout and b"needs recovery" in result.stderr, name)
        require(digest(path) == before, f"{name}: image changed")
        hashes[path.name] = before.hex()
        print(f"Passed v1 fixture {name}: {'rejected' if name in variants[4:] else state}; image unchanged")
    (root / "hashes.json").write_text(json.dumps(hashes, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("probe", type=Path)
    parser.add_argument("--fixtures", type=Path, help="retain fixtures in this new directory for native tests")
    args = parser.parse_args()
    probe = str(args.probe.resolve())
    if args.fixtures:
        exercise(probe, args.fixtures.resolve())
    else:
        with tempfile.TemporaryDirectory(prefix="netbsd-xfs-v1-") as directory:
            exercise(probe, Path(directory) / "fixtures")


if __name__ == "__main__":
    main()
