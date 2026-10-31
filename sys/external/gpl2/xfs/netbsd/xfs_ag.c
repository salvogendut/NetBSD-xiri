/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2000-2002,2005 Silicon Graphics, Inc.
 * Copyright (c) 2014 Red Hat, Inc.
 * Copyright (C) 2016 Oracle. All Rights Reserved.
 */
/* $NetBSD$ */
/*
 * Read-only adaptations of Linux v6.12's xfs_agf_verify, xfs_agi_verify,
 * xfs_agfl_verify and xfs_agfl_needs_reset. See README.import for the
 * deliberate differences from the live-filesystem verifiers.
 */
#include "xfs_io.h"
#include <sys/errno.h>

_Static_assert(sizeof(struct xfs_agf) == 224, "AGF layout");
_Static_assert(sizeof(struct xfs_agi) == 344, "AGI layout");
_Static_assert(sizeof(struct xfs_agfl) == 36, "AGFL layout");
_Static_assert(XFS_AGF_CRC_OFF == 216, "AGF checksum offset");
_Static_assert(XFS_AGI_CRC_OFF == 312, "AGI checksum offset");
_Static_assert(XFS_AGFL_CRC_OFF == 32, "AGFL checksum offset");

static bool
has_crc(const struct xfs_sb *sb)
{
	return XFS_SB_VERSION_NUM(sb) == XFS_SB_VERSION_5;
}

static bool
has_ro_feature(const struct xfs_sb *sb, uint32_t feature)
{
	return has_crc(sb) && (sb->sb_features_ro_compat & feature) != 0;
}

static unsigned int
agfl_size(const struct xfs_sb *sb)
{
	return (sb->sb_sectsize - (has_crc(sb) ? sizeof(struct xfs_agfl) : 0)) / 4;
}

/* A btree/data block must follow the four AG header sectors. */
static bool
valid_block(const struct xfs_sb *sb, uint32_t blocks, uint32_t block)
{
	uint32_t first = (4U * sb->sb_sectsize + sb->sb_blocksize - 1) /
	    sb->sb_blocksize;

	return block >= first && block < blocks;
}

/* Adapted xfs_btree_compute_maxlevels / xfs_btree_space_to_height. */
static unsigned int
tree_height(const struct xfs_sb *sb, size_t record, size_t key,
    uint64_t records, bool by_space)
{
	uint32_t usable = sb->sb_blocksize - (has_crc(sb) ?
	    XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN);
	uint32_t leaf = (usable / record) / 2;
	uint32_t node = (usable / (key + sizeof(__be32))) / 2;
	uint64_t blocks, left;
	unsigned int height = 1;

	if (by_space) {
		blocks = 2;
		left = records - 1;
		while (blocks < left) {
			left -= blocks;
			blocks *= node;
			height++;
		}
	} else {
		blocks = (records + leaf - 1) / leaf;
		while (blocks > 1) {
			blocks = (blocks + node - 1) / node;
			height++;
		}
	}
	return height;
}

static bool
valid_tree(const struct xfs_sb *sb, uint32_t blocks, __be32 root,
    __be32 level, unsigned int maxlevel)
{
	return valid_block(sb, blocks, be32_to_cpu(root)) &&
	    be32_to_cpu(level) >= 1 && be32_to_cpu(level) <= maxlevel;
}

