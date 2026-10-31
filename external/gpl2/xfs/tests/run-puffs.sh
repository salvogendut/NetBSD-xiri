#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
# Run only on NetBSD, as root, with disposable make_puffs_images.py fixtures.
set -eu
if [ "$#" -ne 1 ] || [ "$(uname -s)" != NetBSD ] || [ "$(id -u)" -ne 0 ]; then
	echo "usage (NetBSD, root): sh tests/run-puffs.sh FIXTURE-DIRECTORY" >&2
	exit 2
fi
xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_images=$(CDPATH= cd -- "$1" && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-puffs.XXXXXX")
xfs_pid=
cleanup()
{
	umount "$xfs_build/mnt" 2>/dev/null || true
	if [ -n "$xfs_pid" ]; then
		kill "$xfs_pid" 2>/dev/null || true
		wait "$xfs_pid" 2>/dev/null || true
	fi
	# Never recursively remove a directory that could still contain a mount.
	if rmdir "$xfs_build/mnt"; then
		rm -rf "$xfs_build"
	fi
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir "$xfs_build/mnt"
make -C "$xfs_root/puffs" MKMAN=no
${CC:-cc} -O2 -g -Wall -Wextra -Werror "$xfs_root/tests/t_puffs.c" -o "$xfs_build/t_puffs"

start_server()
{
	"$xfs_root/puffs/mount_xfs" -s "$1" "$xfs_build/mnt" > "$xfs_build/server.log" 2>&1 &
	xfs_pid=$!
	xfs_attempt=0
	until [ -r "$xfs_build/mnt/hello" ]; do
		if ! kill -0 "$xfs_pid" 2>/dev/null || [ "$xfs_attempt" -ge 20 ]; then
			cat "$xfs_build/server.log" >&2
			echo 'PUFFS server did not become ready' >&2
			exit 1
		fi
		sleep 1
		xfs_attempt=$((xfs_attempt + 1))
	done
}

for xfs_version in 4 5; do
	xfs_image="$xfs_images/v$xfs_version.img"
	xfs_before=$(cksum -a sha256 "$xfs_image")
	start_server "$xfs_image"
	"$xfs_build/t_puffs" "$xfs_build/mnt" "$xfs_version"
	if [ "$xfs_version" = 4 ]; then
		# Detect an external metadata change and reject later operations.
		touch -m "$xfs_image"
		"$xfs_build/t_puffs" "$xfs_build/mnt" stale
		kill -TERM "$xfs_pid"
	else
		umount "$xfs_build/mnt"
	fi
	wait "$xfs_pid"
	xfs_pid=
	[ "$xfs_before" = "$(cksum -a sha256 "$xfs_image")" ]
	[ ! -e "$xfs_build/mnt/hello" ]
done

for xfs_bad in root-attr root-acl root-bad-attr bad-log; do
	"$xfs_root/puffs/mount_xfs" -s "$xfs_images/$xfs_bad.img" "$xfs_build/mnt" > "$xfs_build/server.log" 2>&1 &
	xfs_pid=$!
	xfs_attempt=0
	while kill -0 "$xfs_pid" 2>/dev/null; do
		if [ "$xfs_attempt" -ge 20 ] || [ -e "$xfs_build/mnt/hello" ]; then
			echo "server accepted or stalled on $xfs_bad" >&2
			exit 1
		fi
		sleep 1
		xfs_attempt=$((xfs_attempt + 1))
	done
	if wait "$xfs_pid"; then
		echo "server did not reject $xfs_bad" >&2
		exit 1
	fi
	xfs_pid=
	case $xfs_bad in
	root-attr|root-acl) grep -q 'unrecognized trusted attribute' "$xfs_build/server.log" ;;
	root-bad-attr) grep -q 'attribute' "$xfs_build/server.log" ;;
	bad-log) grep -q 'log\|journal' "$xfs_build/server.log" ;;
	esac
	cat "$xfs_build/server.log"
done
echo 'Passed native PUFFS mounts, unmounts, image hashes and startup rejection'
