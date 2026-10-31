/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_IO_H_
#define _XFS_NETBSD_IO_H_

#include "xfs_probe.h"

/*
 * A read-only data device. The callback returns a positive errno, or zero
 * and the number of bytes read in *done. A short read is an I/O error; the
 * callback must not write past length. On error the buffer is unspecified.
 * The caller owns cookie and must keep the device open and unchanged for
 * the lifetime of the reader. There is deliberately no write callback.
 */
struct xfs_io {
	void *cookie;
	uint64_t size;
	int (*read)(void *, uint64_t, void *, size_t, size_t *);
};

/* Initialize with xfs_reader_open; treat the contents as immutable. */
struct xfs_reader {
	struct xfs_io io;
	struct xfs_sb sb;
};

/* Verified raw on-disk headers; AGFL entries are checked, not retained. */
struct xfs_ag_headers {
	struct xfs_agf agf;
	struct xfs_agi agi;
};

int xfs_io_read(const struct xfs_io *, uint64_t, void *, size_t);

/*
 * scratch must hold at least one filesystem sector, up to
 * XFS_MAX_SECTORSIZE bytes. No allocation or large kernel-stack buffer is
 * needed. Outputs are unchanged on failure; scratch may be overwritten.
 * Diagnostics are static strings, as in xfs_probe_superblock.
 * Header verification does not verify referenced btrees or journal state.
 */
int xfs_reader_open(const struct xfs_io *, void *, size_t,
    struct xfs_reader *, const char **);
int xfs_read_ag(const struct xfs_reader *, uint32_t, void *, size_t,
    struct xfs_ag_headers *, const char **);

/*
 * Read whole filesystem blocks within one AG; fsbno is an encoded AG/block.
 * The destination contents are unspecified on an I/O failure.
 */
int xfs_read_blocks(const struct xfs_reader *, uint64_t, uint32_t,
    void *, size_t, const char **);

/* Shared range calculation, also used before reading AG header sectors. */
int xfs_ag_geometry(const struct xfs_sb *, uint32_t, uint64_t *, uint32_t *);

/* Verify the complete metadata buffer, without modifying or casting it. */
int xfs_verify_metadata_crc(const void *, size_t, size_t);

#endif /* _XFS_NETBSD_IO_H_ */
