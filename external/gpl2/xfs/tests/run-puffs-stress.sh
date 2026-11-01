#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-only
# $NetBSD$
# Run only on NetBSD, as root, with disposable make_puffs_images.py fixtures.
set -eu
xfs_pressure=0
if [ "${1:-}" = --vnode-pressure ]; then
	xfs_pressure=1
	shift
fi
if [ "$#" -ne 1 ] || [ "$(uname -s)" != NetBSD ] || [ "$(id -u)" -ne 0 ]; then
	echo 'usage (NetBSD, root): sh tests/run-puffs-stress.sh [--vnode-pressure] FIXTURE-DIRECTORY' >&2
	echo '--vnode-pressure temporarily changes kern.maxvnodes; use a disposable VM only' >&2
	exit 2
fi
xfs_root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
xfs_images=$(CDPATH= cd -- "$1" && pwd)
xfs_build=$(mktemp -d "${TMPDIR:-/tmp}/netbsd-xfs-stress.XXXXXX")
xfs_old_vnodes=
xfs_pid=
cleanup()
{
	xfs_result=$?
	trap - EXIT
	if [ -n "$xfs_pid" ]; then
		kill -TERM "$xfs_pid" 2>/dev/null || true
		wait "$xfs_pid" 2>/dev/null || true
	fi
	umount -f "$xfs_build/mnt" 2>/dev/null || true
	if [ -n "$xfs_old_vnodes" ]; then
		sysctl -w "kern.maxvnodes=$xfs_old_vnodes" || xfs_result=1
	fi
	# Never recursively remove a directory that could still contain a mount.
	if rmdir "$xfs_build/mnt"; then
		if [ "$xfs_result" -eq 0 ]; then
			rm -rf "$xfs_build"
		else
			echo "Stress diagnostics retained in $xfs_build" >&2
		fi
	else
		xfs_result=1
		echo "Could not remove mountpoint; diagnostics retained in $xfs_build" >&2
	fi
	exit "$xfs_result"
}
trap cleanup EXIT
trap 'exit 1' HUP INT TERM
mkdir "$xfs_build/mnt"
make -C "$xfs_root/puffs" MKMAN=no
${CC:-cc} -O2 -g -Wall -Wextra -Werror "$xfs_root/tests/t_puffs_stress.c" -o "$xfs_build/t_puffs_stress"
if [ "$xfs_pressure" -eq 1 ]; then
	xfs_old_vnodes=$(sysctl -n kern.maxvnodes)
	sysctl -w kern.maxvnodes=1024
fi
for xfs_version in 4 5; do
	xfs_image="$xfs_images/v$xfs_version.img"
	xfs_before=$(cksum -a sha256 "$xfs_image")
	"$xfs_build/t_puffs_stress" "$xfs_root/puffs/mount_xfs" "$xfs_image" \
	    "$xfs_build/mnt" "$xfs_build/v$xfs_version.log" "$xfs_pressure" &
	xfs_pid=$!
	# Clear the PID before handling failure; a reaped PID could be reused.
	xfs_status=0
	wait "$xfs_pid" || xfs_status=$?
	xfs_pid=
	[ "$xfs_status" -eq 0 ] || exit "$xfs_status"
	[ "$xfs_before" = "$(cksum -a sha256 "$xfs_image")" ]
	echo "Passed XFS v$xfs_version stress and unchanged whole-image SHA256"
done
