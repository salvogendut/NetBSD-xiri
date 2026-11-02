#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Convert artwork on a host with Pillow; NetBSD needs no image decoder."""
import argparse
import itertools
import struct
from pathlib import Path
from PIL import Image


def encode(path):
    with Image.open(path) as source:
        if not (1 <= source.width <= 2048 and 1 <= source.height <= 2048):
            raise ValueError("image dimensions must be between 1 and 2048")
        if source.convert("RGBA").getextrema()[3] != (255, 255):
            raise ValueError("transparent images are not supported")
        image = source.convert("RGB")
    data = getattr(image, "get_flattened_data", image.getdata)()
    colors = sorted(set(data))
    if len(colors) > 223:
        raise ValueError("reduce the image to at most 223 colours first")
    lookup = {color: i for i, color in enumerate(colors)}
    pixels = bytes(lookup[color] for color in data)
    runs = [(len(list(group)), color) for color, group in itertools.groupby(pixels)]
    result = bytearray(b"WSDIMG1\n")
    result += struct.pack(">4I", image.width, image.height, len(colors), len(runs))
    result += bytes(channel for color in colors for channel in color)
    for length, color in runs:
        result += struct.pack(">IB", length, color)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    data = encode(args.source)
    if args.check:
        if args.output.read_bytes() != data:
            raise SystemExit("generated image differs")
    else:
        args.output.write_bytes(data)


if __name__ == "__main__":
    main()
