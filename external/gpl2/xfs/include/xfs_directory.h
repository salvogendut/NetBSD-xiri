/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_NETBSD_DIRECTORY_H_
#define _XFS_NETBSD_DIRECTORY_H_
#include "xfs_inode.h"

#define XFS_IMAGE_PATH_MAX 4096
#define XFS_IMAGE_SYMLINK_LIMIT 40
#define XFS_DIR_WORKSPACE_SIZE (8U * 1024U * 1024U)

/* Names are byte strings, not NUL terminated. filetype uses XFS disk values
 * (0 unknown, 1 file, 2 directory, ... 7 symlink, 8 whiteout).
 * The callback must not retain name, alter the reader/inode/scratch, or
 * reenter using the same workspace. Return zero or a positive errno.
 * Metadata verification completes before the first callback. A callback
 * failure can occur after earlier entries have been emitted.
 */
typedef int (*xfs_directory_emit)(void *, uint64_t, uint8_t,
    const uint8_t *, size_t);

/* The server supplies an 8 MiB workspace, including external directory data
 * and indexes. Oversized directories fail with EMSGSIZE; no hidden allocation.
 * Buffers may be unaligned, but must not overlap the reader, inode, name or results.
 * External directories are fully verified before emitting entries or looking
 * up a hash. The caller must keep the image immutable throughout the operation.
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
