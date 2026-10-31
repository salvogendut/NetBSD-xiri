/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "inode_fixture.h"

static struct xfs_inode_stat
expect_attributes(int expected)
{
	struct xfs_inode_stat st, before;
	const char *why;
	memset(&st, 0xa5, sizeof(st));
	memcpy(&before, &st, sizeof(st));
	CHECK(xfs_inode_getattr(&reader, &inode, &st, &why) == expected);
	CHECK((why == NULL) == (expected == 0));
	if (expected != 0)
		CHECK(memcmp(&st, &before, sizeof(st)) == 0);
	checks++;
	return st;
}

static void
attribute_checks(void)
{
	static const size_t times[] = {
		offsetof(struct xfs_dinode, di_atime), offsetof(struct xfs_dinode, di_mtime),
		offsetof(struct xfs_dinode, di_ctime), offsetof(struct xfs_dinode, di_crtime)
	};
	static const int64_t seconds[] = { INT32_MIN, -1, 0, INT32_MAX, INT64_C(4102444800) };
	struct xfs_inode_stat st;
	unsigned int v, big, i, j;
	uint8_t *p;
	uint64_t value;
	const char *why;
	for (v = 0; v <= 1; v++) {
		for (big = 0; big <= v; big++) {
			for (i = 0; i < (big ? 5U : 4U); i++) {
				setup(v, 0, false);
				p = disk + 16 * BS;
				if (big) {
					reader.sb.sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_BIGTIME;
					put64(p + offsetof(struct xfs_dinode, di_flags2), XFS_DIFLAG2_BIGTIME);
				}
				put32(p + offsetof(struct xfs_dinode, di_uid), 1234);
				put32(p + offsetof(struct xfs_dinode, di_gid), 5678);
				put32(p + offsetof(struct xfs_dinode, di_nlink), 3);
				put32(p + offsetof(struct xfs_dinode, di_gen), 0xfedcba98);
				value = big ? (uint64_t)(seconds[i] + INT64_C(2147483648)) * 1000000000 + 999999999 :
				    (uint64_t)(uint32_t)seconds[i] << 32 | 999999999;
				for (j = 0; j < (v ? 4U : 3U); j++)
					put64(p + times[j], value);
				inode_crc(); expect_inode(number, 0, NULL);
				st = expect_attributes(0);
				CHECK(st.uid == 1234 && st.gid == 5678 && st.nlink == 3);
				CHECK(st.generation == 0xfedcba98 && st.allocated_bytes == 4 * BS);
				CHECK(st.atime.seconds == seconds[i] && st.atime.nanoseconds == 999999999);
				CHECK(st.mtime.seconds == seconds[i] && st.ctime.seconds == seconds[i]);
				CHECK(st.has_birthtime == (v != 0) && !st.has_attributes);
				if (v) CHECK(st.birthtime.seconds == seconds[i]);
			}
		}
	}
	for (i = 0; i < 4; i++) {
		setup(true, 0, false);
		put64(disk + 16 * BS + times[i], 1000000000);
		inode_crc(); expect_inode(number, 0, NULL); expect_attributes(EINVAL);
		setup(true, 0, false);
		reader.sb.sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_BIGTIME;
		put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_flags2), XFS_DIFLAG2_BIGTIME);
		put64(disk + 16 * BS + times[i], UINT64_MAX);
		inode_crc(); expect_inode(number, 0, NULL); expect_attributes(EINVAL);
	}
	setup(false, 0, false);
	p = disk + 16 * BS;
	p[offsetof(struct xfs_dinode, di_version)] = 1;
	put16(p + offsetof(struct xfs_dinode, di_onlink), 123);
	put32(p + offsetof(struct xfs_dinode, di_nlink), 456);
	expect_inode(number, 0, NULL);
	CHECK(expect_attributes(0).nlink == 123);
	setup(true, 0, false);
	put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_nblocks), UINT64_MAX);
	inode_crc(); expect_inode(number, 0, NULL); expect_attributes(EOVERFLOW);
	setup(true, 0, false);
	p = disk + 16 * BS;
	p[offsetof(struct xfs_dinode, di_forkoff)] = 32;
	p[offsetof(struct xfs_dinode, di_aformat)] = XFS_DINODE_FMT_LOCAL;
	inode_crc(); expect_inode(number, 0, NULL);
	CHECK(expect_attributes(0).has_attributes);
	CHECK(xfs_inode_getattr(NULL, &inode, &st, &why) == EINVAL);
	CHECK(xfs_inode_getattr(&reader, NULL, &st, &why) == EINVAL);
	CHECK(xfs_inode_getattr(&reader, &inode, NULL, &why) == EINVAL);
}

