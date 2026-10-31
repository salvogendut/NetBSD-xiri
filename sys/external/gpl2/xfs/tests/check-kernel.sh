#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
# Compile-only check against the checkout's NetBSD amd64 kernel headers.
set -eu

case $(${CC:-cc} -dumpmachine) in
x86_64-*|amd64-*) ;;
*) echo "check-kernel.sh requires an amd64 compiler" >&2; exit 2 ;;
esac

xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_src=$(CDPATH= cd -- "$xfs_root/../../../.." && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-kernel.XXXXXX")
trap 'rm -rf "$xfs_build"' EXIT
trap 'exit 1' HUP INT TERM

for arch in amd64 x86 i386; do
	name=$arch
	[ "$arch" != amd64 ] || name=machine
	ln -s "$xfs_src/sys/arch/$arch/include" "$xfs_build/$name"
done
for header in stdint stddef stdbool; do
	ln -s "$xfs_src/sys/sys/$header.h" "$xfs_build/$header.h"
done

for source in xfs_sb xfs_probe; do
	${CC:-cc} -std=gnu11 -O2 -ffreestanding -nostdinc \
	    -D_KERNEL -D__NetBSD__ -U__linux__ -Wall -Wextra -Werror \
	    -I "$xfs_build" -isystem "$xfs_src/sys" \
	    -I "$xfs_root/include" -isystem "$xfs_root/dist" \
	    -c "$xfs_root/netbsd/$source.c" -o "$xfs_build/$source.o"
done
echo 'Passed NetBSD amd64 kernel-header compilation (no link or boot test)'
