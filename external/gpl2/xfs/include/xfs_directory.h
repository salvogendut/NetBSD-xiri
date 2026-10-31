/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_DIRECTORY_H_
#define _XFS_NETBSD_DIRECTORY_H_
#include "xfs_inode.h"

#define XFS_IMAGE_PATH_MAX 4096
#define XFS_IMAGE_SYMLINK_LIMIT 40

/* Names are byte strings, not NUL terminated. filetype uses XFS disk values
 * (0 unknown, 1 file, 2 directory, ... 7 symlink, 8 whiteout).
 * The callback must not retain name, alter the reader/inode/scratch, or
 * reenter using the same workspace. Return zero or a positive errno.
 * Entries from earlier verified blocks can be emitted before a later error.
 */
typedef int (*xfs_directory_emit)(void *, uint64_t, uint8_t,
    const uint8_t *, size_t);

/* Workspace size = directory block size + filesystem block size. Buffers
 * may be unaligned, but must not overlap the reader, inode, name or results.
 * External leaf/node hash and free-space indexes are not consulted: these
 * are linear inspection scans, not full directory consistency checks.
 */
size_t xfs_directory_scratch_size(const struct xfs_reader *);
int xfs_read_directory(const struct xfs_reader *, const struct xfs_inode_image *,
    void *, size_t, xfs_directory_emit, void *, const char **);
int xfs_directory_lookup(const struct xfs_reader *, const struct xfs_inode_image *,
    const void *, size_t, void *, size_t, uint64_t *, const char **);

/* Caller-owned workspace, never allocate on a kernel stack. */
struct xfs_path_workspace {
	struct xfs_inode_image inode;
	char pending[XFS_IMAGE_PATH_MAX];
	char target[XFS_SYMLINK_MAXLEN];
};

/* Resolve an absolute byte-string path within the image. Follow intermediate
 * symlinks and optionally the final symlink; a trailing slash requires a
 * directory. Result is unchanged on error; work/scratch are unspecified.
 * work, scratch, path and result must be disjoint. The reader must stay
 * quiescent. No host filesystem pathname is opened during resolution.
 */
int xfs_lookup_path(const struct xfs_reader *, const void *, size_t, bool,
    struct xfs_path_workspace *, void *, size_t, uint64_t *, const char **);
#endif