static const char *
verify_agf(const struct xfs_sb *sb, uint32_t agno, uint32_t blocks,
    const struct xfs_agf *agf)
{
	uint32_t first, last, count, size = agfl_size(sb), active;
	unsigned int maxlevel;

	if (be32_to_cpu(agf->agf_magicnum) != XFS_AGF_MAGIC ||
	    !XFS_AGF_GOOD_VERSION(be32_to_cpu(agf->agf_versionnum)))
		return "invalid AGF magic or version";
	if (has_crc(sb) && memcmp(&agf->agf_uuid, &sb->sb_meta_uuid, 16) != 0)
		return "AGF UUID mismatch";
	if (be32_to_cpu(agf->agf_seqno) != agno ||
	    be32_to_cpu(agf->agf_length) != blocks)
		return "AGF allocation group mismatch";
	first = be32_to_cpu(agf->agf_flfirst);
	last = be32_to_cpu(agf->agf_fllast);
	count = be32_to_cpu(agf->agf_flcount);
	if (first >= size || last >= size || count > size)
		return "AGF free-list index out of bounds";
	active = count == 0 ? 0 : (last >= first ? last - first + 1 :
	    size - first + last + 1);
	if (active != count)
		return "AGF free-list reset required";
	if (be32_to_cpu(agf->agf_freeblks) > blocks ||
	    be32_to_cpu(agf->agf_longest) > be32_to_cpu(agf->agf_freeblks))
		return "invalid AGF free-space counters";
	maxlevel = tree_height(sb, sizeof(xfs_alloc_rec_t), sizeof(xfs_alloc_key_t),
	    ((uint64_t)sb->sb_agblocks + 1) / 2, false);
	if (!valid_tree(sb, blocks, agf->agf_bno_root, agf->agf_bno_level, maxlevel) ||
	    !valid_tree(sb, blocks, agf->agf_cnt_root, agf->agf_cnt_level, maxlevel))
		return "invalid AGF free-space btree";
	if ((has_crc(sb) || ((sb->sb_versionnum & XFS_SB_VERSION_MOREBITSBIT) &&
	    (sb->sb_features2 & XFS_SB_VERSION2_LAZYSBCOUNTBIT))) &&
	    be32_to_cpu(agf->agf_btreeblks) > blocks)
		return "invalid AGF btree block count";
	if (has_ro_feature(sb, XFS_SB_FEAT_RO_COMPAT_RMAPBT)) {
		maxlevel = tree_height(sb, sizeof(struct xfs_rmap_rec),
		    2 * sizeof(struct xfs_rmap_key), sb->sb_agblocks,
		    has_ro_feature(sb, XFS_SB_FEAT_RO_COMPAT_REFLINK));
		if (be32_to_cpu(agf->agf_rmap_blocks) > blocks ||
		    !valid_tree(sb, blocks, agf->agf_rmap_root,
		    agf->agf_rmap_level, maxlevel))
			return "invalid AGF reverse-map btree";
	}
	if (has_ro_feature(sb, XFS_SB_FEAT_RO_COMPAT_REFLINK)) {
		maxlevel = tree_height(sb, sizeof(struct xfs_refcount_rec),
		    sizeof(struct xfs_refcount_key), sb->sb_agblocks, false);
		if (be32_to_cpu(agf->agf_refcount_blocks) > blocks ||
		    !valid_tree(sb, blocks, agf->agf_refcount_root,
		    agf->agf_refcount_level, maxlevel))
			return "invalid AGF reference-count btree";
	}
	return NULL;
}

static const char *
verify_agi(const struct xfs_sb *sb, uint32_t agno, uint32_t blocks,
    const struct xfs_agi *agi)
{
	uint32_t ino;
	unsigned int i, maxlevel;
	uint64_t records = (UINT64_C(1) << (sb->sb_agblklog + sb->sb_inopblog)) /
	    XFS_INODES_PER_CHUNK;

	if (be32_to_cpu(agi->agi_magicnum) != XFS_AGI_MAGIC ||
	    !XFS_AGI_GOOD_VERSION(be32_to_cpu(agi->agi_versionnum)))
		return "invalid AGI magic or version";
	if (has_crc(sb) && memcmp(&agi->agi_uuid, &sb->sb_meta_uuid, 16) != 0)
		return "AGI UUID mismatch";
	if (be32_to_cpu(agi->agi_seqno) != agno ||
	    be32_to_cpu(agi->agi_length) != blocks)
		return "AGI allocation group mismatch";
	if (be32_to_cpu(agi->agi_count) > (uint64_t)blocks * sb->sb_inopblock ||
	    be32_to_cpu(agi->agi_freecount) > be32_to_cpu(agi->agi_count))
		return "invalid AGI inode counters";
	maxlevel = tree_height(sb, sizeof(xfs_inobt_rec_t), sizeof(xfs_inobt_key_t),
	    records, false);
	if (!valid_tree(sb, blocks, agi->agi_root, agi->agi_level, maxlevel))
		return "invalid AGI inode btree";
	if (has_ro_feature(sb, XFS_SB_FEAT_RO_COMPAT_FINOBT) &&
	    !valid_tree(sb, blocks, agi->agi_free_root, agi->agi_free_level, maxlevel))
		return "invalid AGI free-inode btree";
	if (has_ro_feature(sb, XFS_SB_FEAT_RO_COMPAT_INOBTCNT) &&
	    (be32_to_cpu(agi->agi_iblocks) > blocks ||
	     be32_to_cpu(agi->agi_fblocks) > blocks))
		return "invalid AGI btree block counters";
	/* agi_unlinked contains AG-relative inode numbers. */
	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++) {
		ino = be32_to_cpu(agi->agi_unlinked[i]);
		if (ino != NULLAGINO && !valid_block(sb, blocks, ino >> sb->sb_inopblog))
			return "AGI unlinked inode out of bounds";
	}
	return NULL;
}

