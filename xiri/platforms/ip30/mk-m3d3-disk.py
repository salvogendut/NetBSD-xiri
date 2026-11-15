#!/usr/bin/env python3
"""Create the disposable 16 MiB M3d3 SCSI image with a known block pattern."""
import sys
from pathlib import Path

BLOCKS = 32768
BLOCK_SIZE = 512


def payload_byte(lba, offset):
    return (lba * 131 + ((lba >> 8) & 0xff) * 197 +
            ((lba >> 16) & 0xff) * 61 + ((lba >> 24) & 0xff) * 241 +
            (offset & 0xff) * 29 + ((offset >> 8) & 0xff) * 233 +
            0x5a) & 0xff


def sector_bytes(lba):
    header = lba.to_bytes(4, "big")
    complement = ((~lba) & 0xffffffff).to_bytes(4, "big")
    return header + complement + bytes(payload_byte(lba, i) for i in range(8, 512))


def main():
    if len(sys.argv) != 2:
        raise SystemExit(f'usage: {sys.argv[0]} IMAGE')
    path = Path(sys.argv[1])
    headers = set()
    with path.open('wb') as f:
        for lba in range(BLOCKS):
            header = lba.to_bytes(4, "big")
            if header in headers:
                raise SystemExit(f'duplicate sector header for LBA {lba}')
            headers.add(header)
            f.write(sector_bytes(lba))
    if len(headers) != BLOCKS:
        raise SystemExit('sector headers are not unique')
    print(f'{path}: {BLOCKS} blocks, {BLOCKS * BLOCK_SIZE} bytes')


if __name__ == '__main__':
    main()
