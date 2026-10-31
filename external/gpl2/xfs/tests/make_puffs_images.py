# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Create disposable PUFFS mount fixtures; output directory must be new."""
from pathlib import Path
import json
import os
import re
import struct
import subprocess
import sys

from test_images import metadata_crc, require, run, values


def encode_acl(entries):
    return struct.pack(">I", len(entries)) + b"".join(
        struct.pack(">IIHH", tag, identity, perm, 0) for tag, identity, perm in entries)


def replace_attribute(image, ino, sb, version, name, value):
    """Replace a fixture value of identical length, preserving block CRCs.

    xfs_db allocates the actual shortform/leaf/remote storage first. This
    helper only injects binary test data; it is not a filesystem writer.
    """
    bs = sb['blocksize']
    def address(fsb):
        return ((fsb >> sb['agblklog']) * sb['agblocks'] +
                (fsb & ((1 << sb['agblklog']) - 1))) * bs
    offset = address(ino >> sb['inopblog']) + (ino & ((1 << sb['inopblog']) - 1)) * sb['inodesize']
    with image.open('r+b') as f:
        f.seek(offset)
        inode = bytearray(f.read(sb['inodesize']))
        core = 176 if version == 5 else 100
        start = core + inode[82] * 8
        if inode[83] == 1:
            cursor = start + 4
            for _ in range(inode[start + 2]):
                nl, vl, flags = inode[cursor:cursor + 3]
                if flags == 2 and inode[cursor + 3:cursor + 3 + nl] == name:
                    require(vl == len(value), "inline replacement length")
                    inode[cursor + 3 + nl:cursor + 3 + nl + vl] = value
                    if version == 5: metadata_crc(inode, 100)
                    f.seek(offset); f.write(inode)
                    return 'inline'
                cursor += 3 + nl + vl
        mappings = run('xfs_db', '-r', '-c', f'inode {ino}', '-c', 'bmap -a', str(image))
        blocks = {}
        for start, fsb, length in re.findall(r'attr offset (\d+) startblock (\d+) \([^)]*\) count (\d+)', mappings):
            blocks.update({int(start) + i: address(int(fsb) + i) for i in range(int(length))})
        for where in blocks.values():
            f.seek(where)
            data = bytearray(f.read(bs))
            if struct.unpack_from('>H', data, 8)[0] != (0x3bee if version == 5 else 0xfbee):
                continue
            info, header = (56, 80) if version == 5 else (12, 32)
            for i in range(struct.unpack_from('>H', data, info)[0]):
                idx, flags = struct.unpack_from('>HB', data, header + i * 8 + 4)
                if flags & 14 != 2: continue
                if flags & 1:
                    vl, nl = struct.unpack_from('>HB', data, idx)
                    at = idx + 3
                else:
                    logical, vl, nl = struct.unpack_from('>IIB', data, idx)
                    at = idx + 9
                if data[at:at + nl] != name: continue
                require(vl == len(value), 'external replacement length')
                if flags & 1:
                    data[at + nl:at + nl + vl] = value
                    if version == 5: metadata_crc(data, 12)
                    f.seek(where); f.write(data)
                    return 'leaf'
                done, h = 0, 56 if version == 5 else 0
                while done < vl:
                    part = min(vl - done, bs - h)
                    f.seek(blocks[logical]); remote = bytearray(f.read(bs))
                    remote[h:h + part] = value[done:done + part]
                    if version == 5: metadata_crc(remote, 12)
                    f.seek(blocks[logical]); f.write(remote)
                    done += part; logical += 1
                return 'remote'
    raise RuntimeError(f'attribute {name!r} not found in fixture inode {ino}')


