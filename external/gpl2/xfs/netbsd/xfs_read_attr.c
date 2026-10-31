/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Bounded adaptation of xfs_attr_shortform_verify in xfs_attr_leaf.c. */
#include "xfs_read_internal.h"
#include "xfs_attr.h"
#include "xfs_acl.h"

int
xfs_read_attrs(const struct xfs_reader *reader, const struct xfs_inode_image *inode,
    void *workspace, size_t capacity, xfs_attr_emit_fn emit, void *cookie,
    const char **why)
{
	struct xfs_dinode d;
	const uint8_t *attrs, *entry, *prior;
	uint16_t offsets[UINT8_MAX];
	size_t available, offset, total, pos;
	unsigned int i, j, count, length, flags;
	uint64_t nextents;
	bool parent = false;
	int error;

	if (why != NULL)
		*why = NULL;
	if (reader == NULL || inode == NULL || emit == NULL)
		return xfs_read_fail(why, EINVAL, "null attribute argument");
	if (reader->sb.sb_inodesize < sizeof(d) ||
	    reader->sb.sb_inodesize > sizeof(inode->disk))
		return xfs_read_fail(why, EINVAL, "invalid inode size for attributes");
	memcpy(&d, inode->disk, sizeof(d));
	if (d.di_forkoff == 0)
		return 0;
	available = reader->sb.sb_inodesize - xfs_dinode_size(d.di_version);
	offset = (size_t)d.di_forkoff << 3;
	if (offset >= available)
		return xfs_read_fail(why, EINVAL, "attribute fork offset out of bounds");
	available -= offset;
	attrs = inode->disk + xfs_dinode_size(d.di_version) + offset;
	nextents = d.di_version == 3 && (be64_to_cpu(d.di_flags2) & XFS_DIFLAG2_NREXT64) ?
	    be32_to_cpu(d.di_big_anextents) : be16_to_cpu(d.di_anextents);
	if (d.di_aformat == XFS_DINODE_FMT_EXTENTS && nextents == 0)
		return 0;
	if (d.di_aformat == XFS_DINODE_FMT_EXTENTS || d.di_aformat == XFS_DINODE_FMT_BTREE)
		return xfs_read_attrs_external(reader, inode,
		    xfs_dinode_size(d.di_version) + offset, available, d.di_aformat,
		    nextents, workspace, capacity, emit, cookie, why);
	if (d.di_aformat != XFS_DINODE_FMT_LOCAL)
		return xfs_read_fail(why, EINVAL, "invalid attribute fork format");
	if (nextents != 0 || available < 4)
		return xfs_read_fail(why, EINVAL, "invalid shortform attribute header");
	total = xfs_get16(attrs);
	count = attrs[2];
	if (total < 4 || total > available)
		return xfs_read_fail(why, EINVAL, "shortform attribute size out of bounds");
	/* Ignore the unused header padding byte, as upstream does. */
	pos = 4;
	for (i = 0; i < count; i++) {
		if (total - pos < 3)
			return xfs_read_fail(why, EINVAL, "truncated attribute entry");
		entry = attrs + pos;
		length = 3U + entry[0] + entry[1];
		flags = entry[2];
		if (entry[0] == 0 || length > total - pos)
			return xfs_read_fail(why, EINVAL, "invalid attribute name or value length");
		if ((flags & ~(XFS_ATTR_ROOT | XFS_ATTR_SECURE | XFS_ATTR_PARENT)) ||
		    (flags != 0 && (flags & (flags - 1)) != 0))
			return xfs_read_fail(why, EINVAL, "invalid shortform attribute namespace flags");
		parent |= flags == XFS_ATTR_PARENT;
		if (memchr(entry + 3, 0, entry[0]) != NULL)
			return xfs_read_fail(why, EINVAL, "NUL in attribute name");
		/* Parent pointers have different duplicate-name semantics and are
		 * rejected below. All other namespaces require unique byte names. */
		for (j = 0; flags != XFS_ATTR_PARENT && j < i; j++) {
			prior = attrs + offsets[j];
			if (prior[2] == flags && prior[0] == entry[0] &&
			    memcmp(prior + 3, entry + 3, entry[0]) == 0)
				return xfs_read_fail(why, EINVAL, "duplicate attribute name in namespace");
		}
		offsets[i] = (uint16_t)pos;
		pos += length;
	}
	if (pos != total)
		return xfs_read_fail(why, EINVAL, "shortform attribute count/size mismatch");
	if (parent)
		return xfs_read_fail(why, EOPNOTSUPP, "parent-pointer attributes are unsupported");
	for (i = 0; i < count; i++) {
		entry = attrs + offsets[i];
		error = emit(cookie, entry[2], entry + 3, entry[0],
		    entry + 3 + entry[0], entry[1]);
		if (error != 0)
			return xfs_read_fail(why, error, "attribute callback failed");
	}
	return 0;
}

struct attr_policy {
	const struct xfs_reader *reader;
	const struct xfs_inode_image *inode;
	int acl_error;
	bool root;
	bool unsupported;
};

static int
check_readonly(void *cookie, uint8_t flags, const uint8_t *name, size_t namelen,
    const uint8_t *value, size_t valuelen)
{
	struct attr_policy *policy = cookie;
	static const char *const values[] = { "none", "check", "optimize", "repair" };
	unsigned int i;
	if (flags == 0)
		return 0;
	if (flags == XFS_ATTR_ROOT &&
	    ((namelen == SGI_ACL_FILE_SIZE && memcmp(name, SGI_ACL_FILE, namelen) == 0) ||
	    (namelen == SGI_ACL_DEFAULT_SIZE && memcmp(name, SGI_ACL_DEFAULT, namelen) == 0))) {
		int error = xfs_acl_validate(&policy->reader->sb, value, valuelen,
		    policy->inode->mode, namelen == SGI_ACL_DEFAULT_SIZE, NULL);
		if (error != 0) policy->acl_error = error;
		return 0;
	}
	if (policy->root && flags == XFS_ATTR_ROOT &&
	    namelen == sizeof("xfs:autofsck") - 1 &&
	    memcmp(name, "xfs:autofsck", namelen) == 0) {
		for (i = 0; i < sizeof(values) / sizeof(values[0]); i++)
			if (valuelen == strlen(values[i]) && memcmp(value, values[i], valuelen) == 0)
				return 0;
	}
	policy->unsupported = true;
	return 0;
}

int
xfs_attrs_check_readonly(const struct xfs_reader *reader,
    const struct xfs_inode_image *inode, void *workspace, size_t capacity,
    const char **why)
{
	struct attr_policy policy = { .reader = reader, .inode = inode };
	int error;
	if (reader != NULL && inode != NULL)
		policy.root = inode->number == reader->sb.sb_rootino && S_ISDIR(inode->mode);
	error = xfs_read_attrs(reader, inode, workspace, capacity, check_readonly, &policy, why);
	if (error == 0 && policy.acl_error != 0)
		return xfs_read_fail(why, policy.acl_error, "malformed or inconsistent POSIX ACL");
	if (error == 0 && policy.unsupported)
		return xfs_read_fail(why, EOPNOTSUPP,
		    "security or unrecognized trusted attribute is unsupported");
	return error;
}
