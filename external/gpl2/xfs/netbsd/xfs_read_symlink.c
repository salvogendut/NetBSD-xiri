/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * Copyright (c) 2012-2013 Red Hat, Inc.
 * All rights reserved.
 */
/* $NetBSD$ */
/* Adapted xfs_symlink_remote_read, xfs_symlink_verify and hdr_ok. */
#include "xfs_read_internal.h"

_Static_assert(sizeof(struct xfs_dsymlink_hdr) == 56, "symlink header layout");

/* Read every target chunk even for a partial request; never expose headers. */
int
xfs_read_symlink_range(const struct xfs_reader *r,
    const struct xfs_inode_image *inode, uint64_t offset, void *data,
    size_t length, void *scratch, size_t capacity, const char **why)
{
	const struct xfs_sb *sb = &r->sb;
	const uint8_t *p;
	struct xfs_dsymlink_hdr h;
	uint64_t fsb, end, disk_offset, logical = 0;
	size_t consumed = 0, chunk, begin, stop, header;
	bool zero;
	int error;

	if (inode->size == 0 || inode->size >= XFS_SYMLINK_MAXLEN)
		return xfs_read_fail(why, EINVAL, "invalid symlink target length");
	if (inode->format == XFS_DINODE_FMT_LOCAL) {
		memcpy(data, inode->disk + inode->data_offset + offset, length);
		return 0;
	}
	header = xfs_crc_enabled(sb) ? sizeof(h) : 0;
	while (consumed < inode->size) {
		error = xfs_file_mapping(r, inode, logical, scratch, capacity,
		    &fsb, &end, &zero, why);
		if (error != 0)
			return error;
		if (zero)
			return xfs_read_fail(why, EINVAL, "hole or unwritten symlink extent");
		/* V5 blocks are >= 1 KiB, so a <1 KiB target fits one block
		 * unless a 1 KiB block has a header and a second fragment. */
		if (end <= logical)
			return xfs_read_fail(why, EINVAL, "invalid symlink mapping");
		chunk = sb->sb_blocksize - header;
		if (chunk > inode->size - consumed)
			chunk = inode->size - consumed;
		/* Upstream places one header per mapped extent buffer, not per
		 * filesystem block. A contiguous two-block v5 target therefore
		 * needs two blocks of caller scratch for one CRC verification. */
		unsigned int blocks = 1;
		if (header && inode->size - consumed > chunk && end - logical > 1)
			blocks = 2;
		error = xfs_read_blocks(r, fsb, blocks, scratch, capacity, why);
		if (error != 0)
			return error;
		chunk = (size_t)blocks * sb->sb_blocksize - header;
		if (chunk > inode->size - consumed)
			chunk = inode->size - consumed;
		if (header) {
			if (xfs_verify_metadata_crc(scratch, blocks * sb->sb_blocksize,
			    XFS_SYMLINK_CRC_OFF) != 0)
				return xfs_read_fail(why, EINVAL, "symlink CRC32C mismatch");
			memcpy(&h, scratch, sizeof(h));
			if (xfs_data_address(sb, fsb, blocks, &disk_offset) != 0 ||
			    be32_to_cpu(h.sl_magic) != XFS_SYMLINK_MAGIC ||
			    be64_to_cpu(h.sl_blkno) != disk_offset / 512 ||
			    be64_to_cpu(h.sl_owner) != inode->number ||
			    memcmp(&h.sl_uuid, &sb->sb_meta_uuid, 16) != 0 ||
			    be32_to_cpu(h.sl_offset) != consumed ||
			    be32_to_cpu(h.sl_bytes) != chunk)
				return xfs_read_fail(why, EINVAL, "symlink identity or chunk mismatch");
		}
		p = (const uint8_t *)scratch + header;
		if (memchr(p, 0, chunk) != NULL)
			return xfs_read_fail(why, EINVAL, "NUL in symlink target");
		begin = offset > consumed ? offset : consumed;
		stop = offset + length < consumed + chunk ? offset + length : consumed + chunk;
		if (stop > begin)
			memcpy((uint8_t *)data + begin - offset, p + begin - consumed, stop - begin);
		consumed += chunk;
		logical += blocks;
	}
	return 0;
}