def external_attributes(image, ino):
    """Use xfsprogs to build a node tree and two remote values."""
    commands = [f"inode {ino}", "attr_set -u big " + "B" * 8192,
                "attr_set -u max " + "M" * 65536]
    commands += [f"attr_set -u n{i:03d} " + "N" * 1000 for i in range(100)]
    result = subprocess.run(["xfs_db", "-x", str(image)], input="\n".join(commands) + "\n",
                            text=True, capture_output=True, check=True)
    require(not result.stdout.strip() and not result.stderr.strip(),
            f"xfs_db attribute generation: {result.stdout} {result.stderr}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: make_puffs_images.py NEW-DIRECTORY")
    root = Path(sys.argv[1]).resolve()
    root.mkdir()  # Never replace user images.
    (root / "hello").write_bytes(b"NetBSD XFS through PUFFS\n")
    (root / "public").write_bytes(bytes(range(256)) * 512 + b"end")
    proto = root / "prototype"
    proto.write_text("\n".join([
        "dummy", "0 0", "d--755 0 0",
        f"hello ---640 12 34 {root / 'hello'}",
        f"public ---644 12 34 {root / 'public'}",
        f"private ---600 12 34 {root / 'hello'}",
        "attributes d--755 12 34",
        *(f"{name} ---640 12 34 {root / 'hello'}" for name in
          ("user", "acl", "label", "trusted", "foreign", "external", "external-list", "external-acl",
           "external-label", "bad", "duplicate", "incomplete")),
        "$",
        "acls d--755 12 34",
        *(f"{name} ---{mode} 12 34 {root / 'hello'}" for name, mode in
          (("access", "644"), ("external", "644"), ("large", "644"), ("label", "644"),
           ("masked", "604"), ("execute", "654"), ("mismatch", "640"), ("default-file", "644"))),
        "search d--755 12 34", f"file ---644 12 34 {root / 'hello'}", "$",
        "defaults d--755 12 34", f"file ---644 12 34 {root / 'hello'}", "$", "$",
        "link l--777 12 34 hello",
        "longlink l--777 12 34 " + "/" * 990,
        "locked d--700 12 34", f"file ---644 12 34 {root / 'hello'}", "$",
        "many d--755 12 34",
        *(f"f{i:04d} ---644 12 34 {root / 'hello'}" for i in range(1800)),
        "$", "$", ""
    ]))
    for version in (4, 5):
        image = root / f"v{version}.img"
        with image.open("xb") as f:
            f.truncate(400 * 1024 * 1024)
        run("mkfs.xfs", "-q", "-f", "-K", "-n", "parent=0", "-m",
            f"crc={int(version == 5)}", "-p", str(proto), str(image))
        sb = values(run("xfs_db", "-r", "-c", "sb 0", "-c",
                        "p rootino blocksize agblocks agblklog inopblog inodesize logstart", str(image)))
        # Temporarily make room for the synthetic hard-link entry. Restore
        # the default property afterward; the PUFFS mount must accept it.
        run("xfs_db", "-x", "-c", f"inode {sb['rootino']}",
            "-c", "attr_remove -r xfs:autofsck", str(image))
        names = {}
        for line in run("xfs_db", "-r", "-c", "blockget -n", "-c", "ncheck", str(image)).splitlines():
            number, name = line.split(maxsplit=1)
            names[name] = int(number)
        # mkfs also copies SELinux labels from prototype source files. Remove
        # those labels only inside our disposable fixture, never on host files.
        if "security.selinux" in os.listxattr(root / "hello"):
            commands = []
            for name, number in names.items():
                if name in ("hello", "public", "private", "locked/file") or name.startswith("many/f") or (name.startswith(("attributes/", "acls/")) and not name.endswith("/.")):
                    commands.extend((f"inode {number}", "attr_remove -s selinux"))
            subprocess.run(["xfs_db", "-x", str(image)], input="\n".join(commands) + "\n",
                           text=True, capture_output=True, check=True)
        def attr(path, command):
            run("xfs_db", "-x", "-c", f"inode {names[path]}", "-c", command, str(image))

        # Numeric tags follow the disk format: owner, named users, owning
        # group, named groups, mask, other. No setfacl or host mount is used.
        entries = [(1, 0xffffffff, 6), (2, 99, 4), (2, 100, 0), (4, 0xffffffff, 0),
                   (8, 77, 4), (8, 78, 2), (16, 0xffffffff, 4), (32, 0xffffffff, 4)]
        acl_values = {}
        for path in ('access', 'external', 'large', 'label', 'masked', 'execute', 'mismatch', 'default-file', 'search/.', 'defaults/.'):
            current = list(entries)
            if path == 'masked': current[-2] = (16, 0xffffffff, 0)
            if path == 'execute':
                current[-3] = (8, 78, 1)
                current[-2] = (16, 0xffffffff, 5)
            if path == 'search/.':
                current[0] = (1, 0xffffffff, 7)
                current[1] = (2, 99, 5)
                current[4] = (8, 77, 5)
                current[-2] = (16, 0xffffffff, 5)
                current[-1] = (32, 0xffffffff, 5)
            if path == 'large':
                current[3:3] = [(2, i, 0) for i in range(1000, 1000 + (5453 if version == 5 else 17))]
            name = 'SGI_ACL_DEFAULT' if path in ('defaults/.', 'default-file') else 'SGI_ACL_FILE'
            value = encode_acl(current)
            attr('acls/' + path, f'attr_set -r -v {len(value)} {name}')
            storage = replace_attribute(image, names['acls/' + path], sb, version, name.encode(), value)
            if path == 'large' and version == 5: require(storage == 'remote', 'large ACL must be remote')
            acl_values[path] = value.hex()
        for path in ('access', 'external', 'large', 'masked'):
            attr('acls/' + path, 'attr_set -u fixture supported')
        attr('acls/external', 'attr_set -u big ' + 'B' * 8192)
        attr('acls/label', 'attr_set -s selinux denied')
        (root / f'v{version}-acls.json').write_text(json.dumps(acl_values) + '\n')

        for path in ("attributes/user", "private"):
            attr(path, "attr_set -u fixture supported")
        attr("attributes/user", "attr_set -u -v 4 binary")
        attr("attributes/user", "attr_set -u -v 0 empty")
        attr("attributes/acl", "attr_set -r -v 4 SGI_ACL_FILE")
        attr("attributes/label", "attr_set -s selinux denied")
        attr("attributes/trusted", "attr_set -r fixture denied")
        attr("attributes/foreign", "attr_set -r xfs:autofsck repair")
        external_attributes(image, names["attributes/external"])
        commands = [f"inode {names['attributes/external-list']}"]
        commands += [f"attr_set -u -v 0 l{i:04d}" for i in range(800)]
        result = subprocess.run(["xfs_db", "-x", str(image)], input="\n".join(commands) + "\n",
                                text=True, capture_output=True, check=True)
        require(not result.stdout.strip() and not result.stderr.strip(), "large-list fixture generation failed")
        for path, command in (("external-acl", "attr_set -r -v 4 SGI_ACL_FILE"),
                              ("external-label", "attr_set -s selinux denied")):
            attr(f"attributes/{path}", "attr_set -u big " + "B" * 8192)
            attr(f"attributes/{path}", command)
        for path in ("attributes/bad", "attributes/duplicate", "attributes/incomplete"):
            attr(path, "attr_set -u one supported")
        attr("attributes/duplicate", "attr_set -u two supported")
        directory = run("xfs_db", "-r", "-c", f"inode {sb['rootino']}", "-c", "p u3", str(image))
        # xfs_db calls v2 dir data 'u' on some older versions. Read names
        # independently of the presentation prefix.
        if "inumber" not in directory:
            directory = run("xfs_db", "-r", "-c", f"inode {sb['rootino']}", "-c", "p u", str(image))
        ino_match = re.search(r"list\[0\].inumber.i4 = (\d+)", directory)
        require(ino_match is not None, directory)
        hello = int(ino_match.group(1))

        def read_inode(f, ino):
            fsb = ino >> sb["inopblog"]
            ag, agb = fsb >> sb["agblklog"], fsb & ((1 << sb["agblklog"]) - 1)
            offset = (ag * sb["agblocks"] + agb) * sb["blocksize"]
            offset += (ino & ((1 << sb["inopblog"]) - 1)) * sb["inodesize"]
            f.seek(offset)
            return offset, bytearray(f.read(sb["inodesize"]))

        def write_inode(f, offset, data):
            if version == 5:
                metadata_crc(data, 100)
            f.seek(offset)
            f.write(data)

        with image.open("r+b") as f:
            offset, data = read_inode(f, sb["rootino"])
            require(data[82] == 0, "test root still has an attribute fork")
            core = 176 if version == 5 else 100
            size = struct.unpack_from(">Q", data, 56)[0]
            require(data[core + 1] == 0, "test requires 32-bit shortform inodes")
            # Add a genuine second directory reference without allocating an
            # orphan placeholder inode. This is a synthetic hard-link fixture.
            cursor, last_offset, last_length = core + 6, 0, 0
            for _ in range(data[core]):
                last_length = data[cursor]
                last_offset = struct.unpack_from(">H", data, cursor + 1)[0]
                cursor += 3 + last_length + 1 + 4  # ftype enabled by mkfs
            require(cursor == core + size, "unexpected shortform geometry")
            entry_offset = last_offset + ((12 + last_length + 7) & ~7)
            entry = bytes((5,)) + struct.pack(">H", entry_offset) + b"alias" + bytes((1,)) + struct.pack(">I", hello)
            require(core + size + len(entry) <= len(data), "root fork full")
            data[core + size:core + size + len(entry)] = entry
            data[core] += 1
            struct.pack_into(">Q", data, 56, size + len(entry))
            write_inode(f, offset, data)
            for path in ("attributes/user", "attributes/bad", "attributes/duplicate", "attributes/incomplete"):
                offset, data = read_inode(f, names[path])
                start = core + data[82] * 8
                require(data[83] == 1, f"{path}: not shortform")
                if path.endswith("/user"):
                    cursor = start + 4
                    for _ in range(data[start + 2]):
                        nlen, vlen = data[cursor:cursor + 2]
                        if data[cursor + 3:cursor + 3 + nlen] == b"binary":
                            require(vlen == 4, "binary fixture length")
                            data[cursor + 3 + nlen:cursor + 3 + nlen + vlen] = b"\0\xffx\0"
                        cursor += 3 + nlen + vlen
                elif path.endswith("/bad"):
                    data[start + 2] += 1  # count runs beyond the encoded total
                elif path.endswith("/duplicate"):
                    second = start + 4 + 3 + data[start + 4] + data[start + 5]
                    data[second + 3:second + 6] = b"one"
                else:
                    data[start + 6] = 0x80  # INCOMPLETE is invalid in shortform
                write_inode(f, offset, data)
            offset, data = read_inode(f, hello)
            struct.pack_into(">I", data, 16, 2)
            when = 4102444800 if version == 5 else -1
            for time_offset in ((32, 40, 48, 144) if version == 5 else (32, 40, 48)):
                encoded = ((when + (1 << 31)) * 1000000000 + 123456789) if version == 5 else (0xffffffff << 32 | 123456789)
                struct.pack_into(">Q", data, time_offset, encoded)
            if version == 5:
                flags = struct.unpack_from(">Q", data, 120)[0]
                struct.pack_into(">Q", data, 120, flags | 8)
            write_inode(f, offset, data)
        # Adding this property may convert the v4 root to a block directory;
        # xfs_db performs that conversion, preserving the hard link above.
        run("xfs_db", "-x", "-c", f"inode {sb['rootino']}",
            "-c", "attr_set -r xfs:autofsck repair", str(image))
        (root / f"v{version}.json").write_text(json.dumps({"hello": hello, "mtime": when}) + "\n")
        print(f"Created {image}: 1800-entry directory, permissions, symlinks, hard link and timestamps")
        if version == 5:
            for name in ("root-attr.img", "root-acl.img", "root-bad-attr.img", "bad-log.img"):
                destination = root / name
                with image.open("rb") as src, destination.open("xb") as dst:
                    while data := src.read(1024 * 1024):
                        if any(data):
                            dst.write(data)
                        else:
                            dst.seek(len(data), 1)
                    dst.truncate(src.tell())
            run("xfs_db", "-x", "-c", f"inode {sb['rootino']}",
                "-c", "attr_set -r fixture rejected", str(root / "root-attr.img"))
            run("xfs_db", "-x", "-c", f"inode {sb['rootino']}",
                "-c", "attr_set -r -v 4 SGI_ACL_FILE", str(root / "root-acl.img"))
            with (root / "root-bad-attr.img").open("r+b") as f:
                offset, data = read_inode(f, sb["rootino"])
                require(data[83] == 1, "root attributes not shortform")
                data[176 + data[82] * 8 + 2] += 1
                write_inode(f, offset, data)
            with (root / "bad-log.img").open("r+b") as f:
                f.seek(sb["logstart"] * sb["blocksize"])
                f.write(b"BAD!")


if __name__ == "__main__":
    main()
