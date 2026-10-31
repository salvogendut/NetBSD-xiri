#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
set -eu

if [ "$#" -gt 1 ] || { [ "$#" -eq 1 ] && [ "$1" != --images ]; }; then
	echo "usage: sh tests/run.sh [--images]" >&2
	exit 2
fi

xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-tests.XXXXXX")
trap 'rm -rf "$xfs_build"' EXIT
trap 'exit 1' HUP INT TERM

for program in t_probe t_io xfs_probe; do
	# CC and CFLAGS intentionally allow multiple shell words.
	${CC:-cc} ${CFLAGS:--O2 -g} -std=gnu11 -D_DEFAULT_SOURCE \
	    -Wall -Wextra -Werror -I "$xfs_root/include" \
	    -isystem "$xfs_root/dist" \
	    "$xfs_root/netbsd/xfs_sb.c" "$xfs_root/netbsd/xfs_probe.c" \
	    "$xfs_root/netbsd/xfs_cksum.c" "$xfs_root/netbsd/xfs_io.c" \
	    "$xfs_root/netbsd/xfs_ag.c" \
	    "$xfs_root/tests/$program.c" -o "$xfs_build/$program"
done

"$xfs_build/t_probe"
"$xfs_build/t_io"
if [ "${1:-}" = --images ]; then
	python3 "$xfs_root/tests/test_images.py" "$xfs_build/xfs_probe"
fi
