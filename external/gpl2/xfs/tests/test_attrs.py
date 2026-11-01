# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Read real xfsprogs attribute trees at 1K/4K/64K block sizes."""
from pathlib import Path
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

from make_puffs_images import external_attributes
from test_images import digest, metadata_crc, require, run, values


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_attrs.py xfs_probe")
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="netbsd-xfs-attrs-") as directory:
        root = Path(directory)
        source = root / "source"
        source.write_bytes(b"attribute fixture\n")
        proto = root / "prototype"
        proto.write_text(f"dummy\n0 0\nd--755 0 0\nfile ---644 0 0 {source}\n$\n")
        for version in (4, 5):
            for bs in (1024, 4096, 65536):
                image = root / f"v{version}-{bs}.img"
                with image.open("xb") as f:
                    # Large-block v5 needs room for xfsprogs' minimum log.
                    f.truncate((1024 if bs == 65536 else 400) * 1024 * 1024)
                run("mkfs.xfs", "-q", "-f", "-K", "-n", "parent=0", "-b", f"size={bs}",
                    "-m", f"crc={int(version == 5)}", "-p", str(proto), str(image))
                names = run("xfs_db", "-r", "-c", "blockget -n", "-c", "ncheck", str(image))
                ino = int(re.search(r"(\d+) file$", names, re.M)[1])
                if "security.selinux" in os.listxattr(source):
                    run("xfs_db", "-x", "-c", f"inode {ino}", "-c", "attr_remove -s selinux", str(image))
                external_attributes(image, ino)
                sb = values(run("xfs_db", "-r", "-c", "sb 0", "-c", "p agblocks agblklog", str(image)))
                mappings = run("xfs_db", "-r", "-c", f"inode {ino}", "-c", "bmap -a", str(image))
                extents = [tuple(map(int, m)) for m in re.findall(
                    r"attr offset (\d+) startblock (\d+) \([^)]*\) count (\d+)", mappings)]

                def physical(logical):
                    for start, fsb, length in extents:
                        if start <= logical < start + length:
                            fsb += logical - start
                            return ((fsb >> sb['agblklog']) * sb['agblocks'] +
                                    (fsb & ((1 << sb['agblklog']) - 1))) * bs
                    raise RuntimeError(f"unmapped attribute block {logical}")

                def inspect(expected, raw=False):
                    before = digest(image)
                    rows = [json.loads(row) for row in run(probe, *(["--raw"] if raw else []),
                                                          "-X", "/file", str(image)).splitlines()]
                    actual = {bytes.fromhex(row['name']): bytes.fromhex(row['value']) for row in rows}
                    require(len(rows) == len(expected) and all(row['flags'] == 0 for row in rows), "count/namespace")
                    require(actual == expected, f"v{version}/{bs}: attribute bytes disagree")
                    require(digest(image) == before, "reader modified image")

                expected = {b"big": b"B" * 8192, b"max": b"M" * 65536}
                expected.update({f"n{i:03d}".encode(): b"N" * 1000 for i in range(100)})
                inspect(expected)
                # Corrupt a later tree leaf, with a fresh v5 CRC: no partial
                # attribute output may escape before that damage is found.
                with image.open("r+b") as f:
                    f.seek(physical(0))
                    node = f.read(bs)
                    require(struct.unpack_from(">H", node, 8)[0] == (0x3ebe if version == 5 else 0xfebe),
                            "fixture must have a node root")
                    info, header = (56, 64) if version == 5 else (12, 16)
                    count = struct.unpack_from(">H", node, info)[0]
                    child = struct.unpack_from(">I", node, header + (count - 1) * 8 + 4)[0]
                    where = physical(child)
                    f.seek(where)
                    original = f.read(bs)
                    damaged = bytearray(original)
                    struct.pack_into(">H", damaged, info + 2, 0)
                    if version == 5:
                        metadata_crc(damaged, 12)
                    f.seek(where); f.write(damaged); f.flush()
                    result = subprocess.run([probe, "-x", "/file", str(image)], capture_output=True)
                    require(result.returncode == 1 and not result.stdout and b"usedbytes" in result.stderr,
                            f"corrupt leaf exposed data: {result.stderr}")
                    f.seek(where); f.write(original)
                run("xfs_db", "-x", "-c", f"inode {ino}", "-c", "attr_set -r SGI_ACL_FILE denied", str(image))
                result = subprocess.run([probe, "-X", "/file", str(image)], capture_output=True)
                require(result.returncode == 1 and not result.stdout and b"ACL" in result.stderr,
                        "malformed external ACL was accepted")
                commands = [f"inode {ino}", "attr_remove -r SGI_ACL_FILE"]
                commands += [f"attr_remove -u n{i:03d}" for i in range(0, 100, 2)]
                subprocess.run(["xfs_db", "-x", str(image)], input="\n".join(commands) + "\n",
                               text=True, capture_output=True, check=True)
                for i in range(0, 100, 2):
                    del expected[f"n{i:03d}".encode()]
                inspect(expected)
                if version == 5:
                    # xfs_admin also resets the journal. Use the inspector's
                    # explicit analysis mode for this separate metadata-UUID
                    # test; all preceding reads require a clean journal.
                    run("xfs_admin", "-U", "generate", str(image))
                    inspect(expected, raw=True)
                print(f"Passed v{version}/{bs}: external tree, remote values, deletion holes, corruption and ACL policy")


if __name__ == "__main__":
    main()
