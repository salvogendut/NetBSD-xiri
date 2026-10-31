/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Read-side adaptations of xfs_btree, xfs_inobt and xfs_bmbt verifiers. */
#include "xfs_read_internal.h"

int
xfs_data_address(const struct xfs_sb *sb, uint64_t fsb, uint32_t count,
    uint64_t *offset)
{
	uint64_t agno = fsb >> sb->sb_agblklog, base;
	uint32_t agbno = fsb & ((UINT64_C(1) << sb->sb_agblklog) - 1);
	uint32_t blocks, first;

	if (agno >= sb->sb_agcount || count == 0 ||
	    xfs_ag_geometry(sb, (uint32_t)agno, &base, &blocks) != 0)
		return EINVAL;
	first = (4U * sb->sb_sectsize + sb->sb_blocksize - 1) / sb->sb_blocksize;
	if (agbno < first || agbno >= blocks || count > blocks - agbno)
		return EINVAL;
	if (fsb < sb->sb_logstart + sb->sb_logblocks &&
	    fsb + count > sb->sb_logstart)
		return EINVAL;
	*offset = base + (uint64_t)agbno * sb->sb_blocksize;
	return 0;
}

int
xfs_read_tree(const struct xfs_reader *r, uint64_t fsb, uint64_t owner,
    bool bmap, unsigned int level, void *scratch, size_t capacity,
    unsigned int *header, unsigned int *records, const char **why)
{
	const struct xfs_sb *sb = &r->sb;
	struct xfs_btree_block b;
	bool crc = xfs_crc_enabled(sb);
	uint64_t offset, siblings[2], dummy, actual_owner, blkno;
	uint32_t magic;
	unsigned int h, n, i, width;
	const void *uuid;
	int error;

	if (xfs_data_address(sb, fsb, 1, &offset) != 0)
		return xfs_read_fail(why, EINVAL, "btree address out of bounds");
	error = xfs_read_blocks(r, fsb, 1, scratch, capacity, why);
	if (error != 0)
		return error;
	if (crc && xfs_verify_metadata_crc(scratch, sb->sb_blocksize,
	    bmap ? XFS_BTREE_LBLOCK_CRC_OFF : XFS_BTREE_SBLOCK_CRC_OFF) != 0)
		return xfs_read_fail(why, EINVAL, "btree CRC32C mismatch");
	memcpy(&b, scratch, sizeof(b));
	magic = bmap ? (crc ? XFS_BMAP_CRC_MAGIC : XFS_BMAP_MAGIC) :
	    (crc ? XFS_IBT_CRC_MAGIC : XFS_IBT_MAGIC);
	if (be32_to_cpu(b.bb_magic) != magic || be16_to_cpu(b.bb_level) != level)
		return xfs_read_fail(why, EINVAL, "btree magic or level mismatch");
	if (bmap) {
		h = crc ? XFS_BTREE_LBLOCK_CRC_LEN : XFS_BTREE_LBLOCK_LEN;
		siblings[0] = be64_to_cpu(b.bb_u.l.bb_leftsib);
		siblings[1] = be64_to_cpu(b.bb_u.l.bb_rightsib);
		actual_owner = be64_to_cpu(b.bb_u.l.bb_owner);
		blkno = be64_to_cpu(b.bb_u.l.bb_blkno);
		uuid = &b.bb_u.l.bb_uuid;
	} else {
		h = crc ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN;
		siblings[0] = be32_to_cpu(b.bb_u.s.bb_leftsib);
		siblings[1] = be32_to_cpu(b.bb_u.s.bb_rightsib);
		actual_owner = be32_to_cpu(b.bb_u.s.bb_owner);
		blkno = be64_to_cpu(b.bb_u.s.bb_blkno);
		uuid = &b.bb_u.s.bb_uuid;
	}
	if (crc && (actual_owner != owner || blkno != offset / 512 ||
	    memcmp(uuid, &sb->sb_meta_uuid, 16) != 0))
		return xfs_read_fail(why, EINVAL, "btree owner, address or UUID mismatch");
	for (i = 0; i < 2; i++) {
		if (siblings[i] == (bmap ? NULLFSBLOCK : NULLAGBLOCK))
			continue;
		if (!bmap) {
			if (siblings[i] >= sb->sb_agblocks)
				return xfs_read_fail(why, EINVAL, "btree sibling out of bounds");
			siblings[i] |= owner << sb->sb_agblklog;
		}
		if (siblings[i] == fsb ||
		    xfs_data_address(sb, siblings[i], 1, &dummy) != 0)
			return xfs_read_fail(why, EINVAL, "btree sibling out of bounds");
	}
	n = be16_to_cpu(b.bb_numrecs);
	width = level != 0 && !bmap ? 8 : 16;
	if (n > (sb->sb_blocksize - h) / width || (level != 0 && n == 0))
		return xfs_read_fail(why, EINVAL, "invalid btree record count");
	*header = h;
	*records = n;
	return 0;
}

/*
 * Keys and pointers occupy separate arrays sized by capacity, not numrecs.
 * Carry the selected parent's key interval down to verify child contents.
 * An exact first-key match is required below the root. Levels decrease on
 * every read, so corrupt cycles cannot cause an unbounded walk.
 */
int
xfs_tree_child(const struct xfs_sb *sb, const uint8_t *keys,
    unsigned int space, unsigned int n, bool bmap, uint32_t agno,
    uint64_t target, bool exact, uint64_t *low, uint64_t *high,
    uint64_t *child, const char **why)
{
	unsigned int width = bmap ? 8 : 4, max = space / (2 * width), i, chosen = 0;
	uint64_t key, previous = 0, ptr, dummy, nextlow = 0, nexthigh = *high;
	const uint8_t *ptrs = keys + max * width;

	if (n == 0 || n > max)
		return xfs_read_fail(why, EINVAL, "invalid btree node count");
	for (i = 0; i < n; i++) {
		key = bmap ? xfs_get64(keys + i * width) : xfs_get32(keys + i * width);
		ptr = bmap ? xfs_get64(ptrs + i * width) : xfs_get32(ptrs + i * width);
		if (key < *low || key >= *high || (i != 0 && key <= previous) ||
		    (i == 0 && exact && key != *low))
			return xfs_read_fail(why, EINVAL, "btree keys out of order or parent range");
		if (!bmap) {
			if (ptr >= sb->sb_agblocks || (key & 63) != 0)
				return xfs_read_fail(why, EINVAL, "invalid inode btree key or pointer");
			ptr |= (uint64_t)agno << sb->sb_agblklog;
		}
		if (xfs_data_address(sb, ptr, 1, &dummy) != 0)
			return xfs_read_fail(why, EINVAL, "btree child out of bounds");
		if (i == 0 || key <= target) {
			chosen = i;
			nextlow = key;
			*child = ptr;
		} else if (i == chosen + 1) {
			nexthigh = key;
		}
		previous = key;
	}
	*low = nextlow;
	*high = nexthigh;
	return 0;
}
