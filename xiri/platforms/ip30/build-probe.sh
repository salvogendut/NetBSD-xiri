#!/bin/sh
# Build a freestanding N64 diagnostic with the NetBSD cross tools.
set -eu
if [ "$#" -ne 2 ]; then
    echo "usage: $0 /path/to/tools/bin/mips64--netbsd- output-directory" >&2
    exit 2
fi
cross=$1
out=$2
src=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$out"
"${cross}gcc" -mabi=64 -march=mips4 -EB -ffreestanding -fno-pic \
    -mno-abicalls -G0 -fno-stack-protector -fno-builtin -O2 \
    -Wall -Wextra -Werror -c "$src/arcs64-probe.c" -o "$out/arcs64-probe.o"
"${cross}ld" -m elf64btsmip -T "$src/arcs64-probe.ld" \
    "$out/arcs64-probe.o" -o "$out/arcs64-probe"
"${cross}readelf" -h -l "$out/arcs64-probe" > "$out/arcs64-probe.elf.txt"
