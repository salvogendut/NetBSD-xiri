/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (c) 2008, Christoph Hellwig. All Rights Reserved.
 * Copyright (C) 2002,2003 Andreas Gruenbacher <a.gruenbacher@computer.org> */
/* $NetBSD$ */
/* Adapted xfs_acl_from_disk, posix_acl_valid/equiv_mode/permission. */
#include "xfs_read_internal.h"
#include "xfs_acl.h"

_Static_assert(sizeof(struct xfs_acl) == 4, "ACL count header");
_Static_assert(sizeof(struct xfs_acl_entry) == 12, "ACL disk entry");

int
xfs_acl_validate(const struct xfs_sb *sb, const void *value, size_t length,
    uint16_t mode, bool default_acl, const char **why)
{
	const uint8_t *data = value, *entry, *prior;
	unsigned int count, limit, i, j, tag, perm, state = XFS_ACL_USER_OBJ;
	unsigned int aclmode = 0;
	uint32_t id, maxid = 0;
	bool named = false, have_id = false;
	if (why != NULL) *why = NULL;
	if (sb == NULL || value == NULL || length < 4)
		return xfs_read_fail(why, EINVAL, "truncated ACL header");
	limit = xfs_crc_enabled(sb) ? (XFS_XATTR_SIZE_MAX - 4) / 12 : 25;
	count = xfs_get32(data);
	if (count < 3 || count > limit || length != 4 + (size_t)count * 12)
		return xfs_read_fail(why, EINVAL, "invalid ACL count or length");
	if ((!S_ISDIR(mode) && !S_ISREG(mode)) || (default_acl && !S_ISDIR(mode)))
		return xfs_read_fail(why, EINVAL, "ACL incompatible with inode type");
	for (i = 0; i < count; i++) {
		entry = data + 4 + i * 12;
		tag = xfs_get32(entry);
		id = xfs_get32(entry + 4);
		perm = xfs_get16(entry + 8);
		if (perm & ~7U)
			return xfs_read_fail(why, EINVAL, "invalid ACL permissions");
		/* Unused IDs and entry padding are ignored, as upstream does. */
		switch (tag) {
		case XFS_ACL_USER_OBJ:
			if (state != XFS_ACL_USER_OBJ) goto order;
			state = XFS_ACL_USER;
			aclmode = perm << 6;
			break;
		case XFS_ACL_USER:
		case XFS_ACL_GROUP:
			if (state != tag) goto order;
			if (id == UINT32_MAX)
				return xfs_read_fail(why, EINVAL, "undefined named ACL identity");
			/* Sorted IDs take the fast path. Accept unsorted lists, but
			 * reject duplicates; the disk-format count bounds this scan. */
			if (have_id && id <= maxid) {
				for (j = 0; j < i; j++) {
					prior = data + 4 + j * 12;
					if (xfs_get32(prior) == tag && xfs_get32(prior + 4) == id)
						return xfs_read_fail(why, EINVAL, "duplicate named ACL identity");
				}
			}
			if (!have_id || id > maxid) maxid = id;
			have_id = named = true;
			break;
		case XFS_ACL_GROUP_OBJ:
			if (state != XFS_ACL_USER) goto order;
			state = XFS_ACL_GROUP;
			have_id = false;
			aclmode |= perm << 3;
			break;
		case XFS_ACL_MASK:
			if (state != XFS_ACL_GROUP) goto order;
			state = XFS_ACL_OTHER;
			aclmode = (aclmode & ~070U) | (perm << 3);
			break;
		case XFS_ACL_OTHER:
			if (state != XFS_ACL_OTHER && !(state == XFS_ACL_GROUP && !named))
				goto order;
			state = 0;
			aclmode |= perm;
			break;
		default:
			goto order;
		}
	}
	if (state != 0) goto order;
	if (!default_acl && aclmode != (mode & 0777))
		return xfs_read_fail(why, EINVAL, "access ACL disagrees with inode mode");
	return 0;
order:
	return xfs_read_fail(why, EINVAL, "invalid ACL tag, order or required entry");
}

int
xfs_acl_permission(const void *value, size_t length, uint32_t owner,
    uint32_t group, uint32_t uid, xfs_acl_group_fn hasgroup, void *cookie,
    unsigned int want)
{
	const uint8_t *data = value, *entry;
	unsigned int count, i, tag, perm, mask = 7;
	bool matched_group = false;
	if (value == NULL || hasgroup == NULL || length < 4 || (want & ~7U))
		return EINVAL;
	count = xfs_get32(data);
	if (count < 3 || count > (XFS_XATTR_SIZE_MAX - 4) / 12 ||
	    length != 4 + (size_t)count * 12)
		return EINVAL;
	/* Validation guarantees the optional mask immediately precedes other. */
	entry = data + 4 + (count - 2) * 12;
	if (xfs_get32(entry) == XFS_ACL_MASK) mask = xfs_get16(entry + 8);
	for (i = 0; i < count; i++) {
		entry = data + 4 + i * 12;
		tag = xfs_get32(entry);
		perm = xfs_get16(entry + 8);
		switch (tag) {
		case XFS_ACL_USER_OBJ:
			if (uid == owner) return (perm & want) == want ? 0 : EACCES;
			break;
		case XFS_ACL_USER:
			if (uid == xfs_get32(entry + 4))
				return (perm & mask & want) == want ? 0 : EACCES;
			break;
		case XFS_ACL_GROUP_OBJ:
		case XFS_ACL_GROUP:
			if (hasgroup(cookie, tag == XFS_ACL_GROUP_OBJ ? group : xfs_get32(entry + 4))) {
				matched_group = true;
				/* One matching group must grant the whole request; do not
				 * combine partial grants from different groups. */
				if ((perm & want) == want)
					return (perm & mask & want) == want ? 0 : EACCES;
			}
			break;
		case XFS_ACL_MASK:
			break;
		case XFS_ACL_OTHER:
			return !matched_group && (perm & want) == want ? 0 : EACCES;
		default:
			return EINVAL;
		}
	}
	return EINVAL;
}
