/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_LOG_H_
#define _XFS_NETBSD_LOG_H_
#include "xfs_io.h"

/* Caller-owned workspace; allocate on the heap, not the callback stack. */
#define XFS_LOG_SCRATCH_SIZE (256U * 1024 + 8 * 512)

enum xfs_log_state {
	XFS_LOG_CLEAN,
	XFS_LOG_NEEDS_RECOVERY
};
struct xfs_log_info {
	enum xfs_log_state state;
	uint32_t head, tail, record; /* 512-byte log-relative blocks */
	uint32_t cycle; /* write cycle at head */
	uint32_t version;
	bool initialized; /* canonical, zero-CRC mkfs unmount record */
	bool legacy; /* IRIX-style v1 header without format, UUID or checksum */
	bool checksummed; /* latest record has a verified CRC32C */
};

/*
 * Inspect the internal v1/v2 log on a quiescent device. Scan every cycle stamp,
 * then verify the latest complete record and its unmount operation. No replay,
 * trimming or stale-block clearing. Ambiguous/unsupported logs return errors;
 * they never count as clean. Output is unchanged on failure. Scratch may be
 * unaligned, must be disjoint from reader/output, and is overwritten.
 * CLEAN describes the journal only, not global filesystem consistency.
 * Legacy v1 records lack UUID and checksum protection;
 * their geometry, cycle stamps, LSNs and operation framing are checked.
 */
int xfs_log_inspect(const struct xfs_reader *, void *, size_t,
    struct xfs_log_info *, const char **);

/* Return zero only for CLEAN; dirty logs return EBUSY. Does not alter reader. */
int xfs_require_clean_log(const struct xfs_reader *, void *, size_t, const char **);

#endif
