/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Adapted xfs_bmbt_disk_get_all and read-side fork/btree validation. */
#include "xfs_read_internal.h"

int
xfs_extent_lookup(const struct xfs_sb *sb, const uint8_t *records,
    uint64_t count, uint64_t target, bool exact, uint64_t low, uint64_t high,
    uint64_t *mapped, uint64_t *end, bool *zero, const char **why)
{
	uint64_t i, l0, l1, start, fsb, length, previous = low, dummy;
	bool found = false;

	*mapped = 0;
	*end = high;
	*zero = true;
	if (exact && count == 0)
		return xfs_read_fail(why, EINVAL, "empty bmap leaf");
	for (i = 0; i < count; i++, records += sizeof(struct xfs_bmbt_rec)) {
		l0 = xfs_get64(records);
		l1 = xfs_get64(records + 8);
		start = (l0 & INT64_MAX) >> 9;
		fsb = ((l0 & 511) << 43) | (l1 >> 21);
		length = l1 & ((1U << 21) - 1);
		if (length == 0 || start < previous || start >= high ||
		    length > high - start || (i == 0 && exact && start != low) ||
		    xfs_data_address(sb, fsb, (uint32_t)length, &dummy) != 0)
			return xfs_read_fail(why, EINVAL, "invalid, overlapping or out-of-bounds extent");
		previous = start + length;
		if (!found && target < previous) {
			found = true;
			if (target >= start) {
				*mapped = fsb + target - start;
				*end = previous;
				*zero = (l0 >> 63) != 0;
			} else {
				*end = start;
			}
		}
	}
	return 0;
}

int
xfs_bmap_root(const struct xfs_sb *sb, const uint8_t *root, unsigned int size,
    uint64_t target, unsigned int *level, uint64_t *low, uint64_t *high,
    uint64_t *child, const char **why)
{
	uint64_t records = XFS_MAX_EXTCNT_DATA_FORK_LARGE;
	unsigned int limit = 1, fanout, h;

	h = xfs_crc_enabled(sb) ? XFS_BTREE_LBLOCK_CRC_LEN : XFS_BTREE_LBLOCK_LEN;
	fanout = ((sb->sb_blocksize - h) / 16) / 2;
	/* Conservative height bound: minimum external fanout, one root pointer. */
	while (records > 1) {
		records = (records + fanout - 1) / fanout;
		limit++;
	}
	if (size < sizeof(struct xfs_bmdr_block))
		return xfs_read_fail(why, EINVAL, "truncated bmap root");
	*level = xfs_get16(root);
	if (*level == 0 || *level > limit)
		return xfs_read_fail(why, EINVAL, "invalid bmap root level");
	return xfs_tree_child(sb, root + sizeof(struct xfs_bmdr_block),
	    size - sizeof(struct xfs_bmdr_block), xfs_get16(root + 2),
	    true, 0, target, false, low, high, child, why);
}

static int
file_mapping(const struct xfs_reader *r, const struct xfs_inode_image *inode,
    uint64_t target, void *scratch, size_t capacity, uint64_t *mapped,
    bool *zero, const char **why)
{
	const struct xfs_sb *sb = &r->sb;
	const uint8_t *fork = inode->disk + inode->data_offset;
	uint64_t low = 0, high = UINT64_C(1) << 54, fsb, end;
	unsigned int level, h, n;
	int error;

	if (inode->format == XFS_DINODE_FMT_EXTENTS)
		return xfs_extent_lookup(sb, fork, inode->nextents, target,
		    false, low, high, mapped, &end, zero, why);
	if (inode->format != XFS_DINODE_FMT_BTREE)
		return xfs_read_fail(why, EOPNOTSUPP, "unsupported file data format");
	error = xfs_bmap_root(sb, fork, inode->data_size, target,
	    &level, &low, &high, &fsb, why);
	if (error != 0)
		return error;
	for (;;) {
		level--;
		error = xfs_read_tree(r, fsb, inode->number, true, level, scratch,
		    capacity, &h, &n, why);
		if (error != 0)
			return error;
		if (level == 0) {
			if (n > inode->nextents)
				return xfs_read_fail(why, EINVAL, "bmap leaf exceeds inode extent count");
			return xfs_extent_lookup(sb, (const uint8_t *)scratch + h,
			    n, target, true, low, high, mapped, &end, zero, why);
		}
		error = xfs_tree_child(sb, (const uint8_t *)scratch + h,
		    sb->sb_blocksize - h, n, true, 0, target, true,
		    &low, &high, &fsb, why);
		if (error != 0)
			return error;
	}
}

int
xfs_read_file(const struct xfs_reader *r, const struct xfs_inode_image *inode,
    uint64_t offset, void *data, size_t length, size_t *done, void *scratch,
    size_t capacity, const char **why)
{
	uint64_t fsb;
	size_t copied = 0, part, inblock;
	bool zero;
	int error;

	if (why != NULL)
		*why = NULL;
	if (done == NULL)
		return xfs_read_fail(why, EINVAL, "null file read count");
	*done = 0;
	if (r == NULL || inode == NULL || scratch == NULL || (data == NULL && length))
		return xfs_read_fail(why, EINVAL, "null file read argument");
	if (capacity < r->sb.sb_blocksize)
		return xfs_read_fail(why, EMSGSIZE, "file scratch smaller than block");
	if (!S_ISREG(inode->mode) && !S_ISLNK(inode->mode))
		return xfs_read_fail(why, EOPNOTSUPP, "file reader requires regular file or symlink");
	if (S_ISLNK(inode->mode) && inode->format != XFS_DINODE_FMT_LOCAL)
		return xfs_read_fail(why, EOPNOTSUPP, "remote symlink verification is not implemented");
	if (offset > INT64_MAX)
		return xfs_read_fail(why, EINVAL, "file offset exceeds signed range");
	if (offset >= inode->size || length == 0)
		return 0;
	if (length > inode->size - offset)
		length = inode->size - offset;
	if (inode->format == XFS_DINODE_FMT_LOCAL) {
		memcpy(data, inode->disk + inode->data_offset + offset, length);
		*done = length;
		return 0;
	}
	while (copied < length) {
		inblock = offset & (r->sb.sb_blocksize - 1);
		part = r->sb.sb_blocksize - inblock;
		if (part > length - copied)
			part = length - copied;
		error = file_mapping(r, inode, offset >> r->sb.sb_blocklog,
		    scratch, capacity, &fsb, &zero, why);
		if (error != 0)
			return error;
		if (zero) {
			memset((uint8_t *)data + copied, 0, part);
		} else {
			error = xfs_read_blocks(r, fsb, 1, scratch, capacity, why);
			if (error != 0)
				return error;
			memcpy((uint8_t *)data + copied, (uint8_t *)scratch + inblock, part);
		}
		copied += part;
		offset += part;
	}
	*done = copied;
	return 0;
}
