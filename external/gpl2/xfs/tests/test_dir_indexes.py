# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Check external directory indexes on disposable, unmounted mkfs images."""
from pathlib import Path
import json
import re
import struct
import subprocess
import sys
import tempfile

from test_images import digest, metadata_crc, require, run, values


def directory_block(image, path, logical, sb):
    """Return a directory block and physical pieces, including fragmented maps."""
    bs = sb["blocksize"]
    blocks = 1 << sb["dirblklog"]
    aglog = (sb["agblocks"] - 1).bit_length()
    maps = [tuple(map(int, match)) for match in re.findall(
        r"data offset (\d+) startblock (\d+).*?count (\d+)",
        run("xfs_db", "-r", "-c", f"path {path}", "-c", "bmap", str(image)))]
    pieces = []
    data = bytearray()
    with image.open("rb") as stream:
        for at in range(logical, logical + blocks):
            matching = [(start, fsb) for start, fsb, count in maps if start <= at < start + count]
            require(len(matching) == 1, "missing or overlapping directory fixture mapping")
            start, fsb = matching[0]
            fsb += at - start
            offset = ((fsb >> aglog) * sb["agblocks"] + (fsb & ((1 << aglog) - 1))) * bs
            pieces.append(offset)
            stream.seek(offset)
            data.extend(stream.read(bs))
    require(len(data) == bs * blocks, "short directory fixture read")
    return data, pieces


def write_block(image, data, pieces, bs):
    with image.open("r+b") as stream:
        for i, offset in enumerate(pieces):
            stream.seek(offset)
            stream.write(data[i * bs:(i + 1) * bs])


def corrupt_node_fixture(image):
    """Break the /many root's first child while retaining a valid v5 checksum."""
    sb = values(run("xfs_db", "-r", "-c", "sb 0", "-c", "p blocksize agblocks dirblklog", str(image)))
    data, pieces = directory_block(image, "/many", (1 << 35) // sb["blocksize"], sb)
    require(struct.unpack_from(">H", data, 8)[0] == 0x3ebe, "fixture /many must have a v5 node root")
    struct.pack_into(">I", data, 68, 0)
    metadata_crc(data, 12)
    write_block(image, data, pieces, sb["blocksize"])


def main():
    probe = str(Path(sys.argv[1]).resolve())
    corruptions = 0
    with tempfile.TemporaryDirectory(prefix="netbsd-xfs-dir-index-") as directory:
        root = Path(directory)
        payload = root / "payload"
        payload.write_bytes(b"directory index payload\n")
        proto = root / "prototype"
        lines = ["dummy", "0 0", "d--755 0 0"]
        for name, count in (("leaf", 300), ("node", 1800)):
            lines.append(f"{name} d--755 0 0")
            lines.extend(f"f{i:04d} ---644 0 0 {payload}" for i in range(count))
            lines.extend(f"{entry} ---644 0 0 {payload}" for entry in ("aaaaa", "qaaa`"))
            lines.append("$")
        proto.write_text("\n".join(lines + ["$", ""]))
        variants = ((4, 4096, 4096), (5, 4096, 4096), (5, 4096, 8192), (5, 1024, 4096))
        for version, bs, ds in variants:
            image = root / f"v{version}-{bs}-{ds}.img"
            with image.open("xb") as stream:
                stream.truncate(400 * 1024 * 1024)
            run("mkfs.xfs", "-q", "-f", "-K", "-m", f"crc={int(version == 5)}",
                "-b", f"size={bs}", "-n", f"size={ds}", "-p", str(proto), str(image))
            sb = values(run("xfs_db", "-r", "-c", "sb 0", "-c", "p blocksize agblocks dirblklog", str(image)))
            before = digest(image)
            for name, count in (("leaf", 300), ("node", 1800)):
                entries = [json.loads(line) for line in run(probe, "-l", f"/{name}", str(image)).splitlines()]
                require(len(entries) == count + 4, "wrong indexed directory listing length")
                for target in ("aaaaa", "qaaa`", "f0000", f"f{count - 1:04d}"):
                    result = subprocess.run((probe, "-R", f"/{name}/{target}", "0", "99", str(image)),
                                            capture_output=True, check=True)
                    require(result.stdout == payload.read_bytes(), "indexed lookup selected wrong data")
                result = subprocess.run((probe, "-p", f"/{name}/absent", str(image)), capture_output=True)
                require(result.returncode == 1 and b"not found" in result.stderr, "missing name did not return ENOENT")

                logical = (1 << 35) // bs
                data, pieces = directory_block(image, f"/{name}", logical, sb)
                magic = struct.unpack_from(">H", data, 8)[0]
                h, info = (64, 56) if version == 5 else (16, 12)
                require(magic in (0xd2f1, 0x3df1, 0xfebe, 0x3ebe), "unexpected fixture index root")
                targets = [(logical, data, pieces, False)]
                if magic in (0xfebe, 0x3ebe):
                    count = struct.unpack_from(">H", data, info)[0]
                    last = struct.unpack_from(">I", data, h + (count - 1) * 8 + 4)[0]
                    targets.append((last, *directory_block(image, f"/{name}", last, sb), False))
                    free = (2 << 35) // bs
                    targets.append((free, *directory_block(image, f"/{name}", free, sb), True))
                for _, original, pieces, freeblock in targets:
                    tests = [(0 if freeblock else 8, False), (h if freeblock else h + 4, True),
                             ((52 if version == 5 else 8) if freeblock else info, True)]
                    if version == 5:
                        tests += [(500, False), (8 if freeblock else 16, True),
                                  (24 if freeblock else 32, True), (40 if freeblock else 48, True)]
                    for offset, update in tests:
                        changed = bytearray(original)
                        changed[offset] ^= 0x80
                        if update and version == 5:
                            metadata_crc(changed, 4 if freeblock else 12)
                        write_block(image, changed, pieces, bs)
                        try:
                            for command, path in (("-p", f"/{name}/f0000"), ("-l", f"/{name}")):
                                result = subprocess.run((probe, command, path, str(image)), capture_output=True)
                                require(result.returncode == 1 and not result.stdout and b"directory" in result.stderr,
                                        f"missed index corruption v{version}/{bs}/{ds}: {result.stderr!r}")
                            corruptions += 1
                        finally:
                            write_block(image, original, pieces, bs)
            require(before == digest(image), "directory-index test changed image")
            print(f"PASS directory indexes v{version}, {bs}-byte blocks, {ds}-byte directory blocks")
    print(f"Passed 4 directory-index image configurations and {corruptions} corruption cases")


if __name__ == "__main__":
    main()
