/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Adapted from xfs_inode_buf.c, xfs_inode_fork.c and xfs_ialloc.c. */
#include "xfs_read_internal.h"

_Static_assert(sizeof(struct xfs_dinode) == 176, "v3 inode layout");
_Static_assert(XFS_DINODE_CRC_OFF == 100, "v2 inode core size");

static unsigned int
bitcount(uint64_t n)
{
	unsigned int count = 0;
	while (n != 0) {
		n &= n - 1;
		count++;
	}
	return count;
}

static int
inode_allocated(const struct xfs_reader *r, uint32_t agno, uint32_t agino,
    void *scratch, size_t capacity, const char **why)
{
	struct xfs_ag_headers ag;
	const struct xfs_sb *sb = &r->sb;
	const uint8_t *p;
	uint64_t fsb, low = 0, high, start, previous, holes, freebits, base, dummy;
	uint32_t blocks, freecount, count, holemask;
	unsigned int level, n, h, i, j;
	bool exact = false, allocated = false;
	int error;

	error = xfs_read_ag(r, agno, scratch, capacity, &ag, why);
	if (error != 0)
		return error;
	error = xfs_ag_geometry(sb, agno, &base, &blocks);
	if (error != 0)
		return error;
	base = (uint64_t)agno << sb->sb_agblklog;
	high = (uint64_t)blocks << sb->sb_inopblog;
	if (high > UINT32_MAX)
		high = UINT32_MAX;
	level = be32_to_cpu(ag.agi.agi_level) - 1;
	fsb = base | be32_to_cpu(ag.agi.agi_root);
	for (;;) {
		error = xfs_read_tree(r, fsb, agno, false, level, scratch,
		    capacity, &h, &n, why);
		if (error != 0)
			return error;
		p = scratch;
		if (!exact && (xfs_get32(p + 8) != NULLAGBLOCK ||
		    xfs_get32(p + 12) != NULLAGBLOCK))
			return xfs_read_fail(why, EINVAL, "inode btree root has siblings");
		p += h;
		if (level == 0)
			break;
		error = xfs_tree_child(sb, p, sb->sb_blocksize - h, n, false,
		    agno, agino, exact, &low, &high, &fsb, why);
		if (error != 0)
			return error;
		exact = true;
		level--;
	}
	if (exact && n == 0)
		return xfs_read_fail(why, EINVAL, "empty inode btree child");
	previous = low;
	for (i = 0; i < n; i++, p += sizeof(xfs_inobt_rec_t)) {
		start = xfs_get32(p);
		if ((start & 63) != 0 || start < previous || start >= high ||
		    high - start < 64 || (i == 0 && exact && start != low) ||
		    xfs_data_address(sb, base | (start >> sb->sb_inopblog),
		    1, &dummy) != 0)
			return xfs_read_fail(why, EINVAL, "invalid inode chunk range");
		previous = start + 64;
		freebits = xfs_get64(p + 8);
		holes = 0;
		if (xfs_crc_enabled(sb) &&
		    (sb->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_SPINODES)) {
			holemask = xfs_get16(p + 4);
			for (j = 0; j < 16; j++)
				if (holemask & (1U << j))
					holes |= UINT64_C(15) << (j * 4);
			count = p[6];
			freecount = p[7];
		} else {
			count = 64;
			freecount = xfs_get32(p + 4);
		}
		if (count == 0 || count != 64 - bitcount(holes) ||
		    freecount != bitcount(freebits & ~holes))
			return xfs_read_fail(why, EINVAL, "invalid inode chunk counters");
		if (agino >= start && agino < start + 64)
			allocated = ((freebits | holes) &
			    (UINT64_C(1) << (agino - start))) == 0;
	}
	return allocated ? 0 : xfs_read_fail(why, ENOENT, "inode is not allocated");
}

/* Validate fork geometry before inspecting any data at the fork offset. */
static bool
valid_fork(unsigned int format, uint64_t extents, unsigned int size)
{
	switch (format) {
	case XFS_DINODE_FMT_LOCAL:
		return extents == 0;
	case XFS_DINODE_FMT_EXTENTS:
		return extents <= size / sizeof(struct xfs_bmbt_rec);
	case XFS_DINODE_FMT_BTREE:
		return size >= 20 && extents > size / sizeof(struct xfs_bmbt_rec);
	default:
		return false;
	}
}

