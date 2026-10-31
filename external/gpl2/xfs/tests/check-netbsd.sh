#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
# Compile-only check against the checkout's NetBSD amd64 user-space headers.
set -eu

case $(${CC:-cc} -dumpmachine) in
x86_64-*|amd64-*) ;;
*) echo "check-netbsd.sh requires an amd64 compiler" >&2; exit 2 ;;
esac

xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_src=$(CDPATH= cd -- "$xfs_root/../../.." && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-userspace.XXXXXX")
trap 'rm -rf "$xfs_build"' EXIT
trap 'exit 1' HUP INT TERM

for arch in amd64 x86 i386; do
	name=$arch
	[ "$arch" != amd64 ] || name=machine
	ln -s "$xfs_src/sys/arch/$arch/include" "$xfs_build/$name"
done
for header in stdint stddef stdbool stdarg fcntl; do
	ln -s "$xfs_src/sys/sys/$header.h" "$xfs_build/$header.h"
done
ln -s "$xfs_src/lib/libpthread/pthread_types.h" "$xfs_build/pthread_types.h"
ln -s "$xfs_src/common/include/prop" "$xfs_build/prop"

for source in xfs_sb xfs_probe xfs_cksum xfs_io xfs_ag xfs_read_log xfs_read_btree \
    xfs_read_inode xfs_inode_stat xfs_read_attr xfs_read_attr_external xfs_read_acl xfs_read_file xfs_read_symlink xfs_read_directory xfs_lookup_path; do
	${CC:-cc} -std=gnu11 -O2 -nostdinc \
	    -D__NetBSD__ -U__linux__ -Wall -Wextra -Werror \
	    -I "$xfs_build" -isystem "$xfs_src/include" -isystem "$xfs_src/sys" \
	    -I "$xfs_root/include" -isystem "$xfs_root/dist" \
	    -c "$xfs_root/netbsd/$source.c" -o "$xfs_build/$source.o"
done
${CC:-cc} -std=gnu11 -O2 -nostdinc \
    -D__NetBSD__ -U__linux__ -Wall -Wextra -Werror \
    -I "$xfs_build" -isystem "$xfs_src/include" -isystem "$xfs_src/sys" \
    -I "$xfs_root/include" -isystem "$xfs_root/dist" \
    -c "$xfs_root/tests/xfs_probe.c" -o "$xfs_build/inspector.o"
${CC:-cc} -std=gnu11 -O2 -nostdinc \
    -D__NetBSD__ -U__linux__ -Wall -Wextra -Werror \
    -I "$xfs_build" -isystem "$xfs_src/include" -isystem "$xfs_src/sys" \
    -I "$xfs_src/lib/libpuffs" -I "$xfs_root/include" -isystem "$xfs_root/dist" \
    -c "$xfs_root/puffs/mount_xfs.c" -o "$xfs_build/mount_xfs.o"
${CC:-cc} -std=gnu11 -O2 -nostdinc \
    -D__NetBSD__ -U__linux__ -Wall -Wextra -Werror \
    -I "$xfs_build" -isystem "$xfs_src/include" -isystem "$xfs_src/sys" \
    -c "$xfs_root/tests/t_puffs.c" -o "$xfs_build/t_puffs.o"
echo 'Passed NetBSD amd64 user-space header compilation (no link or runtime test)'
