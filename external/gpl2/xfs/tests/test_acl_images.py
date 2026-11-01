# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
"""Exercise ACL policy with inline/leaf/remote xfsprogs attribute storage."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

import make_puffs_images
from test_images import digest, require, run


def main():
    if len(sys.argv) != 2:
        raise SystemExit('usage: test_acl_images.py xfs_probe')
    probe = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='netbsd-xfs-acls-') as directory:
        root = Path(directory) / 'fixtures'
        saved = sys.argv
        try:
            sys.argv = ['make_puffs_images.py', str(root)]
            make_puffs_images.main()
        finally:
            sys.argv = saved
        for version in (4, 5):
            image = root / f'v{version}.img'
            before = digest(image)
            expected = json.loads((root / f'v{version}-acls.json').read_text())
            for path, value in expected.items():
                result = subprocess.run([probe, '-X', '/acls/' + path, str(image)], capture_output=True)
                if path == 'label':
                    require(result.returncode == 1 and not result.stdout and b'security' in result.stderr,
                            'valid ACL concealed a security label')
                    continue
                if path in ('mismatch', 'default-file'):
                    require(result.returncode == 1 and not result.stdout and b'ACL' in result.stderr,
                            f'inconsistent ACL accepted: {path}')
                    continue
                require(result.returncode == 0, f'{path}: {result.stderr}')
                attrs = [row for line in result.stdout.splitlines() if (row := json.loads(line))['flags'] == 2]
                require(len(attrs) == 1 and attrs[0]['value'] == value, f'{path}: ACL data differs')
            for path in ('attributes/acl', 'attributes/external-acl'):
                result = subprocess.run([probe, '-X', '/' + path, str(image)], capture_output=True)
                require(result.returncode == 1 and not result.stdout and b'ACL' in result.stderr, path)
            require(digest(image) == before, 'ACL reader modified image')
            print(f'Passed v{version}: inline/leaf/large ACLs, default ACLs and inconsistent ACL rejection')


if __name__ == '__main__':
    main()
