/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_ATTR_H_
#define _XFS_NETBSD_ATTR_H_
#include "xfs_inode.h"

/* Namespace bits from dist/xfs_da_format.h; zero denotes user attributes. */
#define XFS_ATTR_ROOT 2U
#define XFS_ATTR_SECURE 4U
#define XFS_ATTR_PARENT 8U
/* Default server budget, including mapped blocks, indexes and value staging. */
#define XFS_ATTR_WORKSPACE_SIZE (8U * 1024 * 1024)

typedef int (*xfs_attr_emit_fn)(void *, uint8_t, const uint8_t *, size_t,
    const uint8_t *, size_t);

/* Read a verified immutable inode's attributes. No hidden allocation.
 * Validate the whole fork before emitting anything. Names are non-NUL byte
 * strings; values are arbitrary bytes (including empty). Pointers refer to
 * inode/workspace storage and are valid only during the callback. A callback
 * error stops iteration and is returned unchanged. Inline/empty forks need no
 * workspace or I/O. External forks require caller-owned, disjoint workspace
 * and return EMSGSIZE if it cannot hold all mapped blocks and indexes.
 * Enumeration order is unspecified.
 * Parent pointers remain unsupported; names within a namespace must be unique.
 */
int xfs_read_attrs(const struct xfs_reader *, const struct xfs_inode_image *,
    void *, size_t, xfs_attr_emit_fn, void *, const char **);

/* Mount policy, separate from structural decoding: permit user attributes,
 * valid POSIX ACLs and known root-directory xfs:autofsck values. The caller
 * MUST enforce access ACLs using xfs_acl_permission; successful validation
 * alone does not authorize access. Default ACLs are directory metadata only
 * on a read-only mount. Reject security labels and unknown trusted attrs.
 */
int xfs_attrs_check_readonly(const struct xfs_reader *,
    const struct xfs_inode_image *, void *, size_t, const char **);
#endif
