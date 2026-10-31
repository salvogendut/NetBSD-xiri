/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Selected constants/layouts from xfs_da_format.h; UUID uses the port type. */
#ifndef _XFS_NETBSD_DIR_LAYOUT_H_
#define _XFS_NETBSD_DIR_LAYOUT_H_
#include "xfs_compat.h"

#define XFS_DIR2_BLOCK_MAGIC 0x58443242
#define XFS_DIR2_DATA_MAGIC 0x58443244
#define XFS_DIR3_BLOCK_MAGIC 0x58444233
#define XFS_DIR3_DATA_MAGIC 0x58444433
#define XFS_DIR3_FT_UNKNOWN 0
#define XFS_DIR3_FT_REG_FILE 1
#define XFS_DIR3_FT_DIR 2
#define XFS_DIR3_FT_SYMLINK 7
#define XFS_DIR3_FT_MAX 9
#define XFS_DIR2_DATA_FREE_TAG 0xffff
#define XFS_DIR2_SPACE_SIZE (1ULL << 35)
#define XFS_DIR2_LEAF1_MAGIC 0xd2f1
#define XFS_DIR2_LEAFN_MAGIC 0xd2ff
#define XFS_DIR3_LEAF1_MAGIC 0x3df1
#define XFS_DIR3_LEAFN_MAGIC 0x3dff
#define XFS_DIR2_FREE_MAGIC 0x58443246
#define XFS_DIR3_FREE_MAGIC 0x58444633
/* The common DA headers are also selected by the attribute reader. */
#include "xfs_attr_layout.h"

struct xfs_dir2_leaf_hdr {
	struct xfs_attr_blkinfo info;
	__be16 count, stale;
};
struct xfs_dir3_leaf_hdr {
	struct xfs_attr3_blkinfo info;
	__be16 count, stale;
	__be32 pad;
};

struct xfs_dir2_data_free {
	__be16 offset;
	__be16 length;
};
struct xfs_dir2_data_hdr {
	__be32 magic;
	struct xfs_dir2_data_free bestfree[3];
};
struct xfs_dir3_blk_hdr {
	__be32 magic;
	__be32 crc;
	__be64 blkno;
	__be64 lsn;
	xfs_uuid_t uuid;
	__be64 owner;
};
struct xfs_dir3_data_hdr {
	struct xfs_dir3_blk_hdr hdr;
	struct xfs_dir2_data_free best_free[3];
	__be32 pad;
};
struct xfs_dir2_free_hdr {
	__be32 magic, firstdb, nvalid, nused;
};
struct xfs_dir3_free_hdr {
	struct xfs_dir3_blk_hdr hdr;
	__be32 firstdb, nvalid, nused, pad;
};
struct xfs_dir2_leaf_entry {
	__be32 hashval;
	__be32 address;
};
struct xfs_dir2_block_tail {
	__be32 count;
	__be32 stale;
};
#define XFS_DIR3_DATA_CRC_OFF offsetof(struct xfs_dir3_data_hdr, hdr.crc)
#endif
