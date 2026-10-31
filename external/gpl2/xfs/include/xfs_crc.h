/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_CRC_H_
#define _XFS_NETBSD_CRC_H_
#include "xfs_compat.h"

/* Internal streaming CRC32C; initialize with UINT32_MAX and complement last. */
uint32_t xfs_crc32c(uint32_t, const void *, size_t);
#endif
