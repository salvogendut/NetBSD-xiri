#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
# Native mount guard tests using test_log_v1.py's disposable fixtures.
set -eu
if [ "$#" -ne 1 ] || [ "$(uname -s)" != NetBSD ] || [ "$(id -u)" -ne 0 ]; then
	echo 'usage (NetBSD, root): sh tests/run-v1-puffs.sh FIXTURE-DIRECTORY' >&2
	exit 2
fi
xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_images=$(CDPATH= cd -- "$1" && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-v1.XXXXXX")
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
for xfs_case in mkfs-v1 irix-modern irix-string irix-wrapped irix-dirty \
    irix-torn irix-bad-unmount irix-bad-frame irix-identity; do
	xfs_image="$xfs_images/$xfs_case.img"
	xfs_before=$(cksum -a sha256 "$xfs_image")
	"$xfs_root/puffs/mount_xfs" -s "$xfs_image" "$xfs_build/mnt" > "$xfs_build/server.log" 2>&1 &
	xfs_pid=$!
	xfs_attempt=0
	case $xfs_case in
	mkfs-v1|irix-modern|irix-string|irix-wrapped)
		until [ -r "$xfs_build/mnt/hello" ]; do
			if ! kill -0 "$xfs_pid" 2>/dev/null || [ "$xfs_attempt" -ge 20 ]; then
				cat "$xfs_build/server.log" >&2
				echo "$xfs_case: mount did not become ready" >&2
				exit 1
			fi
			sleep 1
			xfs_attempt=$((xfs_attempt + 1))
		done
		cmp "$xfs_images/contents" "$xfs_build/mnt/hello"
		cmp "$xfs_images/contents" "$xfs_build/mnt/sub/child"
		cmp "$xfs_images/contents" "$xfs_build/mnt/link"
		[ "$(readlink "$xfs_build/mnt/link")" = hello ]
		[ "$(stat -f '%u:%g' "$xfs_build/mnt/hello")" = '12:34' ]
		[ "$(ls "$xfs_build/mnt/sub")" = child ]
		umount "$xfs_build/mnt"
		wait "$xfs_pid"
		;;
	*)
		while kill -0 "$xfs_pid" 2>/dev/null; do
			if [ "$xfs_attempt" -ge 20 ] || [ -e "$xfs_build/mnt/hello" ]; then
				echo "$xfs_case: invalid journal accepted or stalled" >&2
				exit 1
			fi
			sleep 1
			xfs_attempt=$((xfs_attempt + 1))
		done
		if wait "$xfs_pid"; then echo "$xfs_case: unexpectedly accepted" >&2; exit 1; fi
		grep -q 'log\|journal' "$xfs_build/server.log"
		if [ "$xfs_case" = irix-dirty ]; then
			grep -q 'journal needs recovery' "$xfs_build/server.log"
		fi
		cat "$xfs_build/server.log"
		;;
	esac
	xfs_pid=
	[ ! -e "$xfs_build/mnt/hello" ]
	[ "$xfs_before" = "$(cksum -a sha256 "$xfs_image")" ]
	echo "Passed native v1 fixture $xfs_case; image unchanged"
done
