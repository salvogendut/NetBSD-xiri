/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_ACL_H_
#define _XFS_NETBSD_ACL_H_
#include "xfs_inode.h"

/* POSIX ACL tag/permission values used by the XFS on-disk format. */
#define XFS_ACL_USER_OBJ  0x01U
#define XFS_ACL_USER      0x02U
#define XFS_ACL_GROUP_OBJ 0x04U
#define XFS_ACL_GROUP     0x08U
#define XFS_ACL_MASK      0x10U
#define XFS_ACL_OTHER     0x20U
#define XFS_ACL_READ      4U
#define XFS_ACL_WRITE     2U
#define XFS_ACL_EXECUTE   1U

/* Validate the entire unaligned disk value, without allocation. Access ACLs
 * must agree with inode mode; default ACLs are allowed only on directories
 * and do not determine the directory's own access permissions. */
int xfs_acl_validate(const struct xfs_sb *, const void *, size_t, uint16_t,
    bool, const char **);

typedef bool (*xfs_acl_group_fn)(void *, uint32_t);
/* Check an immutable value that has passed xfs_acl_validate. No I/O or
 * allocation. IDs are used directly (no Linux namespace/idmap translation).
 * The caller handles privileged credentials and read-only mount policy.
 * A matching owner/named user/group class never falls back to other. */
int xfs_acl_permission(const void *, size_t, uint32_t, uint32_t, uint32_t,
    xfs_acl_group_fn, void *, unsigned int);
#endif
