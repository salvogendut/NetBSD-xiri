/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#include "xfs_probe.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "fixture.h"

static unsigned int checks;

static struct xfs_sb
expect(const void *data, size_t length, uint64_t bytes, int error)
{
	struct xfs_sb sb, before;
	const char *why = NULL;
	int actual;

	memset(&sb, 0xa5, sizeof(sb));
	memcpy(&before, &sb, sizeof(sb));
	actual = xfs_probe_superblock(data, length, bytes, &sb, &why);
	if (actual != error)
		fprintf(stderr, "expected %d, got %d (%s)\n", error, actual,
		    why != NULL ? why : "success");
	CHECK(actual == error);
	CHECK((why == NULL) == (error == 0));
	if (error != 0)
		CHECK(memcmp(&sb, &before, sizeof(sb)) == 0);
	checks++;
	return sb;
}

static void
expect_sector(union sector *s, int error)
{

	checksum(s);
	(void)expect(s->bytes, sizeof(*s), IMAGE_BYTES, error);
}

int
main(void)
{
	union sector s;
	struct xfs_sb sb;
	uint8_t *shortbuf, *unaligned;
	unsigned int i;

	fixture(&s, true);
	sb = expect(s.bytes, sizeof(s), IMAGE_BYTES, 0);
	CHECK(sb.sb_blocksize == 4096 && sb.sb_dblocks == 80000);
	CHECK(sb.sb_agcount == 4 && sb.sb_rootino == 128);
	CHECK(sb.sb_uquotino == NULLFSINO && sb.sb_crc == 0);
	CHECK(memcmp(&sb.sb_meta_uuid, &s.sb.sb_uuid, 16) == 0);
	s.sb.sb_features_incompat |= cpu_to_be32(XFS_SB_FEAT_INCOMPAT_META_UUID);
	checksum(&s);
	sb = expect(s.bytes, sizeof(s), IMAGE_BYTES, 0);
	CHECK(memcmp(&sb.sb_meta_uuid, &s.sb.sb_meta_uuid, 16) == 0);

	/* Exercise the unchanged upstream v4 quota conversion. */
	fixture(&s, false);
	s.sb.sb_qflags = cpu_to_be16(XFS_PQUOTA_ACCT | XFS_OQUOTA_ENFD);
	s.sb.sb_gquotino = cpu_to_be64(129);
	sb = expect(s.bytes, sizeof(s), IMAGE_BYTES, 0);
	CHECK(sb.sb_pquotino == 129 && sb.sb_gquotino == NULLFSINO);
	CHECK(sb.sb_qflags == (XFS_PQUOTA_ACCT | XFS_PQUOTA_ENFD));

	fixture(&s, true);
	/* Real allocation boundaries make short reads visible to ASan. */
	for (i = 0; i < 512; i++) {
		shortbuf = malloc(i == 0 ? 1 : i);
		CHECK(shortbuf != NULL);
		memcpy(shortbuf, s.bytes, i);
		(void)expect(shortbuf, i, IMAGE_BYTES, EMSGSIZE);
		free(shortbuf);
	}
	unaligned = malloc(sizeof(s) + 1);
	CHECK(unaligned != NULL);
	memcpy(unaligned + 1, s.bytes, sizeof(s));
	(void)expect(unaligned + 1, sizeof(s), IMAGE_BYTES, 0);
	free(unaligned);
	(void)expect(NULL, sizeof(s), IMAGE_BYTES, EINVAL);
	CHECK(xfs_probe_superblock(s.bytes, sizeof(s), IMAGE_BYTES, NULL, NULL)
	    == EINVAL);
	CHECK(xfs_probe_superblock(s.bytes, sizeof(s), IMAGE_BYTES, &sb, NULL) == 0);
	(void)expect(s.bytes, sizeof(s), IMAGE_BYTES - 1, EMSGSIZE);

	/* All byte corruption, including sector padding, must fail the CRC. */
	for (i = 0; i < 512; i++) {
		const char *why;
		s.bytes[i] ^= 0x80;
		CHECK(xfs_probe_superblock(s.bytes, sizeof(s), IMAGE_BYTES, &sb, &why)
		    != 0);
		s.bytes[i] ^= 0x80;
		checks++;
	}
	fixture(&s, true);
	s.sb.sb_sectsize = cpu_to_be16(4096);
	s.sb.sb_sectlog = 12;
	checksum(&s);
	(void)expect(s.bytes, 512, IMAGE_BYTES, EMSGSIZE);
	(void)expect(s.bytes, 4096, IMAGE_BYTES, 0);
	s.bytes[4095] ^= 1;
	(void)expect(s.bytes, sizeof(s), IMAGE_BYTES, EINVAL);

#define BAD_FIELD(field, value, error) do { \
	fixture(&s, true); \
	s.sb.field = (value); \
	expect_sector(&s, (error)); \
} while (0)
	BAD_FIELD(sb_magicnum, 0, EINVAL);
	BAD_FIELD(sb_versionnum, cpu_to_be16(3), EOPNOTSUPP);
	BAD_FIELD(sb_blocklog, 255, EINVAL);
	BAD_FIELD(sb_sectlog, 255, EINVAL);
	BAD_FIELD(sb_inodelog, 255, EINVAL);
	BAD_FIELD(sb_agblklog, 255, EINVAL);
	BAD_FIELD(sb_inopblog, 255, EINVAL);
	BAD_FIELD(sb_dirblklog, 255, EINVAL);
	BAD_FIELD(sb_blocksize, cpu_to_be32(8192), EINVAL);
	BAD_FIELD(sb_inopblock, cpu_to_be16(16), EINVAL);
	BAD_FIELD(sb_inodesize, cpu_to_be16(256), EINVAL);
	BAD_FIELD(sb_agblocks, 0, EINVAL);
	BAD_FIELD(sb_agcount, 0, EINVAL);
	BAD_FIELD(sb_agcount, cpu_to_be32(3), EINVAL);
	BAD_FIELD(sb_dblocks, 0, EINVAL);
	BAD_FIELD(sb_dblocks, cpu_to_be64(UINT64_MAX), EINVAL);
	BAD_FIELD(sb_inprogress, 1, EINVAL);
	BAD_FIELD(sb_shared_vn, 1, EINVAL);
	BAD_FIELD(sb_imax_pct, 101, EINVAL);
	BAD_FIELD(sb_rootino, 0, EINVAL);
	BAD_FIELD(sb_rootino, cpu_to_be64(UINT64_MAX), EINVAL);
	BAD_FIELD(sb_rootino, cpu_to_be64(UINT64_C(4) << 18), EINVAL);
	BAD_FIELD(sb_rootino, cpu_to_be64(UINT64_C(25000) << 3), EINVAL);
	BAD_FIELD(sb_logblocks, 0, EINVAL);
	BAD_FIELD(sb_logblocks, cpu_to_be32(20000), EINVAL);
	BAD_FIELD(sb_logstart, cpu_to_be64(UINT64_C(4) << 15), EINVAL);
	BAD_FIELD(sb_logstart, cpu_to_be64(25000), EINVAL);
	BAD_FIELD(sb_logstart, 0, EOPNOTSUPP);
	BAD_FIELD(sb_rblocks, cpu_to_be64(1024), EOPNOTSUPP);
	BAD_FIELD(sb_features_incompat, cpu_to_be32(0x80000000), EOPNOTSUPP);
	BAD_FIELD(sb_features_incompat,
	    cpu_to_be32(XFS_SB_FEAT_INCOMPAT_NEEDSREPAIR), EINVAL);
	BAD_FIELD(sb_features_log_incompat, cpu_to_be32(1), EOPNOTSUPP);
	/* Unknown read-only-compatible features do not affect SB inspection. */
	BAD_FIELD(sb_features_ro_compat, cpu_to_be32(0x80000000), 0);
	BAD_FIELD(sb_features_compat, cpu_to_be32(0x80000000), 0);

	/* Last AG may be short; encoded inode/log numbers must respect it. */
	fixture(&s, true);
	s.sb.sb_dblocks = cpu_to_be64(75000);
	expect_sector(&s, 0);
	s.sb.sb_rootino = cpu_to_be64((UINT64_C(3) << 18) | (16000U << 3));
	expect_sector(&s, EINVAL);
	s.sb.sb_rootino = cpu_to_be64(128);
	s.sb.sb_logstart = cpu_to_be64((UINT64_C(3) << 15) | 14999);
	expect_sector(&s, EINVAL);

	printf("Passed %u superblock checks\n", checks);
	return 0;
}
