/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_INODE_H_
#define _XFS_NETBSD_INODE_H_

#include "xfs_io.h"

/* Initialize with xfs_read_inode and keep immutable, with its original reader. */
struct xfs_inode_image {
	uint64_t number, size, nextents;
	uint16_t mode, data_offset, data_size;
	uint8_t format;
	uint8_t disk[1U << XFS_DINODE_MAX_LOG];
};

struct xfs_inode_time {
	int64_t seconds;
	uint32_t nanoseconds;
};
struct xfs_inode_stat {
	uint32_t uid, gid, nlink, generation;
	uint64_t allocated_bytes, change_count;
	struct xfs_inode_time atime, mtime, ctime, birthtime;
	uint16_t flags; /* XFS_DIFLAG_*; translate explicitly for the host. */
	bool has_birthtime, has_attributes;
};

/* Decode attributes of a verified inode. Reject invalid timestamps and
 * allocation-byte overflow. Output is unchanged on failure. Attribute forks
 * are reported, not interpreted; callers must not ignore ACL semantics.
 */
int xfs_inode_getattr(const struct xfs_reader *, const struct xfs_inode_image *,
    struct xfs_inode_stat *, const char **);

/*
 * Verify allocation in the inode btree, then read and verify the inode.
 * scratch must hold sb_blocksize bytes and must not overlap result.
 * result is unchanged on failure. Attribute contents are not inspected.
 */
int xfs_read_inode(const struct xfs_reader *, uint64_t, void *, size_t,
    struct xfs_inode_image *, const char **);

/*
 * Read regular-file bytes or a symlink target, clamped at EOF.
 * scratch (at least sb_blocksize bytes, or 2048 bytes for a remote symlink
 * on a filesystem with 1024-byte blocks), data, inode and done must not overlap.
 * *done is zero on failure; data may have been partly overwritten.
 * No allocation, writes, journal replay, or assertion of journal cleanliness.
 */
int xfs_read_file(const struct xfs_reader *, const struct xfs_inode_image *,
    uint64_t, void *, size_t, size_t *, void *, size_t, const char **);

#endif
