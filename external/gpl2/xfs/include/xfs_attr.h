/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_ATTR_H_
#define _XFS_NETBSD_ATTR_H_
#include "xfs_inode.h"

/* Namespace bits from dist/xfs_da_format.h; zero denotes user attributes. */
#define XFS_ATTR_ROOT 2U
#define XFS_ATTR_SECURE 4U
#define XFS_ATTR_PARENT 8U

typedef int (*xfs_attr_emit_fn)(void *, uint8_t, const uint8_t *, size_t,
    const uint8_t *, size_t);

/* Read a verified immutable inode's shortform attributes. No I/O/allocation.
 * Validate the whole fork before emitting anything. Names are non-NUL byte
 * strings; values are arbitrary bytes (including empty). Pointers refer to
 * inode storage. A callback error stops iteration and is returned unchanged.
 * Empty extent forks are accepted; external blocks and parent pointers are
 * unsupported. Duplicate names within a namespace are rejected.
 */
int xfs_read_attrs(const struct xfs_reader *, const struct xfs_inode_image *,
    xfs_attr_emit_fn, void *, const char **);

/* Mount policy, separate from structural decoding: permit user attributes
 * and known root-directory xfs:autofsck values only. Reject ACLs, security
 * labels and unknown trusted attributes before making a node accessible.
 */
int xfs_attrs_check_readonly(const struct xfs_reader *,
    const struct xfs_inode_image *, const char **);
#endif
