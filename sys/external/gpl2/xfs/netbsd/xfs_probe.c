/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#include "xfs_probe.h"

#ifdef _KERNEL
#include <sys/errno.h>
#else
#include <errno.h>
#endif

static int
reject(const char **why, int error, const char *message)
{

	if (why != NULL)
		*why = message;
	return error;
}

/* Castagnoli CRC, with the on-disk checksum field treated as zero. */
static uint32_t
superblock_crc(const uint8_t *bytes, size_t length)
{
	uint32_t crc = UINT32_MAX;
	size_t i;
	unsigned int bit;

	for (i = 0; i < length; i++) {
		if (i < XFS_SB_CRC_OFF || i >= XFS_SB_CRC_OFF + 4)
			crc ^= bytes[i];
		for (bit = 0; bit < 8; bit++)
			crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
	}
	return ~crc;
}

static unsigned int
ceil_log2(uint32_t value)
{
	unsigned int bits = 0;

	for (value--; value != 0; value >>= 1)
		bits++;
	return bits;
}

int
xfs_probe_superblock(const void *data, size_t length, uint64_t device_bytes,
    struct xfs_sb *result, const char **why)
{
	struct xfs_dsb disk;
	struct xfs_sb sb = { 0 };
	uint64_t agno, agbno, rawblock, mask, count;
	unsigned int inobits;
	bool v5;

	if (why != NULL)
		*why = NULL;
	if (data == NULL || result == NULL)
		return reject(why, EINVAL, "null superblock argument");
	if (length < XFS_MIN_SECTORSIZE)
		return reject(why, EMSGSIZE, "truncated superblock sector");

	/* Never cast an unaligned or short input buffer to a disk structure. */
	memcpy(&disk, data, sizeof(disk));
	if (be32_to_cpu(disk.sb_magicnum) != XFS_SB_MAGIC)
		return reject(why, EINVAL, "not an XFS superblock");
	xfs_sb_from_disk(&sb, &disk);
	if (!xfs_sb_good_version(&sb))
		return reject(why, EOPNOTSUPP, "unsupported version or feature mask");
	v5 = xfs_sb_is_v5(&sb);

	/* Bound every disk-controlled shift before evaluating it. */
	if (sb.sb_sectlog < XFS_MIN_SECTORSIZE_LOG ||
	    sb.sb_sectlog > XFS_MAX_SECTORSIZE_LOG ||
	    sb.sb_sectsize != (1U << sb.sb_sectlog))
		return reject(why, EINVAL, "invalid sector geometry");
	if (length < sb.sb_sectsize)
		return reject(why, EMSGSIZE, "truncated superblock sector");
	if (v5 && superblock_crc(data, sb.sb_sectsize) != le32toh(disk.sb_crc))
		return reject(why, EINVAL, "superblock CRC32C mismatch");
	if (sb.sb_blocklog < XFS_MIN_BLOCKSIZE_LOG ||
	    sb.sb_blocklog > XFS_MAX_BLOCKSIZE_LOG ||
	    sb.sb_blocksize != (1U << sb.sb_blocklog) ||
	    sb.sb_blocksize < sb.sb_sectsize ||
	    (v5 && sb.sb_blocksize < XFS_MIN_CRC_BLOCKSIZE) ||
	    sb.sb_dirblklog + sb.sb_blocklog > XFS_MAX_BLOCKSIZE_LOG)
		return reject(why, EINVAL, "invalid block geometry");
	if (sb.sb_inodelog < XFS_DINODE_MIN_LOG ||
	    sb.sb_inodelog > XFS_DINODE_MAX_LOG ||
	    sb.sb_inodesize != (1U << sb.sb_inodelog) ||
	    sb.sb_inodelog > sb.sb_blocklog ||
	    (v5 && sb.sb_inodesize < 512) ||
	    sb.sb_inopblog != sb.sb_blocklog - sb.sb_inodelog ||
	    sb.sb_inopblock != sb.sb_blocksize / sb.sb_inodesize)
		return reject(why, EINVAL, "invalid inode geometry");
	if (sb.sb_agblocks == 0 || sb.sb_agcount == 0 || sb.sb_dblocks == 0 ||
	    sb.sb_agblklog != ceil_log2(sb.sb_agblocks) ||
	    (uint64_t)sb.sb_agblocks * sb.sb_blocksize < (1U << 24) ||
	    (uint64_t)sb.sb_agblocks * sb.sb_blocksize > (1ULL << 40))
		return reject(why, EINVAL, "invalid allocation group geometry");
	count = sb.sb_dblocks / sb.sb_agblocks;
	if (sb.sb_dblocks % sb.sb_agblocks != 0)
		count++;
	if (count != sb.sb_agcount ||
	    sb.sb_dblocks > (uint64_t)INT64_MAX / sb.sb_blocksize)
		return reject(why, EINVAL, "invalid data block count");
	if (sb.sb_dblocks > device_bytes / sb.sb_blocksize)
		return reject(why, EMSGSIZE, "filesystem exceeds data device");
	if (sb.sb_inprogress != 0 || sb.sb_shared_vn != 0 || sb.sb_imax_pct > 100)
		return reject(why, EINVAL, "invalid or incomplete filesystem state");

	if (v5) {
		if (sb.sb_features_incompat & XFS_SB_FEAT_INCOMPAT_UNKNOWN)
			return reject(why, EOPNOTSUPP, "unknown incompatible features");
		if (sb.sb_features_incompat & XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR)
			return reject(why, EINVAL, "filesystem needs repair");
		if (sb.sb_features_log_incompat != 0)
			return reject(why, EOPNOTSUPP, "log features require recovery support");
	}
	if (sb.sb_rblocks != 0 || sb.sb_rextents != 0 || sb.sb_rbmblocks != 0 ||
	    sb.sb_rextslog != 0 || sb.sb_frextents != 0)
		return reject(why, EOPNOTSUPP, "realtime devices are not supported");
	if (sb.sb_logstart == 0)
		return reject(why, EOPNOTSUPP, "external logs are not supported");

	/* fsblock and inode numbers encode an AG number, not a linear offset. */
	mask = (1ULL << sb.sb_agblklog) - 1;
	agno = sb.sb_logstart >> sb.sb_agblklog;
	agbno = sb.sb_logstart & mask;
	if (agno >= sb.sb_agcount || agbno >= sb.sb_agblocks ||
	    sb.sb_logblocks == 0 || sb.sb_logblocks > sb.sb_agblocks - agbno)
		return reject(why, EINVAL, "invalid internal log extent");
	rawblock = agno * sb.sb_agblocks + agbno;
	if (rawblock >= sb.sb_dblocks || sb.sb_logblocks > sb.sb_dblocks - rawblock)
		return reject(why, EINVAL, "internal log exceeds data device");

	inobits = sb.sb_agblklog + sb.sb_inopblog;
	if (inobits > 32 || sb.sb_rootino == 0 || sb.sb_rootino == NULLFSINO)
		return reject(why, EINVAL, "invalid root inode number");
	agno = sb.sb_rootino >> inobits;
	agbno = (sb.sb_rootino >> sb.sb_inopblog) & mask;
	if (agno >= sb.sb_agcount || agbno >= sb.sb_agblocks ||
	    agno * sb.sb_agblocks + agbno >= sb.sb_dblocks)
		return reject(why, EINVAL, "root inode exceeds data device");

	*result = sb;
	return 0;
}
