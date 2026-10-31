#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Compare the portable reader with references captured in a Linux XFS guest."""
import hashlib
import json
from pathlib import Path
import re
import stat
import struct
import subprocess
import sys

from test_images import digest, require, run, values


def attributes(references):
    """Translate Linux's ACL xattr ABI to XFS's privileged SGI ACL storage."""
    result = {}
    for row in references:
        if row["type"] != "attr":
            continue
        name, data = row["name"], bytes.fromhex(row["value"])
        if name.startswith("user."):
            key = (0, name[5:])
        else:
            require(name in ("system.posix_acl_access", "system.posix_acl_default"), name)
            require(struct.unpack_from("<I", data)[0] == 2 and len(data) % 8 == 4, "Linux ACL format")
            entries = list(struct.iter_unpack("<HHI", data[4:]))
            data = struct.pack(">I", len(entries)) + b"".join(struct.pack(">IIHH", tag, identity, perm, 0)
                                                         for tag, perm, identity in entries)
            key = (2, "SGI_ACL_FILE" if name.endswith("access") else "SGI_ACL_DEFAULT")
        result.setdefault(row["path"], {})[key] = data
    return result


def native_manifest(references):
    """Small whitespace format for the native C client (fixture names are ASCII)."""
    lines = []
    for row in references:
        if row["type"] == "file":
            keys = ("path", "ino", "mode", "uid", "gid", "size", "nlink", "allocated_bytes", "mtime_sec", "mtime_nsec", "sha256")
            lines.append("file " + " ".join(str(row[k]) for k in keys))
        elif row["type"] == "access":
            lines.append("access " + " ".join(str(row[k]) for k in ("path", "uid", "gid", "group", "allowed")))
    for path, attrs in attributes(references).items():
        for (flags, name), data in attrs.items():
            lines.append(f"attr {path} {flags} {name} {data.hex()}")
    return "\n".join(lines) + "\n"


def layout(references):
    maps = {r["path"]: r["extents"] for r in references if r["type"] == "fiemap"}
    sparse = maps["sparse.bin"]
    require(any(a["logical"] + a["length"] < b["logical"] for a, b in zip(sparse, sparse[1:])), "fixture has no hole")
    require(any(e["flags"] & 0x800 for e in maps["prealloc.bin"]), "fixture has no unwritten extent")
    fragmented = maps["fragmented.bin"]
    require(any(a["logical"] + a["length"] == b["logical"] and
                a["physical"] + a["length"] != b["physical"] for a, b in zip(fragmented, fragmented[1:])), "fixture is not physically fragmented")


def main():
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_linux_images.py xfs_probe FIXTURE-DIRECTORY")
    probe, directory = str(Path(sys.argv[1]).resolve()), Path(sys.argv[2])
    provenance = json.loads((directory / "provenance.json").read_text())
    versions = ["4", "5"]
    if (directory / "linux-v4-log1.img").exists():
        versions.append("4-log1")
    for version in versions:
        image = directory / f"linux-v{version}.img"
        dirty = directory / f"linux-v{version}-dirty.img"
        refs = json.loads((directory / f"linux-v{version}.json").read_text())
        layout(refs)
        attrs = attributes(refs)
        for path, state in ((image, "clean"), (dirty, "needs-recovery")):
            require(digest(path).hex() == provenance[path.name], f"fixture changed before testing: {path}")
            info = dict(line.split(" = ") for line in run(probe, "-j", str(path)).splitlines())
            upstream = run("xfs_logprint", "-t", str(path))
            match = re.search(r"log tail: (\d+) head: (\d+) state: <(CLEAN|DIRTY)>", upstream)
            require(match is not None and (info["tail"], info["head"]) == match.groups()[:2], upstream)
            require(info["state"] == state and info["initialized"] == "0" and
                    match[3] == ("CLEAN" if state == "clean" else "DIRTY"), str(info))
            require(info["version"] == ("1" if version == "4-log1" else "2") and
                    info["legacy"] == "0" and info["checksummed"] == "1", str(info))
        for row in refs:
            if row["type"] != "file":
                continue
            path = "/" + row["path"]
            actual = values(run(probe, "-p", path, str(image)))
            for key in ("ino", "mode", "uid", "gid", "size", "nlink", "allocated_bytes", "mtime_sec", "mtime_nsec"):
                require(actual[key] == row[key], f"{path}: {key}: {actual[key]} != {row[key]}")
            if stat.S_ISREG(row["mode"]):
                content = subprocess.check_output([probe, "-R", path, "0", str(row["size"] + 1), str(image)])
                require(len(content) == row["size"] and hashlib.sha256(content).hexdigest() == row["sha256"], f"{path}: Linux content hash differs")
                # Exercise unaligned/partial reads across extent and hole boundaries.
                for offset in (1, 4093, 65533, 69629, 1048573, 4194301, max(0, row["size"] - 7)):
                    part = subprocess.check_output([probe, "-R", path, str(offset), "1031", str(image)])
                    require(part == content[offset:offset + 1031], f"{path}: partial read at {offset}")
            result = run(probe, "-X", path, str(image))
            actual_attrs = {(r["flags"], bytes.fromhex(r["name"]).decode()): bytes.fromhex(r["value"])
                            for line in result.splitlines() if (r := json.loads(line))}
            require(actual_attrs == attrs.get(row["path"], {}), f"{path}: Linux attributes differ")
        names = [json.loads(line)["name"] for line in run(probe, "-l", "/many", str(image)).splitlines()]
        expected = {".", ".."} | {f"f{i:04d}" for i in range(768) if i % 4}
        require(len(names) == len(expected) and set(names) == expected, "Linux directory entries differ")
        require(subprocess.check_output([probe, "-s", "/link", str(image)]) == b"plain.bin", "symlink differs")
        for args in (("-p", "/"), ("-l", "/"), ("-R", "/plain.bin", "0", "20"), ("-X", "/acl.txt")):
            result = subprocess.run([probe, *args, str(dirty)], capture_output=True)
            require(result.returncode == 1 and not result.stdout and b"needs recovery" in result.stderr, f"dirty image accepted: {args}")
        for path in (image, dirty):
            require(digest(path).hex() == provenance[path.name], f"inspection modified {path}")
        (directory / f"linux-v{version}.manifest").write_text(native_manifest(refs))
        print(f"Passed Linux v{version}: FIEMAP layouts, content/partial reads, metadata, directory, links, attributes/ACLs, journal comparison and unchanged dirty rejection")


if __name__ == "__main__":
    main()
