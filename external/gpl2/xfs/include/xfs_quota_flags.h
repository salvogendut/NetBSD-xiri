// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2000-2003,2005 Silicon Graphics, Inc.
 * All Rights Reserved.
 */
/* $NetBSD$ */
/* Extracted unchanged from Linux v6.12 libxfs/xfs_log_format.h. */
#ifndef _XFS_NETBSD_QUOTA_FLAGS_H_
#define _XFS_NETBSD_QUOTA_FLAGS_H_

#define XFS_UQUOTA_ACCT	0x0001  /* user quota accounting ON */
#define XFS_UQUOTA_ENFD	0x0002  /* user quota limits enforced */
#define XFS_UQUOTA_CHKD	0x0004  /* quotacheck run on usr quotas */
#define XFS_PQUOTA_ACCT	0x0008  /* project quota accounting ON */
#define XFS_OQUOTA_ENFD	0x0010  /* other (grp/prj) quota limits enforced */
#define XFS_OQUOTA_CHKD	0x0020  /* quotacheck run on other (grp/prj) quotas */
#define XFS_GQUOTA_ACCT	0x0040  /* group quota accounting ON */

/*
 * Conversion to and from the combined OQUOTA flag (if necessary)
 * is done only in xfs_sb_qflags_to_disk() and xfs_sb_qflags_from_disk()
 */
#define XFS_GQUOTA_ENFD	0x0080  /* group quota limits enforced */
#define XFS_GQUOTA_CHKD	0x0100  /* quotacheck run on group quotas */
#define XFS_PQUOTA_ENFD	0x0200  /* project quota limits enforced */
#define XFS_PQUOTA_CHKD	0x0400  /* quotacheck run on project quotas */

#endif
