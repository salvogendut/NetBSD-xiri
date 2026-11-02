#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Run the actual wsdecorctl decoder against valid and malformed files."""
import argparse
import os
import struct
import subprocess
import tempfile
from pathlib import Path


def check(executable, themes, root):
    image = (themes / "sgiconsole.wsd").read_bytes()
    palette_end = 24 + 3 * struct.unpack_from(">I", image, 16)[0]
    results = []

    def run(name, text, expected, data=image):
        (root / "asset.wsd").write_bytes(data)
        (root / "test.conf").write_text(text)
        result = subprocess.run(
            [executable, root / "test.conf"], capture_output=True,
            env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"},
        )
        assert (result.returncode == 0) == expected, (
            name, result.returncode, result.stderr,
        )
        assert b"Sanitizer" not in result.stderr, (name, result.stderr)
        results.append(name)

    base = "type image\nimage asset.wsd\nviewport 288 176 704 672\n"
    run("valid image", base, True)
    run("valid gradient", (themes / "initial.conf").read_text(), True)
    run("solid", "type solid\ncolor 123456\n", True)
    run("descending ramp", "type gradient\ntop ffffff\nbottom 000000\n", True)
    for name, content in {
        "unknown": "unknown foo\n",
        "duplicate": base + "type image\n",
        "bad viewport": base.replace("704 672", "4294967295 672"),
        "wrong type": base.replace("type image", "type gradient"),
        "negative": "type solid\ncolor -1\n",
        "overflow": "type solid\ncolor 1000000\n",
        "missing": "type gradient\ntop 000000\n",
        "nonuniform": base.replace("288 176 704 672", "0 0 704 672"),
    }.items():
        run(name, content, False)
    for name, data in {
        "short header": image[:7],
        "bad magic": b"x" + image[1:],
        "zero width": image[:8] + bytes(4) + image[12:],
        "huge width": image[:8] + struct.pack(">I", 0xffffffff) + image[12:],
        "palette overflow": image[:16] + struct.pack(">I", 224) + image[20:],
        "short palette": image[:30],
        "short run": image[:-1],
        "trailing data": image + b"x",
        "zero run": image[:palette_end] + bytes(4) + image[palette_end + 4:],
        "oversize run": image[:palette_end] + b"\xff" * 4 + image[palette_end + 4:],
        "invalid index": image[:palette_end + 4] + b"\xff" + image[palette_end + 5:],
    }.items():
        run(name, base, False, data)
    print("PASS", len(results), "parser/image cases:", ", ".join(results))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", type=Path)
    parser.add_argument(
        "--themes", type=Path,
        default=Path(__file__).resolve().parents[3] / "share/wscons/themes",
    )
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="wsdecor-parser-") as directory:
        check(args.executable.resolve(), args.themes, Path(directory))


if __name__ == "__main__":
    main()
