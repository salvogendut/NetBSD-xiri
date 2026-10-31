/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#ifndef _XFS_TEST_FIXTURE_H_
#define _XFS_TEST_FIXTURE_H_
#include "xfs_io.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { \
	if (!(x)) { \
		fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); \
		exit(1); \
	} \
} while (0)

#define IMAGE_BYTES (UINT64_C(4) * 20000 * 4096)

union sector {
	struct xfs_dsb sb;
	struct xfs_agf agf;
	struct xfs_agi agi;
	struct xfs_agfl agfl;
	uint8_t bytes[XFS_MAX_SECTORSIZE];
};

/* Table-driven test checksum; real mkfs.xfs checksums are also tested. */
static void
metadata_checksum(void *data, unsigned int length, size_t offset)
{
	uint32_t table[256], value, crc = UINT32_MAX;
	unsigned int i, j;
	uint8_t *bytes = data;
	uint32_t encoded;

	CHECK(offset <= length && length - offset >= 4);
	for (i = 0; i < 256; i++) {
		value = i;
		for (j = 0; j < 8; j++)
			value = (value >> 1) ^ ((value & 1) ? 0x82f63b78U : 0);
		table[i] = value;
	}
	memset(bytes + offset, 0, 4);
	for (i = 0; i < length; i++)
		crc = table[(crc ^ bytes[i]) & 255] ^ (crc >> 8);
	encoded = htole32(~crc);
	memcpy(bytes + offset, &encoded, 4);
}

static void
checksum(union sector *s)
{
	metadata_checksum(s->bytes, be16_to_cpu(s->sb.sb_sectsize), XFS_SB_CRC_OFF);
}

static void
fixture(union sector *s, bool v5)
{
	uint16_t version;

	memset(s, 0, sizeof(*s));
	s->sb.sb_magicnum = cpu_to_be32(XFS_SB_MAGIC);
	s->sb.sb_blocksize = cpu_to_be32(4096);
	s->sb.sb_dblocks = cpu_to_be64(80000);
	s->sb.sb_agblocks = cpu_to_be32(20000); /* Not a power of two. */
	s->sb.sb_agcount = cpu_to_be32(4);
	s->sb.sb_logstart = cpu_to_be64((UINT64_C(1) << 15) | 100);
	s->sb.sb_logblocks = cpu_to_be32(1000);
	s->sb.sb_rootino = cpu_to_be64(128);
	s->sb.sb_rextsize = cpu_to_be32(1);
	s->sb.sb_sectsize = cpu_to_be16(512);
	s->sb.sb_inodesize = cpu_to_be16(512);
	s->sb.sb_inopblock = cpu_to_be16(8);
	s->sb.sb_blocklog = 12;
	s->sb.sb_sectlog = 9;
	s->sb.sb_inodelog = 9;
	s->sb.sb_inopblog = 3;
	s->sb.sb_agblklog = 15;
	s->sb.sb_imax_pct = 25;
	memset(&s->sb.sb_uuid, 0xab, sizeof(s->sb.sb_uuid));
	memset(&s->sb.sb_meta_uuid, 0xcd, sizeof(s->sb.sb_meta_uuid));
	version = (v5 ? 5 : 4) | XFS_SB_VERSION_NLINKBIT |
	    XFS_SB_VERSION_ALIGNBIT | XFS_SB_VERSION_LOGV2BIT |
	    XFS_SB_VERSION_EXTFLGBIT | XFS_SB_VERSION_DIRV2BIT |
	    XFS_SB_VERSION_MOREBITSBIT;
	s->sb.sb_versionnum = cpu_to_be16(version);
	s->sb.sb_features2 = cpu_to_be32(XFS_SB_VERSION2_LAZYSBCOUNTBIT |
	    XFS_SB_VERSION2_ATTR2BIT | XFS_SB_VERSION2_PROJID32BIT |
	    (v5 ? XFS_SB_VERSION2_CRCBIT : 0));
	if (v5) {
		s->sb.sb_features_incompat = cpu_to_be32(XFS_SB_FEAT_INCOMPAT_FTYPE);
		checksum(s);
	}
}

#endif
