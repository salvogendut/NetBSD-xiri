/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#ifndef _XFS_NETBSD_PROBE_H_
#define _XFS_NETBSD_PROBE_H_

#include "xfs_compat.h"

/*
 * Validate a primary superblock sector and decode it using the ported XFS
 * routines.  device_bytes is the known data-device length, not a sentinel.
 * Return a positive errno and leave *sb unchanged on failure.  *why receives
 * a static diagnostic (or NULL on success), if why is not NULL.
 *
 * Success is only a superblock/geometry check.  It does NOT establish that
 * the log is clean or that any inode, btree, or data block is safe to read.
 * This interface neither mounts a filesystem nor performs any writes.
 */
int xfs_probe_superblock(const void *, size_t, uint64_t, struct xfs_sb *,
    const char **);

#endif /* _XFS_NETBSD_PROBE_H_ */
