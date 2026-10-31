# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Exercise directory formats and path traversal on unmounted mkfs images."""
import json
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile

from test_images import digest, metadata_crc, require, run, values


def main():
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix="netbsd-xfs-paths-") as directory:
        root = Path(directory)
        source = root / "payload"
        payload = b"file bytes reached through an XFS pathname\n"
        source.write_bytes(payload)
        longname = "n" * 250
        deep = ["d" * 80 + str(i) for i in range(11)]
        remote_target = "/" + "/".join(deep) + "/payload"
        longtarget = "/" * 980 + "tiny/f0000"
        lines = ["dummy", "0 0", "d--755 0 0"]
        sizes = {"tiny": 2, "block": 60, "leaf": 300, "node": 1800}
        for name, count in sizes.items():
            lines.append(f"{name} d--755 0 0")
            for i in range(count):
                lines.append(f"f{i:04d} ---644 0 0 {source}")
            lines.append("$")
        for name in deep:
            lines.append(f"{name} d--755 0 0")
        lines.append(f"payload ---644 0 0 {source}")
        lines.extend(["$"] * len(deep))
        links = {
            "relative": "tiny/f0000", "absolute": "/block/f0000", "alias": "tiny",
            "chain": "relative", "loop": "loop", "loopa": "loopb", "loopb": "loopa",
            "dangling": "missing", "rootlink": "/", "remote": remote_target,
            "longslashes": longtarget,
        }
        for name, target in links.items():
            lines.append(f"{name} l--777 0 0 {target}")
        lines += [f"{longname} ---644 0 0 {source}", "sub d--755 0 0",
                  "up l--777 0 0 ../tiny/f0001", "root l--777 0 0 /", "$", "$", ""]
        proto = root / "prototype"
        proto.write_text("\n".join(lines))
        variants = (
            ("default", []),
            ("8k-directory", ["-n", "size=8192"]),
            ("64k-directory", ["-n", "size=65536"]),
            ("1k-block", ["-b", "size=1024"]),
        )
        for variant, options in variants:
            # xfsprogs 7.1.1's prototype writer truncates remote targets
            # requiring two 1 KiB blocks (core.size exceeds its one-block
            # mapping). Keep these mkfs images consistent; t_directory.c
            # independently covers contiguous and fragmented two-block links.
            variant_links = dict(links)
            variant_lines = list(lines)
            if variant == "1k-block":
                variant_links["longslashes"] = "/" * 920 + "tiny/f0000"
                variant_lines = [line.replace(longtarget, variant_links["longslashes"])
                                 for line in variant_lines]
            proto.write_text("\n".join(variant_lines))
            image = root / (variant + ".img")
            with image.open("xb") as stream:
                stream.truncate(400 * 1024 * 1024)
            run("mkfs.xfs", "-q", "-f", "-K", "-p", str(proto), *options, str(image))
            before = digest(image)
            sb = values(run(probe, str(image)))
            dirbytes = sb["blocksize"] << values(run("xfs_db", "-r", "-c", "sb 0",
                                                      "-c", "p dirblklog", str(image)))["dirblklog"]
            listing = [json.loads(line) for line in run(probe, "-l", "/", str(image)).splitlines()]
            ino_by_name = {entry["name"]: entry["ino"] for entry in listing}
            require(ino_by_name["."] == ino_by_name[".."] == sb["rootino"], "wrong root dots")
            for name, count in sizes.items():
                output = run(probe, "-l", "/" + name, str(image))
                entries = {entry["name"]: (entry["ino"], entry["type"])
                           for line in output.splitlines() if (entry := json.loads(line))}
                expected = {}
                db = run("xfs_db", "-r", "-c", f"inode {ino_by_name[name]}", "-c", "ls", str(image))
                for line in db.splitlines():
                    parts = line.split()
                    if len(parts) >= 7:
                        expected[parts[5]] = (int(parts[1]), 2 if parts[2] == "directory" else 1)
                require(entries == expected and len(entries) == count + 2, f"{variant}/{name}: listing mismatch")
                for i in (0, count // 2, count - 1):
                    result = subprocess.run((probe, "-R", f"/{name}/f{i:04d}", "0", "1000", str(image)),
                                            capture_output=True, check=True)
                    require(result.stdout == payload, f"{variant}/{name}: path read mismatch")
            # Verify that the default fixture really covers each external index form.
            if variant == "default":
                for name, magic, logical in (("block", "0x58444233", 0),
                                             ("leaf", "0x3df1", (1 << 35) // sb["blocksize"]),
                                             ("node", "0x3ebe", (1 << 35) // sb["blocksize"])):
                    printed = run("xfs_db", "-r", "-c", f"path /{name}", "-c", f"dblock {logical}",
                                  "-c", "p", str(image))
                    require(magic in printed, f"fixture {name} has wrong format")
            for name, target in variant_links.items():
                result = subprocess.run((probe, "-s", "/" + name, str(image)), capture_output=True, check=True)
                require(result.stdout == target.encode(), f"{variant}/{name}: readlink mismatch")
            for path in ("/relative", "/absolute", "/chain", "/alias/f0000", "/sub/up", "/remote",
                         "/longslashes", "/tiny//./f0000", "/tiny/../block/f0000",
                         "/rootlink/tiny/f0000", "/sub/root/tiny/f0000", "/" + longname,
                         "/../../tiny/f0000"):
                result = subprocess.run((probe, "-R", path, "3", "10", str(image)), capture_output=True, check=True)
                require(result.stdout == payload[3:13], f"{variant}{path}: symlink/path read mismatch")
            for path in ("/", "////", "/tiny/", "/alias/", "/rootlink/", "/sub/.."):
                require(values(run(probe, "-p", path, str(image)))["mode"] & 0o170000 == 0o040000,
                        f"{variant}{path}: trailing slash directory lookup failed")
            for path, message in (("/missing", "not found"), ("/dangling", "not found"),
                                  ("/loop", "too many"), ("/loopa", "too many"),
                                  ("/tiny/f0000/", "trailing slash"), ("/relative/", "trailing slash"),
                                  ("/tiny/f0000/child", "not a directory"),
                                  ("/" + "a" * 256, "component too long"),
                                  ("/" + "a" * 4096, "path too long"),
                                  ("/longslashes/" + "/" * 3300, "expanded image path too long")):
                result = subprocess.run((probe, "-p", path, str(image)), capture_output=True, text=True)
                require(result.returncode == 1 and message in result.stderr,
                        f"{variant}{path[:60]}: {result.returncode}: {result.stderr}")
            result = subprocess.run((probe, "-s", "/tiny/f0000", str(image)), capture_output=True, text=True)
            require(result.returncode == 1 and "requires a symlink" in result.stderr, "readlink accepted regular file")
            require(before == digest(image), f"{variant}: inspection changed image")

            # Mutate only disposable fixtures, and restore every block afterwards.
            def physical(fsb):
                aglog = (sb["agblocks"] - 1).bit_length()
                return ((fsb >> aglog) * sb["agblocks"] + (fsb & ((1 << aglog) - 1))) * sb["blocksize"]

            def bad_block(path, byte, crc_offset, message, update, command="-l"):
                maps = run("xfs_db", "-r", "-c", f"path {path}", "-c", "bmap", str(image))
                match = re.search(r"data offset 0 startblock (\d+).*?count (\d+)", maps)
                require(match is not None, "missing external data mapping")
                offset = physical(int(match[1]))
                blocksize = dirbytes if command == "-l" else min(int(match[2]), 2) * sb["blocksize"]
                with image.open("r+b") as stream:
                    stream.seek(offset)
                    original = stream.read(blocksize)
                    changed = bytearray(original)
                    changed[byte] ^= 1
                    if update:
                        metadata_crc(changed, crc_offset)
                    stream.seek(offset)
                    stream.write(changed)
                try:
                    result = subprocess.run((probe, command, path, str(image)), capture_output=True, text=True)
                    require(result.returncode == 1 and message in result.stderr,
                            f"{variant}{path} corruption: {result.returncode}: {result.stderr}")
                finally:
                    with image.open("r+b") as stream:
                        stream.seek(offset)
                        stream.write(original)

            for path, crc_offset, command, owner, uuid in (("/block", 4, "-l", 40, 24),
                                                          ("/longslashes", 12, "-s", 32, 16)):
                bad_block(path, 500, crc_offset, "CRC32C", False, command)
                bad_block(path, owner, crc_offset, "mismatch", True, command)
                bad_block(path, uuid, crc_offset, "mismatch", True, command)
            require(before == digest(image), f"{variant}: fixture restoration failed")
            print(f"PASS {variant}: directories match xfs_db; paths, symlinks, errors and corruption checked")
    print("Passed 4 pathname image configurations and 24 directory/symlink corruption cases")


if __name__ == "__main__":
    main()
