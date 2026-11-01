/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2006 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Attribute decoding follows xfs_inode_from_disk{,_ts} in xfs_inode_buf.c. */
#include "xfs_read_internal.h"

static int
decode_time(xfs_timestamp_t disk, bool bigtime, struct xfs_inode_time *time)
{
	uint64_t value = be64_to_cpu(disk), seconds;
	if (bigtime) {
		seconds = value / UINT64_C(1000000000);
		if (seconds > (UINT64_MAX / UINT64_C(1000000000) & ~UINT64_C(3)))
			return EINVAL;
		time->seconds = (int64_t)seconds - INT64_C(2147483648);
		time->nanoseconds = value % UINT64_C(1000000000);
	} else {
		seconds = value >> 32;
		time->seconds = seconds >= UINT64_C(2147483648) ?
		    (int64_t)seconds - INT64_C(4294967296) : (int64_t)seconds;
		time->nanoseconds = (uint32_t)value;
		if (time->nanoseconds >= 1000000000)
			return EINVAL;
	}
	return 0;
}

int
xfs_inode_getattr(const struct xfs_reader *reader, const struct xfs_inode_image *inode,
    struct xfs_inode_stat *result, const char **why)
{
	struct xfs_dinode d;
	struct xfs_inode_stat st = { 0 };
	uint64_t blocks;
	bool bigtime;

	if (why != NULL)
		*why = NULL;
	if (reader == NULL || inode == NULL || result == NULL)
		return xfs_read_fail(why, EINVAL, "null inode attribute argument");
	memcpy(&d, inode->disk, sizeof(d));
	bigtime = d.di_version == 3 && (be64_to_cpu(d.di_flags2) & XFS_DIFLAG2_BIGTIME);
	st.uid = be32_to_cpu(d.di_uid);
	st.gid = be32_to_cpu(d.di_gid);
	st.nlink = d.di_version == 1 ? be16_to_cpu(d.di_onlink) : be32_to_cpu(d.di_nlink);
	st.generation = be32_to_cpu(d.di_gen);
	st.flags = be16_to_cpu(d.di_flags);
	st.has_attributes = d.di_forkoff != 0;
	st.has_birthtime = d.di_version == 3;
	st.change_count = st.has_birthtime ? be64_to_cpu(d.di_changecount) : 0;
	blocks = be64_to_cpu(d.di_nblocks);
	if (reader->sb.sb_blocksize == 0 || blocks > INT64_MAX / reader->sb.sb_blocksize)
		return xfs_read_fail(why, EOVERFLOW, "inode allocated byte count overflow");
	st.allocated_bytes = blocks * reader->sb.sb_blocksize;
	if (decode_time(d.di_atime, bigtime, &st.atime) != 0 ||
	    decode_time(d.di_mtime, bigtime, &st.mtime) != 0 ||
	    decode_time(d.di_ctime, bigtime, &st.ctime) != 0 ||
	    (st.has_birthtime && decode_time(d.di_crtime, bigtime, &st.birthtime) != 0))
		return xfs_read_fail(why, EINVAL, "invalid inode timestamp");
	*result = st;
	return 0;
}