int
xfs_read_inode(const struct xfs_reader *r, uint64_t ino, void *scratch,
    size_t capacity, struct xfs_inode_image *result, const char **why)
{
	struct xfs_dinode d;
	const struct xfs_sb *sb;
	const uint8_t *raw;
	uint64_t agno, agino, fsb, offset, size, nex, naex, blocks, flags2 = 0;
	uint64_t mapped, end;
	unsigned int core, available, dsize, asize, flags, mode, hint, cowhint;
	bool crc, large, unwritten;
	int error;

	if (why != NULL)
		*why = NULL;
	if (r == NULL || scratch == NULL || result == NULL)
		return xfs_read_fail(why, EINVAL, "null inode argument");
	sb = &r->sb;
	if (capacity < sb->sb_blocksize)
		return xfs_read_fail(why, EMSGSIZE, "inode scratch smaller than block");
	if (xfs_crc_enabled(sb) &&
	    (sb->sb_features_ro_compat & XFS_SB_FEAT_RO_COMPAT_UNKNOWN))
		return xfs_read_fail(why, EOPNOTSUPP, "unknown read-only feature for inode access");
	fsb = ino >> sb->sb_inopblog;
	agno = fsb >> sb->sb_agblklog;
	agino = ino & ((UINT64_C(1) << (sb->sb_agblklog + sb->sb_inopblog)) - 1);
	if (ino == 0 || agno >= sb->sb_agcount || agino >= NULLAGINO ||
	    xfs_data_address(sb, fsb, 1, &offset) != 0)
		return xfs_read_fail(why, EINVAL, "inode number out of bounds");
	error = inode_allocated(r, (uint32_t)agno, (uint32_t)agino,
	    scratch, capacity, why);
	if (error != 0)
		return error;
	error = xfs_read_blocks(r, fsb, 1, scratch, capacity, why);
	if (error != 0)
		return error;
	raw = (const uint8_t *)scratch +
	    (ino & (sb->sb_inopblock - 1)) * sb->sb_inodesize;
	memcpy(&d, raw, sizeof(d));
	crc = xfs_crc_enabled(sb);
	if (be16_to_cpu(d.di_magic) != XFS_DINODE_MAGIC ||
	    (crc ? d.di_version != 3 : (d.di_version != 1 && d.di_version != 2)))
		return xfs_read_fail(why, EINVAL, "invalid inode magic or version");
	if (crc && xfs_verify_metadata_crc(raw, sb->sb_inodesize, XFS_DINODE_CRC_OFF) != 0)
		return xfs_read_fail(why, EINVAL, "inode CRC32C mismatch");
	if (crc && (be64_to_cpu(d.di_ino) != ino ||
	    memcmp(&d.di_uuid, &sb->sb_meta_uuid, 16) != 0))
		return xfs_read_fail(why, EINVAL, "inode number or UUID mismatch");
	mode = be16_to_cpu(d.di_mode);
	size = be64_to_cpu(d.di_size);
	if (size > INT64_MAX || mode == 0 || (d.di_version >= 2 && d.di_onlink))
		return xfs_read_fail(why, EINVAL, "invalid inode mode, size or link field");
	if ((S_ISLNK(mode) || S_ISDIR(mode)) && size == 0)
		return xfs_read_fail(why, EOPNOTSUPP, "empty directory or symlink is unsupported");
	if (crc)
		flags2 = be64_to_cpu(d.di_flags2);
	large = (flags2 & XFS_DIFLAG2_NREXT64) != 0;
	if ((flags2 & ~XFS_DIFLAG2_ANY) ||
	    (large && (!(sb->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_NREXT64) ||
	    d.di_nrext64_pad)) || (!large && crc && d.di_v3_pad) ||
	    ((flags2 & XFS_DIFLAG2_BIGTIME) &&
	    !(sb->sb_features_incompat & XFS_SB_FEAT_INCOMPAT_BIGTIME)))
		return xfs_read_fail(why, EINVAL, "invalid inode feature flags or extent padding");
	nex = large ? be64_to_cpu(d.di_big_nextents) : be32_to_cpu(d.di_nextents);
	naex = large ? be32_to_cpu(d.di_big_anextents) : be16_to_cpu(d.di_anextents);
	blocks = be64_to_cpu(d.di_nblocks);
	if (nex > (large ? XFS_MAX_EXTCNT_DATA_FORK_LARGE : XFS_MAX_EXTCNT_DATA_FORK_SMALL) ||
	    naex > (large ? XFS_MAX_EXTCNT_ATTR_FORK_LARGE : XFS_MAX_EXTCNT_ATTR_FORK_SMALL) ||
	    nex + naex > blocks || (nex + naex == 0 && blocks != 0))
		return xfs_read_fail(why, EINVAL, "invalid inode extent counters");
	core = xfs_dinode_size(d.di_version);
	available = sb->sb_inodesize - core;
	dsize = d.di_forkoff ? (unsigned int)d.di_forkoff << 3 : available;
	if (dsize > available || (d.di_forkoff && dsize == available))
		return xfs_read_fail(why, EINVAL, "inode fork offset out of bounds");
	asize = available - dsize;
	if (asize ? !valid_fork(d.di_aformat, naex, asize) :
	    (naex != 0 || (d.di_aformat != 0 && d.di_aformat != XFS_DINODE_FMT_EXTENTS)))
		return xfs_read_fail(why, EINVAL, "invalid attribute fork geometry");
	if (S_ISREG(mode) || S_ISDIR(mode) || S_ISLNK(mode)) {
		if (!valid_fork(d.di_format, nex, dsize) ||
		    (d.di_format == XFS_DINODE_FMT_LOCAL && (S_ISREG(mode) || size > dsize)) ||
		    (S_ISDIR(mode) && size <= dsize && d.di_format != XFS_DINODE_FMT_LOCAL))
			return xfs_read_fail(why, EINVAL, "invalid data fork geometry");
	} else if (S_ISCHR(mode) || S_ISBLK(mode) || S_ISFIFO(mode) || S_ISSOCK(mode)) {
		if (d.di_format != XFS_DINODE_FMT_DEV || (d.di_forkoff && d.di_forkoff != 1))
			return xfs_read_fail(why, EINVAL, "invalid device inode fork");
	} else {
		return xfs_read_fail(why, EINVAL, "unknown inode type");
	}
	flags = be16_to_cpu(d.di_flags);
	if (flags & XFS_DIFLAG_REALTIME)
		return xfs_read_fail(why, EOPNOTSUPP, "realtime inode is unsupported");
	hint = be32_to_cpu(d.di_extsize);
	cowhint = crc ? be32_to_cpu(d.di_cowextsize) : 0;
	if (((flags & XFS_DIFLAG_EXTSIZE) && !S_ISREG(mode)) ||
	    ((flags & XFS_DIFLAG_EXTSZINHERIT) && !S_ISDIR(mode)) ||
	    (!!(flags & (XFS_DIFLAG_EXTSIZE | XFS_DIFLAG_EXTSZINHERIT)) != (hint != 0)) ||
	    hint > XFS_MAX_BMBT_EXTLEN || hint > sb->sb_agblocks / 2 ||
	    (!!(flags2 & XFS_DIFLAG2_COWEXTSIZE) != (cowhint != 0)) ||
	    cowhint > XFS_MAX_BMBT_EXTLEN || cowhint > sb->sb_agblocks / 2 ||
	    ((flags2 & XFS_DIFLAG2_COWEXTSIZE) && !(S_ISREG(mode) || S_ISDIR(mode))) ||
	    ((flags2 & XFS_DIFLAG2_REFLINK) && !S_ISREG(mode)) ||
	    ((flags2 & (XFS_DIFLAG2_REFLINK | XFS_DIFLAG2_COWEXTSIZE)) &&
	    !(sb->sb_features_ro_compat & XFS_SB_FEAT_RO_COMPAT_REFLINK)))
		return xfs_read_fail(why, EINVAL, "invalid inode extent hints or reflink flags");
	if (S_ISLNK(mode) && (size > XFS_SYMLINK_MAXLEN ||
	    (d.di_format == XFS_DINODE_FMT_LOCAL && memchr(raw + core, 0, size) != NULL)))
		return xfs_read_fail(why, EINVAL, "invalid local symlink target");
	if (d.di_format == XFS_DINODE_FMT_EXTENTS) {
		uint64_t i, total = 0;
		error = xfs_extent_lookup(sb, raw + core, nex, 0, false, 0,
		    UINT64_C(1) << 54, &mapped, &end, &unwritten, why);
		if (error != 0)
			return error;
		for (i = 0; i < nex; i++) {
			const uint8_t *rec = raw + core + i * sizeof(struct xfs_bmbt_rec);
			total += xfs_get64(rec + 8) & ((1U << 21) - 1);
			if (!S_ISREG(mode) && (xfs_get64(rec) >> 63))
				return xfs_read_fail(why, EINVAL, "unwritten metadata extent");
		}
		if (total > blocks)
			return xfs_read_fail(why, EINVAL, "extent blocks exceed inode block count");
	} else if (d.di_format == XFS_DINODE_FMT_BTREE) {
		unsigned int level;
		uint64_t low = 0, high = UINT64_C(1) << 54;
		error = xfs_bmap_root(sb, raw + core, dsize, 0,
		    &level, &low, &high, &mapped, why);
		if (error != 0)
			return error;
	}
	/* Publish only after the selected core and inline extent checks. */
	memset(result, 0, sizeof(*result));
	result->number = ino;
	result->size = size;
	result->nextents = nex;
	result->mode = mode;
	result->data_offset = core;
	result->data_size = dsize;
	result->format = d.di_format;
	memcpy(result->disk, raw, sb->sb_inodesize);
	return 0;
}