int
main(void)
{
	unsigned int v, i, j, h;
	uint8_t *p;
	const char *why;
	size_t done;

	attribute_checks();
	/* A 64-inode record need not start at a multiple of 64. Exercise
	 * block-aligned chunks and their interior keys, as found in mkfs v4. */
	for (v = 0; v <= 1; v++) {
		setup(v, 0, false);
		memcpy(disk + 20 * BS, disk + 16 * BS, BS);
		number += 32;
		if (v) {
			put64(disk + 20 * BS + offsetof(struct xfs_dinode, di_ino), number);
			metadata_checksum(disk + 20 * BS, 512, XFS_DINODE_CRC_OFF);
		}
		h = v ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN;
		put32(disk + 3 * BS + h, 160);
		tree_crc(3, false);
		expect_inode(number, 0, NULL);
		expect_inode(number + 1, ENOENT, "not allocated");
		memcpy(disk + 4 * BS, disk + 3 * BS, BS);
		if (v) put64(disk + 4 * BS + 16, (base + 4 * BS) / 512);
		tree_crc(4, false);
		h = tree(3, false, 1, 1);
		put32(disk + 3 * BS + h, 160);
		put32(disk + 3 * BS + h + ((BS - h) / 8) * 4, 4);
		tree_crc(3, false);
		put32(disk + 1024 + 24, 2);
		if (v) metadata_checksum(disk + 1024, 512, XFS_AGI_CRC_OFF);
		expect_inode(number, 0, NULL);
		put32(disk + 4 * BS + h, 161);
		tree_crc(4, false);
		expect_inode(number, EINVAL, "chunk range");
		put32(disk + 3 * BS + h, 161);
		tree_crc(3, false);
		expect_inode(number, EINVAL, "key or pointer");
	}

	for (v = 0; v < 2; v++) {
		setup(v, 2, false);
		expect_inode(number, 0, NULL);
		expect_read(0, 6 * BS, 0, 6 * BS - 17);
		for (i = 0; i < 6 * BS - 17; i++) {
			unsigned int block = i / BS;
			CHECK(output[i + 1] == (block < 2 ? 32 + block : block == 5 ? 35 : 0));
		}
		expect_read(BS - 3, 10, 0, 10);
		CHECK(output[1] == 32 && output[4] == 33);
		expect_read(6 * BS - 20, 10, 0, 3);
		expect_read(6 * BS, 10, 0, 0);
		expect_read(UINT64_MAX, 0, EINVAL, 0);
		expect_inode(number + 1, ENOENT, "not allocated");
		expect_inode(UINT64_MAX, EINVAL, "bounds");
		setup(v, 2, true);
		expect_inode(number, 0, NULL);
		expect_read(29 * BS + 1, 5 * BS, 0, 5 * BS);
		for (i = 0; i < 5 * BS; i++) {
			unsigned int block = (29 * BS + 1 + i) / BS;
			CHECK(output[i + 1] == (block % 2 ? 0 : 64 + block / 2));
		}
	}
	/* Every inode and inode-tree byte is covered by the checksum. */
	setup(true, 0, false);
	for (i = 0; i < 512; i++) {
		disk[16 * BS + i] ^= 1;
		expect_inode(number, EINVAL, NULL);
		disk[16 * BS + i] ^= 1;
	}
	for (i = 0; i < BS; i++) {
		disk[3 * BS + i] ^= 1;
		expect_inode(number, EINVAL, "CRC32C");
		disk[3 * BS + i] ^= 1;
	}
	/* Fail/shorten every read needed for inode allocation and core loading. */
	for (i = 1; i <= 5; i++) {
		setup(true, 0, false);
		fail_call = i;
		expect_inode(number, EACCES, "read failed");
		CHECK(calls == i);
		setup(true, 0, false);
		short_call = i;
		expect_inode(number, EIO, "read failed");
	}
	/* Interior inobt node; use capacity-derived pointer placement. */
	setup(true, 2, false);
	memcpy(disk + 4 * BS, disk + 3 * BS, BS);
	put64(disk + 4 * BS + 16, (base + 4 * BS) / 512);
	tree_crc(4, false);
	h = tree(3, false, 1, 1);
	put32(disk + 3 * BS + h, 128);
	put32(disk + 3 * BS + h + ((BS - h) / 8) * 4, 4);
	tree_crc(3, false);
	put32(disk + 1024 + 24, 2);
	metadata_checksum(disk + 1024, 512, XFS_AGI_CRC_OFF);
	expect_inode(number, 0, NULL);
	put32(disk + 3 * BS + h + ((BS - h) / 8) * 4, 3);
	tree_crc(3, false);
	expect_inode(number, EINVAL, "level mismatch");
	CHECK(calls < 20);
	/* Sparse chunks: a hole is unavailable even if its free bit is clear. */
	setup(true, 0, false);
	reader.sb.sb_features_incompat |= XFS_SB_FEAT_INCOMPAT_SPINODES;
	p = disk + 3 * BS + XFS_BTREE_SBLOCK_CRC_LEN;
	put16(p + 4, 2);
	p[6] = 60;
	p[7] = 59;
	put64(p + 8, (UINT64_MAX - 1) & ~UINT64_C(0xf0));
	tree_crc(3, false);
	expect_inode(number, 0, NULL);
	expect_inode(number + 4, ENOENT, "not allocated");
	p[6] = 64;
	tree_crc(3, false);
	expect_inode(number, EINVAL, "counters");
	/* Recomputed checksums cannot disguise invalid inode core fields. */
#define BAD16(field, value) do { \
	setup(true, 0, false); \
	put16(disk + 16 * BS + offsetof(struct xfs_dinode, field), value); \
	inode_crc(); expect_inode(number, EINVAL, NULL); \
} while (0)
#define BAD64(field, value) do { \
	setup(true, 0, false); \
	put64(disk + 16 * BS + offsetof(struct xfs_dinode, field), value); \
	inode_crc(); expect_inode(number, EINVAL, NULL); \
} while (0)
	BAD16(di_mode, 0);
	BAD16(di_onlink, 1);
	BAD64(di_size, UINT64_MAX);
	BAD64(di_ino, 129);
	BAD64(di_nblocks, 2);
	BAD64(di_flags2, XFS_DIFLAG2_BIGTIME);
	BAD64(di_flags2, XFS_DIFLAG2_NREXT64);
	BAD64(di_flags2, UINT64_C(1) << 63);
	for (i = 0; i < 5; i++) {
		setup(true, 0, false);
		p = disk + 16 * BS;
		if (i == 0) p[offsetof(struct xfs_dinode, di_forkoff)] = 255;
		if (i == 1) p[offsetof(struct xfs_dinode, di_uuid)] ^= 1;
		if (i == 2) p[offsetof(struct xfs_dinode, di_format)] = XFS_DINODE_FMT_LOCAL;
		if (i == 3) put32(p + offsetof(struct xfs_dinode, di_nextents), 22);
		if (i == 4) put32(p + offsetof(struct xfs_dinode, di_extsize), 1);
		inode_crc();
		expect_inode(number, EINVAL, NULL);
	}
	for (i = 0; i < 6; i++) {
		setup(true, 0, false);
		p = disk + 16 * BS + sizeof(struct xfs_dinode);
		if (i == 0) extent(p, 0, 32, 0, false);
		if (i == 1) extent(p + 16, 1, 34, 1, false);
		if (i == 2) extent(p, 0, 19999, 2, false);
		if (i == 3) extent(p, 0, 0, 2, false);
		if (i == 4) extent(p, 0, (UINT64_C(1) << 15) + 100, 2, false);
		if (i == 5) extent(p, 0, UINT64_C(1) << 51, 2, false);
		inode_crc();
		expect_inode(number, EINVAL, "extent");
	}
	/* Corrupt bmap nodes, leaves, identity, levels and parent key intervals. */
	setup(true, 0, true);
	expect_inode(number, 0, NULL);
	for (i = 0; i < BS; i++) {
		disk[42 * BS + i] ^= 1;
		expect_read(0, 1, EINVAL, 0);
		disk[42 * BS + i] ^= 1;
	}
	for (i = 0; i < 9; i++) {
		setup(true, 0, true);
		expect_inode(number, 0, NULL);
		p = disk + 42 * BS;
		if (i == 0) put64(p + 24, 0);
		if (i == 1) put64(p + 56, number + 1);
		if (i == 2) p[40] ^= 1;
		if (i == 3) put64(p + 8, 42);
		if (i == 4) put16(p + 4, 1);
		if (i == 5) put16(p + 6, UINT16_MAX);
		if (i == 6) put16(p + 6, 0);
		if (i == 7) extent(p + XFS_BTREE_LBLOCK_CRC_LEN, 1, 64, 1, false);
		if (i == 8) extent(p + XFS_BTREE_LBLOCK_CRC_LEN + 16, 0, 65, 1, false);
		tree_crc(42, true);
		expect_read(0, 1, EINVAL, 0);
	}
	for (i = 1; i <= 3; i++) {
		for (j = 0; j < 2; j++) {
			setup(true, 0, true);
			expect_inode(number, 0, NULL);
			calls = 0;
			if (j) short_call = i; else fail_call = i;
			expect_read(0, 1, j ? EIO : EACCES, 0);
		}
	}
	/* Header fields are checked even when checksums have been recomputed. */
	for (i = 0; i < 7; i++) {
		setup(true, 2, false);
		p = disk + 3 * BS;
		if (i == 0) put64(p + 16, 0);
		if (i == 1) put32(p + 48, 0);
		if (i == 2) p[32] ^= 1;
		if (i == 3) put32(p + 8, 3);
		if (i == 4) put16(p + 6, UINT16_MAX);
		if (i == 5) put32(p + XFS_BTREE_SBLOCK_CRC_LEN, 129);
		if (i == 6) put32(p + XFS_BTREE_SBLOCK_CRC_LEN + 4, 62);
		tree_crc(3, false);
		expect_inode(number, EINVAL, NULL);
	}
	/* Bmap root/internal corruption and cycles are bounded by level checks. */
	for (i = 0; i < 6; i++) {
		setup(true, 0, true);
		p = disk + 16 * BS + sizeof(struct xfs_dinode);
		if (i == 0) put16(p, 0);
		if (i == 1) put16(p, UINT16_MAX);
		if (i == 2) put16(p + 2, UINT16_MAX);
		if (i == 3) put64(p + 12, 0);
		if (i == 4) put64(p + 4 + ((512 - 176 - 4) / 16) * 8, UINT64_MAX);
		if (i == 5) put64(p + 4 + ((512 - 176 - 4) / 16) * 8, 41);
		inode_crc();
		if (i < 5)
			expect_inode(number, EINVAL, NULL);
		else {
			expect_inode(number, 0, NULL);
			expect_read(0, 1, EINVAL, 0);
		}
	}
	setup(true, 0, true);
	expect_inode(number, 0, NULL);
	h = XFS_BTREE_LBLOCK_CRC_LEN;
	put64(disk + 40 * BS + h + ((BS - h) / 16) * 8, 40);
	tree_crc(40, true);
	calls = 0;
	expect_read(0, 1, EINVAL, 0);
	CHECK(calls == 2);
	/* Hole and unwritten data never reach the device read callback. */
	setup(true, 0, false);
	expect_inode(number, 0, NULL);
	calls = 0;
	expect_read(2 * BS, 3 * BS, 0, 3 * BS);
	CHECK(calls == 0);
	for (i = 1; i <= 3 * BS; i++) CHECK(output[i] == 0);
	/* Offsets beyond 4 GiB and EOF use 64-bit arithmetic. */
	setup(true, 2, false);
	p = disk + 16 * BS;
	put64(p + offsetof(struct xfs_dinode, di_size), (UINT64_C(1) << 34) + 73);
	put32(p + offsetof(struct xfs_dinode, di_nextents), 1);
	put64(p + offsetof(struct xfs_dinode, di_nblocks), 1);
	extent(p + sizeof(struct xfs_dinode), UINT64_C(1) << 22, agbase | 32, 1, false);
	inode_crc();
	expect_inode(number, 0, NULL);
	expect_read((UINT64_C(1) << 34) - 3, 100, 0, 76);
	CHECK(output[1] == 0 && output[3] == 0 && output[4] == 32 && output[76] == 32);
	/* Local symlinks, empty files and unsupported remote symlinks. */
	setup(true, 0, false);
	p = disk + 16 * BS;
	put16(p + offsetof(struct xfs_dinode, di_mode), S_IFLNK | 0777);
	p[offsetof(struct xfs_dinode, di_format)] = XFS_DINODE_FMT_LOCAL;
	put32(p + offsetof(struct xfs_dinode, di_nextents), 0);
	put64(p + offsetof(struct xfs_dinode, di_nblocks), 0);
	put64(p + offsetof(struct xfs_dinode, di_size), 6);
	memcpy(p + sizeof(struct xfs_dinode), "/hello", 6);
	inode_crc();
	expect_inode(number, 0, NULL);
	expect_read(1, 10, 0, 5);
	CHECK(memcmp(output + 1, "hello", 5) == 0);
	p[sizeof(struct xfs_dinode) + 1] = 0;
	inode_crc();
	expect_inode(number, EINVAL, "symlink");
	setup(true, 0, false);
	p = disk + 16 * BS;
	put32(p + offsetof(struct xfs_dinode, di_nextents), 0);
	put64(p + offsetof(struct xfs_dinode, di_nblocks), 0);
	put64(p + offsetof(struct xfs_dinode, di_size), 0);
	inode_crc();
	expect_inode(number, 0, NULL);
	expect_read(0, 10, 0, 0);
	setup(true, 3, false);
	reader.sb.sb_dblocks = 75000;
	put32(disk + 512 + 12, 15000);
	put32(disk + 1024 + 12, 15000);
	metadata_checksum(disk + 512, 512, XFS_AGF_CRC_OFF);
	metadata_checksum(disk + 1024, 512, XFS_AGI_CRC_OFF);
	expect_inode(number, 0, NULL);
	expect_read(0, 1, 0, 1);
	extent(disk + 16 * BS + sizeof(struct xfs_dinode), 0, agbase | 14999, 2, false);
	inode_crc();
	expect_inode(number, EINVAL, "extent");
	setup(true, 0, false);
	reader.sb.sb_features_ro_compat |= UINT32_C(1) << 31;
	expect_inode(number, EOPNOTSUPP, "unknown read-only");
	/* V4 has no meaning for fields reserved for v5 feature flags. */
	setup(false, 0, false);
	reader.sb.sb_features_incompat = UINT32_MAX;
	reader.sb.sb_features_ro_compat = UINT32_MAX;
	expect_inode(number, 0, NULL);
	expect_read(0, 1, 0, 1);
	setup(true, 0, false);
	expect_inode(number, 0, NULL);
	calls = 0;
	CHECK(xfs_read_file(&reader, &inode, 0, output, 1, &done, scratch, BS - 1, &why) == EMSGSIZE);
	CHECK(xfs_read_inode(&reader, number, scratch, BS - 1, &inode, &why) == EMSGSIZE);
	CHECK(calls == 0);
	CHECK(xfs_read_file(NULL, &inode, 0, output, 1, &done, scratch, BS, &why) == EINVAL);
	CHECK(xfs_read_inode(NULL, number, scratch, BS, &inode, &why) == EINVAL);
	printf("Passed %u inode, allocation, bmap and file-read checks\n", checks);
	return 0;
}
