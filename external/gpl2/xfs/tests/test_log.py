# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Journal checks on disposable mkfs images; no mounts or device writes."""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from test_images import digest, metadata_crc, require, run, values


def journal(probe, image):
    return dict(line.split(" = ") for line in run(probe, "-j", str(image)).splitlines())


def compare(probe, image):
    info = journal(probe, image)
    output = run("xfs_logprint", "-t", str(image))
    match = re.search(r"log tail: (\d+) head: (\d+) state: <CLEAN>", output)
    require(match is not None, output)
    require(info["state"] == "clean" and
            (info["tail"], info["head"]) == match.groups(), f"{info}: {output}")
    return info


def seal(record):
    length = struct.unpack_from(">I", record, 12)[0]
    size = struct.unpack_from(">I", record, 320)[0]
    hblocks = (size + 32767) // 32768
    pieces = [record[:328]]
    for i in range(1, (length + 32767) // 32768):
        pieces.append(record[i * 512:i * 512 + 260])
    pieces.append(record[hblocks * 512:hblocks * 512 + length])
    protected = bytearray(b"".join(pieces))
    metadata_crc(protected, 32)
    record[32:36] = protected[32:36]


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: test_log.py xfs_probe")
    for tool in ("mkfs.xfs", "xfs_db", "xfs_logprint"):
        if shutil.which(tool) is None:
            raise SystemExit(f"journal image checks require {tool}")
    probe = str(Path(sys.argv[1]).resolve())
    variants = (
        ("default", []),
        ("4k-sector", ["-s", "size=4096"]),
        ("64k-stripe", ["-l", "su=64k"]),
        ("256k-stripe", ["-l", "su=256k"]),
        ("1k-block", ["-b", "size=1024"]),
        ("v4", ["-m", "crc=0"]),
    )
    with tempfile.TemporaryDirectory(prefix="netbsd-xfs-log-") as directory:
        root = Path(directory)
        for name, options in variants:
            image = root / (name + ".img")
            with image.open("xb") as stream:
                stream.truncate(400 * 1024 * 1024)
            run("mkfs.xfs", "-q", "-f", "-K", *options, str(image))
            before = digest(image)
            info = compare(probe, image)
            require(info["initialized"] == "1", f"{name}: {info}")
            run(probe, "-p", "/", str(image))
            require(before == digest(image), f"{name}: inspection changed image")
            print(f"PASS {name}: initial log agrees with xfs_logprint; guarded root lookup")

        image = root / "default.img"
        sb = values(run("xfs_db", "-r", "-c", "sb 0", "-c",
                        "p blocksize agblocks agblklog logstart logblocks", str(image)))
        ag = sb["logstart"] >> sb["agblklog"]
        agb = sb["logstart"] & ((1 << sb["agblklog"]) - 1)
        base = (ag * sb["agblocks"] + agb) * sb["blocksize"]
        length = sb["logblocks"] * sb["blocksize"]
        with image.open("rb") as stream:
            stream.seek(base)
            original = bytearray(stream.read(length))
        initial = original[:1024]

        def replace(data):
            with image.open("r+b") as stream:
                stream.seek(base)
                stream.write(data)

        def rejected(message):
            for args in (("-j",), ("-p", "/"), ("-l", "/"), ("-R", "/", "0", "20")):
                result = subprocess.run((probe, *args, str(image)), capture_output=True, text=True)
                require(result.returncode == 1 and message in result.stderr and not result.stdout,
                        f"{args}: {result.returncode}: {result.stderr}: {result.stdout}")

        clean = bytearray(initial)
        seal(clean)
        replace(clean)
        require(compare(probe, image)["initialized"] == "0", "checksummed unmount")
        clean[512 + 100] ^= 1
        replace(clean)
        rejected("CRC32C")

        replace(bytes(length))
        rejected("cleanliness unknown")
        replace(original)
        newer = bytearray(initial)
        struct.pack_into(">Q", newer, 16, (1 << 32) | 2)
        struct.pack_into(">I", newer, 36, 0)
        newer[512 + 8:512 + 10] = bytes((0x69, 2))
        seal(newer)
        replace(initial + newer)
        info = journal(probe, image)
        require(info["state"] == "needs-recovery" and info["head"] == "4", str(info))
        result = subprocess.run((probe, "-p", "/", str(image)), capture_output=True, text=True)
        require(result.returncode == 1 and "needs recovery" in result.stderr and not result.stdout,
                f"dirty log bypassed guard: {result}")
        # Explicit raw inspection remains possible and makes no cleanliness claim.
        run(probe, "--raw", "-p", "/", str(image))
        newer[512 + 8:512 + 10] = bytes((0xaa, 0x20))
        newer[32:36] = bytes(4)
        replace(initial + newer)
        rejected("canonical initial")
        seal(newer)
        replace(initial + newer)
        require(compare(probe, image)["head"] == "4", "latest unmount not found")
        struct.pack_into(">I", newer, 12, 1024)
        replace(initial + newer)
        rejected("incomplete")

        # A synthetic wrapped unmount in a real mkfs image, compared with xfsprogs.
        blocks = length // 512
        wrapped = bytearray((b"\0\0\0\1" + bytes(508)) * blocks)
        record = bytearray(initial)
        struct.pack_into(">Q", record, 16, (1 << 32) | (blocks - 1))
        struct.pack_into(">Q", record, 24, (1 << 32) | (blocks - 1))
        struct.pack_into(">I", record, 512, 2)
        seal(record)
        wrapped[-512:] = record[:512]
        wrapped[:512] = record[512:]
        replace(wrapped)
        before = digest(image)
        info = compare(probe, image)
        require(info["head"] == "1" and info["cycle"] == "2", str(info))
        run(probe, "-p", "/", str(image))
        require(before == digest(image), "wrapped inspection changed image")
        replace(original)
        print("PASS journal corruption, dirty/clean transitions, raw override and wrapped-record comparison")
    print("Passed 6 mkfs journal comparisons and journal guard/corruption checks")


if __name__ == "__main__":
    main()
