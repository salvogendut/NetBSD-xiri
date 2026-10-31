#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
# Requires the corrected libpuffs from this checkout (see README).
set -eu
if [ "$#" -ne 1 ] || [ "$(uname -s)" != NetBSD ] || [ "$(id -u)" -ne 0 ]; then
	echo 'usage (NetBSD, root): sh tests/run-linux-puffs.sh FIXTURE-DIRECTORY' >&2
	exit 2
fi
xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_images=$(CDPATH= cd -- "$1" && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-linux.XXXXXX")
xfs_pid=
cleanup()
{
	umount "$xfs_build/mnt" 2>/dev/null || true
	if [ -n "$xfs_pid" ]; then
		kill "$xfs_pid" 2>/dev/null || true
		wait "$xfs_pid" 2>/dev/null || true
	fi
	if rmdir "$xfs_build/mnt"; then rm -rf "$xfs_build"; fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir "$xfs_build/mnt"
make -C "$xfs_root/puffs" MKMAN=no
${CC:-cc} -O2 -g -Wall -Wextra -Werror "$xfs_root/tests/t_linux_puffs.c" -o "$xfs_build/client"
xfs_versions='4 5'
if [ -f "$xfs_images/linux-v4-log1.img" ]; then xfs_versions="$xfs_versions 4-log1"; fi
for xfs_version in $xfs_versions; do
	xfs_image="$xfs_images/linux-v$xfs_version.img"
	xfs_before=$(cksum -a sha256 "$xfs_image")
	"$xfs_root/puffs/mount_xfs" -s "$xfs_image" "$xfs_build/mnt" > "$xfs_build/server.log" 2>&1 &
	xfs_pid=$!
	xfs_attempt=0
	until [ -r "$xfs_build/mnt/plain.bin" ]; do
		if ! kill -0 "$xfs_pid" 2>/dev/null || [ "$xfs_attempt" -ge 20 ]; then
			cat "$xfs_build/server.log" >&2
			echo 'Linux fixture mount did not become ready' >&2
			exit 1
		fi
		sleep 1
		xfs_attempt=$((xfs_attempt + 1))
	done
	"$xfs_build/client" "$xfs_build/mnt" "$xfs_images/linux-v$xfs_version.manifest"
	umount "$xfs_build/mnt"
	wait "$xfs_pid"
	xfs_pid=
	[ "$xfs_before" = "$(cksum -a sha256 "$xfs_image")" ]
	[ ! -e "$xfs_build/mnt/plain.bin" ]
	xfs_image="$xfs_images/linux-v$xfs_version-dirty.img"
	xfs_before=$(cksum -a sha256 "$xfs_image")
	"$xfs_root/puffs/mount_xfs" -s "$xfs_image" "$xfs_build/mnt" > "$xfs_build/server.log" 2>&1 &
	xfs_pid=$!
	xfs_attempt=0
	while kill -0 "$xfs_pid" 2>/dev/null; do
		if [ "$xfs_attempt" -ge 20 ] || [ -e "$xfs_build/mnt/plain.bin" ]; then
			echo 'dirty Linux fixture accepted or stalled' >&2
			exit 1
		fi
		sleep 1
		xfs_attempt=$((xfs_attempt + 1))
	done
	if wait "$xfs_pid"; then echo 'dirty fixture unexpectedly accepted' >&2; exit 1; fi
	xfs_pid=
	grep -q 'journal needs recovery' "$xfs_build/server.log"
	cat "$xfs_build/server.log"
	[ "$xfs_before" = "$(cksum -a sha256 "$xfs_image")" ]
done
echo 'Passed native Linux fixture mounts, unchanged image hashes and dirty startup rejection'
