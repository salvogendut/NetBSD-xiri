/* SPDX-License-Identifier: GPL-2.0 */
/* Copyright (c) 2000-2001,2005 Silicon Graphics, Inc.
 * Copyright (c) 2013 Red Hat, Inc. All Rights Reserved. */
/* $NetBSD$ */
/* Selected disk layouts from xfs_da_format.h; use the port UUID type. */
#ifndef _XFS_NETBSD_ATTR_LAYOUT_H_
#define _XFS_NETBSD_ATTR_LAYOUT_H_
#include "xfs_compat.h"
#define XFS_ATTR_LEAF_MAGIC 0xfbee
#define XFS_ATTR3_LEAF_MAGIC 0x3bee
#define XFS_DA_NODE_MAGIC 0xfebe
#define XFS_DA3_NODE_MAGIC 0x3ebe
#define XFS_DA_NODE_MAXDEPTH 5
#define XFS_ATTR3_RMT_MAGIC 0x5841524d
#define XFS_ATTR_LOCAL 1U
#define XFS_ATTR_INCOMPLETE 128U
struct xfs_attr_blkinfo {
	__be32 forw, back;
	__be16 magic, pad;
};
struct xfs_attr3_blkinfo {
	struct xfs_attr_blkinfo hdr;
	__be32 crc;
	__be64 blkno, lsn;
	xfs_uuid_t uuid;
	__be64 owner;
};
struct xfs_attr_leaf_map { __be16 base, size; };
struct xfs_attr_leaf_hdr {
	struct xfs_attr_blkinfo info;
	__be16 count, usedbytes, firstused;
	uint8_t holes, pad;
	struct xfs_attr_leaf_map freemap[3];
};
struct xfs_attr3_leaf_hdr {
	struct xfs_attr3_blkinfo info;
	__be16 count, usedbytes, firstused;
	uint8_t holes, pad;
	struct xfs_attr_leaf_map freemap[3];
	__be32 pad2;
};
struct xfs_attr_node_hdr {
	struct xfs_attr_blkinfo info;
	__be16 count, level;
};
struct xfs_attr3_node_hdr {
	struct xfs_attr3_blkinfo info;
	__be16 count, level;
	__be32 pad;
};
struct xfs_attr3_rmt_hdr {
	__be32 magic, offset, bytes, crc;
	xfs_uuid_t uuid;
	__be64 owner, blkno, lsn;
};
#endif
