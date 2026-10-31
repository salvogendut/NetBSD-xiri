# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Compare the port with xfsprogs using disposable, unmounted image files."""

import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile


def run(*args):
    return subprocess.run(args, check=True, capture_output=True, text=True).stdout


def values(output):
    result = {}
    for line in output.splitlines():
        name, value = line.split(" = ")
        result[name] = int(value, 0)
    return result


def digest(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").digest()


def metadata_crc(data, offset):
    data[offset:offset + 4] = bytes(4)
    crc = 0xFFFFFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ (0x82F63B78 if crc & 1 else 0)
    struct.pack_into("<I", data, offset, crc ^ 0xFFFFFFFF)


def update_crc(data):
    length = struct.unpack_from(">H", data, 102)[0]
    sector = data[:length]
    metadata_crc(sector, 224)
    data[:length] = sector


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_images.py xfs_probe")
    for tool in ("mkfs.xfs", "xfs_db"):
        if shutil.which(tool) is None:
            raise SystemExit(f"--images requires {tool} from xfsprogs")
    probe = str(Path(sys.argv[1]).resolve())
    print(run("mkfs.xfs", "-V").strip())
    fields = (
        "blocksize dblocks agblocks agcount sectsize inodesize rootino "
        "logstart logblocks features_incompat features_ro_compat"
    )
    variants = (
        ("default", []),
        ("4k-sector", ["-s", "size=4096"]),
        ("64k-block", ["-b", "size=65536"]),
        ("large-inode", ["-i", "size=2048"]),
        ("short-last-ag", ["-d", "agsize=112m"]),
        ("no-optional-trees", ["-m", "reflink=0,rmapbt=0,finobt=0,inobtcount=0"]),
    )
    image_size = 400 * 1024 * 1024
    with tempfile.TemporaryDirectory(prefix="netbsd-xfs-images-") as directory:
        root = Path(directory)
        first_sector = None
        ag_comparisons = 0
        for name, options in variants:
            image = root / (name + ".img")
            with image.open("xb") as stream:
                stream.truncate(image_size)
            run("mkfs.xfs", "-q", "-f", "-K", *options, str(image))
            before = digest(image)
            actual = values(run(probe, str(image)))
            expected = values(run("xfs_db", "-r", "-c", "sb 0",
                                  "-c", "p versionnum " + fields, str(image)))
            expected["version"] = expected.pop("versionnum") & 15
            require(actual == expected, f"{name}: {actual} != {expected}")
            for agno in range(actual["agcount"]):
                actual_ag = values(run(probe, "-a", str(agno), str(image)))
                expected_ag = {}
                for kind, names in (("agf", "seqno length freeblks longest flcount"),
                                    ("agi", "seqno length count freecount")):
                    decoded = values(run("xfs_db", "-r", "-c", f"{kind} {agno}",
                                         "-c", "p " + names, str(image)))
                    expected_ag.update((kind + "." + k, v) for k, v in decoded.items())
                require(actual_ag == expected_ag,
                        f"{name} AG {agno}: {actual_ag} != {expected_ag}")
                ag_comparisons += 1
            require(before == digest(image), f"{name}: probe changed the image")
            print(f"PASS {name}: superblock and all AG headers match xfs_db; image unchanged")
            if first_sector is None:
                with image.open("rb") as stream:
                    first_sector = bytearray(stream.read(32768))

        def rejected(name, data, message, size=image_size):
            image = root / (name + ".img")
            with image.open("xb") as stream:
                stream.write(data)
                stream.truncate(size)
            result = subprocess.run((probe, str(image)), capture_output=True,
                                    text=True)
            require(result.returncode == 1 and message in result.stderr,
                    f"{name}: {result.returncode}: {result.stderr}")
            print(f"PASS {name}: rejected ({message})")

        corrupt = first_sector.copy()
        corrupt[500] ^= 1
        rejected("bad-crc", corrupt, "CRC32C")

        unknown = first_sector.copy()
        features = struct.unpack_from(">I", unknown, 216)[0]
        struct.pack_into(">I", unknown, 216, features | 0x80000000)
        update_crc(unknown)
        rejected("unknown-feature", unknown, "unknown incompatible")

        repair = first_sector.copy()
        struct.pack_into(">I", repair, 216, features | (1 << 4))
        update_crc(repair)
        rejected("needs-repair", repair, "needs repair")

        external = first_sector.copy()
        struct.pack_into(">Q", external, 48, 0)
        update_crc(external)
        rejected("external-log", external, "external logs")

        damaged = first_sector.copy()
        damaged[120] = 255
        update_crc(damaged)
        rejected("bad-block-shift", damaged, "block geometry")
        rejected("truncated-device", first_sector, "exceeds data device",
                 size=image_size - 1)
        rejected("short-sector", first_sector[:128], "truncated", size=128)

        # These sparse fixtures contain only AG0's headers. No mount is attempted.
        sector_size = struct.unpack_from(">H", first_sector, 102)[0]
        ag_rejections = 0

        def bad_ag(name, sector, crc_offset, field_offset, value, message,
                   repair_crc=True):
            nonlocal ag_rejections
            data = first_sector.copy()
            start = sector * sector_size
            header = data[start:start + sector_size]
            if isinstance(value, bytes):
                header[field_offset:field_offset + len(value)] = value
            else:
                struct.pack_into(">I", header, field_offset, value)
            if repair_crc:
                metadata_crc(header, crc_offset)
            data[start:start + sector_size] = header
            image = root / (name + ".img")
            with image.open("xb") as stream:
                stream.write(data)
                stream.truncate(image_size)
            result = subprocess.run((probe, "-a", "0", str(image)),
                                    capture_output=True, text=True)
            require(result.returncode == 1 and message in result.stderr,
                    f"{name}: {result.returncode}: {result.stderr}")
            ag_rejections += 1
            print(f"PASS {name}: rejected ({message})")

        for kind, sector, crc_offset, uuid_offset in (
                ("AGF", 1, 216, 64), ("AGI", 2, 312, 296), ("AGFL", 3, 32, 8)):
            bad_ag(kind + "-crc", sector, crc_offset, sector_size - 1, b"\x80",
                   "CRC32C", repair_crc=False)
            bad_ag(kind + "-uuid", sector, crc_offset, uuid_offset, bytes(16),
                   "UUID mismatch")
            bad_ag(kind + "-seqno", sector, crc_offset, 4 if kind == "AGFL" else 8,
                   1, "allocation group mismatch")
        bad_ag("AGF-root", 1, 216, 16, 0xFFFFFFFF, "free-space btree")
        bad_ag("AGI-root", 2, 312, 20, 0xFFFFFFFF, "inode btree")
        bad_ag("AGFL-entry", 3, 32, 36, 0xFFFFFFFE, "out of bounds")
    print(f"Passed {len(variants)} image comparisons, {ag_comparisons} AG comparisons, "
          f"7 superblock and {ag_rejections} AG rejection cases")


if __name__ == "__main__":
    main()
