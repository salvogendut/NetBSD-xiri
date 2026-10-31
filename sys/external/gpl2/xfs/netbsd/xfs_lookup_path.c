/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "xfs_read_internal.h"
#include "xfs_directory.h"

int
xfs_lookup_path(const struct xfs_reader *r, const void *path, size_t length,
    bool follow_last, struct xfs_path_workspace *work, void *scratch,
    size_t capacity, uint64_t *result, const char **why)
{
	uint64_t child, parent;
	size_t cursor = 0, start, end, target_length, remaining;
	unsigned int links = 0;
	int error;

	if (why != NULL)
		*why = NULL;
	if (r == NULL || path == NULL || work == NULL || scratch == NULL || result == NULL)
		return xfs_read_fail(why, EINVAL, "null pathname argument");
	if (length >= sizeof(work->pending))
		return xfs_read_fail(why, ENAMETOOLONG, "image path too long");
	if (!length || *(const char *)path != '/' || memchr(path, 0, length) != NULL)
		return xfs_read_fail(why, EINVAL, "image path must be absolute without embedded NULs");
	if (capacity < xfs_directory_scratch_size(r))
		return xfs_read_fail(why, EMSGSIZE, "pathname scratch buffer too small");
	memcpy(work->pending, path, length);
	work->pending[length] = 0;
	error = xfs_read_inode(r, r->sb.sb_rootino, scratch, capacity, &work->inode, why);
	if (error != 0)
		return error;
	for (;;) {
		start = cursor;
		while (cursor < length && work->pending[cursor] == '/')
			cursor++;
		if (cursor == length) {
			if (cursor > start && !S_ISDIR(work->inode.mode))
				return xfs_read_fail(why, ENOTDIR, "trailing slash requires a directory");
			*result = work->inode.number;
			return 0;
		}
		end = cursor;
		while (end < length && work->pending[end] != '/')
			end++;
		if (end - cursor > 255)
			return xfs_read_fail(why, ENAMETOOLONG, "image path component too long");
		parent = work->inode.number;
		error = xfs_directory_lookup(r, &work->inode, work->pending + cursor,
		    end - cursor, scratch, capacity, &child, why);
		if (error != 0)
			return error;
		error = xfs_read_inode(r, child, scratch, capacity, &work->inode, why);
		if (error != 0)
			return error;
		if (S_ISLNK(work->inode.mode) && (follow_last || end < length)) {
			if (links++ >= XFS_IMAGE_SYMLINK_LIMIT)
				return xfs_read_fail(why, ELOOP, "too many image symlinks");
			error = xfs_read_file(r, &work->inode, 0, work->target,
			    sizeof(work->target), &target_length, scratch, capacity, why);
			if (error != 0)
				return error;
			if (!target_length)
				return xfs_read_fail(why, EINVAL, "empty symlink target");
			remaining = length - end;
			if (target_length + remaining >= sizeof(work->pending))
				return xfs_read_fail(why, ENAMETOOLONG, "expanded image path too long");
			memmove(work->pending + target_length, work->pending + end, remaining);
			memcpy(work->pending, work->target, target_length);
			length = target_length + remaining;
			work->pending[length] = 0;
			cursor = 0;
			error = xfs_read_inode(r, work->target[0] == '/' ? r->sb.sb_rootino : parent,
			    scratch, capacity, &work->inode, why);
			if (error != 0)
				return error;
		} else {
			cursor = end;
		}
	}
}
