/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Selected physical log definitions from Linux v6.12 xfs_log_format.h. */
#ifndef _XFS_NETBSD_LOG_LAYOUT_H_
#define _XFS_NETBSD_LOG_LAYOUT_H_
#include "xfs_compat.h"

#define XLOG_HEADER_MAGIC_NUM 0xfeedbabeU
#define XLOG_VERSION_1 1
#define XLOG_VERSION_2 2
#define XLOG_MIN_RECORD_BSIZE (16 * 1024)
#define XLOG_BIG_RECORD_BSIZE (32 * 1024)
#define XLOG_MAX_RECORD_BSIZE (256 * 1024)
#define XLOG_HEADER_CYCLE_SIZE (32 * 1024)
#define XLOG_HEADER_SIZE 512
#define XFS_TRANSACTION 0x69
#define XFS_LOG 0xaa
#define XLOG_UNMOUNT_TYPE 0x556e
#define XLOG_UNMOUNT_TRANS 0x20
#define XLOG_FMT_UNKNOWN 0
#define XLOG_FMT_LINUX_LE 1
#define XLOG_FMT_LINUX_BE 2
#define XLOG_FMT_IRIX_BE 3

struct xlog_op_header {
	__be32 oh_tid;
	__be32 oh_len;
	__u8 oh_clientid;
	__u8 oh_flags;
	__u16 oh_res2;
};
struct xlog_rec_header {
	__be32 h_magicno;
	__be32 h_cycle;
	__be32 h_version;
	__be32 h_len;
	__be64 h_lsn;
	__be64 h_tail_lsn;
	__le32 h_crc;
	__be32 h_prev_block;
	__be32 h_num_logops;
	__be32 h_cycle_data[XLOG_HEADER_CYCLE_SIZE / XLOG_HEADER_SIZE];
	__be32 h_fmt;
	xfs_uuid_t h_fs_uuid;
	__be32 h_size;
};
struct xlog_rec_ext_header {
	__be32 xh_cycle;
	__be32 xh_cycle_data[XLOG_HEADER_CYCLE_SIZE / XLOG_HEADER_SIZE];
};

#endif
