/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "xfs_io.h"
#include <sys/errno.h>

static int
fail(const char **why, int error, const char *message)
{
	if (why != NULL)
		*why = message;
	return error;
}

int
xfs_io_read(const struct xfs_io *io, uint64_t offset, void *data, size_t length)
{
	size_t done = 0;
	int error;

	if (io == NULL || io->read == NULL || (data == NULL && length != 0))
		return EINVAL;
	if (offset > INT64_MAX || length > (uint64_t)INT64_MAX - offset)
		return EOVERFLOW;
	if (offset > io->size || length > io->size - offset)
		return EMSGSIZE;
	if (length == 0)
		return 0;
	error = io->read(io->cookie, offset, data, length, &done);
	if (error != 0)
		return error > 0 ? error : EIO;
	return done == length ? 0 : EIO;
}

int
xfs_reader_open(const struct xfs_io *io, void *scratch, size_t capacity,
    struct xfs_reader *result, const char **why)
{
	struct xfs_reader reader = { 0 };
	struct xfs_dsb disk;
	uint16_t sector;
	int error;

	if (why != NULL)
		*why = NULL;
	if (io == NULL || scratch == NULL || result == NULL)
		return fail(why, EINVAL, "null reader argument");
	if (capacity < XFS_MIN_SECTORSIZE || io->size < XFS_MIN_SECTORSIZE)
		return fail(why, EMSGSIZE, "truncated superblock sector or scratch buffer");
	error = xfs_io_read(io, 0, scratch, XFS_MIN_SECTORSIZE);
	if (error != 0)
		return fail(why, error, "superblock read failed");
	memcpy(&disk, scratch, sizeof(disk));
	if (be32_to_cpu(disk.sb_magicnum) != XFS_SB_MAGIC)
		return fail(why, EINVAL, "not an XFS superblock");
	sector = be16_to_cpu(disk.sb_sectsize);
	if (sector < XFS_MIN_SECTORSIZE || sector > XFS_MAX_SECTORSIZE ||
	    (sector & (sector - 1)) != 0)
		return fail(why, EINVAL, "invalid sector geometry");
	if (capacity < sector)
		return fail(why, EMSGSIZE, "scratch buffer smaller than sector");
	if (sector > XFS_MIN_SECTORSIZE) {
		error = xfs_io_read(io, 0, scratch, sector);
		if (error != 0)
			return fail(why, error, "superblock sector read failed");
	}
	error = xfs_probe_superblock(scratch, sector, io->size, &reader.sb, why);
	if (error != 0)
		return error;
	reader.io = *io;
	*result = reader;
	return 0;
}

int
xfs_ag_geometry(const struct xfs_sb *sb, uint32_t agno, uint64_t *offset,
    uint32_t *blocks)
{
	uint64_t base, remaining;
	uint32_t length;

	if (sb == NULL || offset == NULL || blocks == NULL ||
	    sb->sb_blocklog < XFS_MIN_BLOCKSIZE_LOG ||
	    sb->sb_blocklog > XFS_MAX_BLOCKSIZE_LOG ||
	    sb->sb_blocksize != (1U << sb->sb_blocklog) ||
	    sb->sb_agblocks == 0 || agno >= sb->sb_agcount ||
	    sb->sb_dblocks > (uint64_t)INT64_MAX / sb->sb_blocksize)
		return EINVAL;
	base = (uint64_t)agno * sb->sb_agblocks;
	if (base >= sb->sb_dblocks)
		return EINVAL;
	remaining = sb->sb_dblocks - base;
	length = remaining < sb->sb_agblocks ? (uint32_t)remaining : sb->sb_agblocks;
	/* XFS_MIN_AG_BLOCKS, from the pinned upstream libxfs/xfs_fs.h. */
	if (length < 64 ||
	    (uint64_t)length * sb->sb_blocksize < (uint64_t)4 * sb->sb_sectsize)
		return EINVAL;
	*offset = base * sb->sb_blocksize;
	*blocks = length;
	return 0;
}

int
xfs_read_blocks(const struct xfs_reader *reader, uint64_t fsbno,
    uint32_t count, void *data, size_t capacity, const char **why)
{
	uint64_t agno, agbno, offset, bytes;
	uint32_t blocks;
	const struct xfs_sb *sb;
	int error;

	if (why != NULL)
		*why = NULL;
	if (reader == NULL || data == NULL || count == 0)
		return fail(why, EINVAL, "invalid block read argument");
	sb = &reader->sb;
	if (sb->sb_agblklog > 32)
		return fail(why, EINVAL, "invalid allocation group shift");
	agno = fsbno >> sb->sb_agblklog;
	agbno = fsbno & ((UINT64_C(1) << sb->sb_agblklog) - 1);
	if (agno >= sb->sb_agcount)
		return fail(why, EINVAL, "block outside filesystem");
	error = xfs_ag_geometry(sb, (uint32_t)agno, &offset, &blocks);
	if (error != 0 || agbno >= blocks || count > blocks - agbno)
		return fail(why, EINVAL, "block range outside allocation group");
	bytes = (uint64_t)count * sb->sb_blocksize;
	if (bytes > capacity)
		return fail(why, EMSGSIZE, "block read buffer too small");
	error = xfs_io_read(&reader->io, offset + agbno * sb->sb_blocksize,
	    data, (size_t)bytes);
	if (error != 0)
		return fail(why, error, "block read failed");
	return 0;
}