static const char *
verify_agfl(const struct xfs_sb *sb, uint32_t agno, uint32_t blocks,
    const struct xfs_agf *agf, const void *data)
{
	struct xfs_agfl agfl;
	const uint8_t *bytes = data;
	uint32_t entry, i, size = agfl_size(sb);
	uint32_t first = be32_to_cpu(agf->agf_flfirst);
	uint32_t count = be32_to_cpu(agf->agf_flcount);
	size_t start = 0;

	if (has_crc(sb)) {
		memcpy(&agfl, data, sizeof(agfl));
		if (be32_to_cpu(agfl.agfl_magicnum) != XFS_AGFL_MAGIC)
			return "invalid AGFL magic";
		if (be32_to_cpu(agfl.agfl_seqno) != agno)
			return "AGFL allocation group mismatch";
		if (memcmp(&agfl.agfl_uuid, &sb->sb_meta_uuid, 16) != 0)
			return "AGFL UUID mismatch";
		start = sizeof(agfl);
		/* Unlike v4, v5 initializes every AGFL slot. */
		for (i = 0; i < size; i++) {
			memcpy(&entry, bytes + start + i * 4, sizeof(entry));
			if (be32_to_cpu(entry) != NULLAGBLOCK && be32_to_cpu(entry) >= blocks)
				return "AGFL block out of bounds";
		}
	}
	/* Validate only the active ring on v4: unused slots may be garbage. */
	for (i = 0; i < count; i++) {
		memcpy(&entry, bytes + start + ((first + i) % size) * 4, sizeof(entry));
		if (!valid_block(sb, blocks, be32_to_cpu(entry)))
			return "invalid active AGFL block";
	}
	return NULL;
}

int
xfs_read_ag(const struct xfs_reader *reader, uint32_t agno, void *scratch,
    size_t capacity, struct xfs_ag_headers *result, const char **why)
{
	struct xfs_ag_headers headers;
	const struct xfs_sb *sb;
	const char *message = NULL;
	uint64_t offset;
	uint32_t blocks;
	unsigned int sector;
	size_t crc_offset;
	int error = EINVAL;

	if (why != NULL)
		*why = NULL;
	if (reader == NULL || scratch == NULL || result == NULL) {
		message = "null allocation group argument";
		goto fail;
	}
	sb = &reader->sb;
	if (sb->sb_sectsize < XFS_MIN_SECTORSIZE ||
	    sb->sb_sectsize > XFS_MAX_SECTORSIZE ||
	    sb->sb_agblklog + sb->sb_inopblog > 32 ||
	    xfs_ag_geometry(sb, agno, &offset, &blocks) != 0) {
		message = "invalid allocation group geometry";
		goto fail;
	}
	if (capacity < sb->sb_sectsize) {
		error = EMSGSIZE;
		message = "allocation group scratch buffer too small";
		goto fail;
	}
	for (sector = 1; sector <= 3; sector++) {
		error = xfs_io_read(&reader->io, offset + sector * sb->sb_sectsize,
		    scratch, sb->sb_sectsize);
		if (error != 0) {
			message = "allocation group header read failed";
			goto fail;
		}
		crc_offset = sector == 1 ? XFS_AGF_CRC_OFF :
		    (sector == 2 ? XFS_AGI_CRC_OFF : XFS_AGFL_CRC_OFF);
		error = EINVAL;
		if (has_crc(sb) && xfs_verify_metadata_crc(scratch, sb->sb_sectsize,
		    crc_offset) != 0) {
			message = sector == 1 ? "AGF CRC32C mismatch" :
			    (sector == 2 ? "AGI CRC32C mismatch" : "AGFL CRC32C mismatch");
			goto fail;
		}
		if (sector == 1) {
			memcpy(&headers.agf, scratch, sizeof(headers.agf));
			message = verify_agf(sb, agno, blocks, &headers.agf);
		} else if (sector == 2) {
			memcpy(&headers.agi, scratch, sizeof(headers.agi));
			message = verify_agi(sb, agno, blocks, &headers.agi);
		} else {
			message = verify_agfl(sb, agno, blocks, &headers.agf, scratch);
		}
		if (message != NULL)
			goto fail;
	}
	*result = headers;
	return 0;
fail:
	if (why != NULL)
		*why = message;
	return error;
}
