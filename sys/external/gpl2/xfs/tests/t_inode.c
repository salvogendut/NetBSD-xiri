/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "fixture.h"
#include "xfs_inode.h"
#include <errno.h>
#include <sys/stat.h>

#define BS 4096U
static uint8_t disk[96 * BS], scratch[BS + 1], output[48 * BS + 1];
static union sector primary;
static struct xfs_reader reader;
static struct xfs_inode_image inode;
static uint64_t base, agbase, number;
static bool crc;
static unsigned int calls, fail_call, short_call, checks;

static void
put16(void *p, uint16_t n)
{
	__be16 v = cpu_to_be16(n);
	memcpy(p, &v, sizeof(v));
}

static void
put32(void *p, uint32_t n)
{
	__be32 v = cpu_to_be32(n);
	memcpy(p, &v, sizeof(v));
}

static void
put64(void *p, uint64_t n)
{
	__be64 v = cpu_to_be64(n);
	memcpy(p, &v, sizeof(v));
}

static int
read_image(void *cookie, uint64_t offset, void *data, size_t length, size_t *done)
{
	CHECK(cookie == disk);
	calls++;
	if (calls == fail_call)
		return EACCES;
	if (offset == 0 && length == 512)
		memcpy(data, primary.bytes, length);
	else {
		CHECK(offset >= base && offset - base + length <= sizeof(disk));
		memcpy(data, disk + offset - base, length);
	}
	*done = calls == short_call ? length - 1 : length;
	return 0;
}

static void
tree_crc(unsigned int block, bool bmap)
{
	if (crc)
		metadata_checksum(disk + block * BS, BS,
		    bmap ? XFS_BTREE_LBLOCK_CRC_OFF : XFS_BTREE_SBLOCK_CRC_OFF);
}

static unsigned int
tree(unsigned int block, bool bmap, unsigned int level, unsigned int n)
{
	uint8_t *p = disk + block * BS;
	memset(p, 0, BS);
	put32(p, bmap ? (crc ? XFS_BMAP_CRC_MAGIC : XFS_BMAP_MAGIC) :
	    (crc ? XFS_IBT_CRC_MAGIC : XFS_IBT_MAGIC));
	put16(p + 4, level);
	put16(p + 6, n);
	if (bmap) {
		put64(p + 8, NULLFSBLOCK);
		put64(p + 16, NULLFSBLOCK);
		if (crc) {
			put64(p + 24, (base + block * BS) / 512);
			memcpy(p + 40, &reader.sb.sb_meta_uuid, 16);
			put64(p + 56, number);
		}
		return crc ? XFS_BTREE_LBLOCK_CRC_LEN : XFS_BTREE_LBLOCK_LEN;
	}
	put32(p + 8, NULLAGBLOCK);
	put32(p + 12, NULLAGBLOCK);
	if (crc) {
		put64(p + 16, (base + block * BS) / 512);
		memcpy(p + 32, &reader.sb.sb_meta_uuid, 16);
		put32(p + 48, agbase >> 15);
	}
	return crc ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN;
}

static void
extent(void *p, uint64_t off, uint64_t fsb, unsigned int length, bool unwritten)
{
	put64(p, ((uint64_t)unwritten << 63) | (off << 9) | (fsb >> 43));
	put64((uint8_t *)p + 8, (fsb << 21) | length);
}

static void
inode_crc(void)
{
	if (crc)
		metadata_checksum(disk + 16 * BS, 512, XFS_DINODE_CRC_OFF);
}

static void
setup(bool v5, unsigned int agno, bool bmap)
{
	struct xfs_io io = { disk, IMAGE_BYTES, read_image };
	struct xfs_agf agf = { 0 };
	struct xfs_agi agi = { 0 };
	struct xfs_agfl agfl = { 0 };
	struct xfs_dinode d = { 0 };
	uint8_t *p;
	unsigned int i, j, h, core;
	const char *why;

	crc = v5;
	calls = fail_call = short_call = 0;
	base = (uint64_t)agno * 20000 * BS;
	agbase = (uint64_t)agno << 15;
	number = (agbase << 3) + 128;
	memset(disk, 0, sizeof(disk));
	fixture(&primary, v5);
	CHECK(xfs_reader_open(&io, scratch + 1, BS, &reader, &why) == 0);
	agf.agf_magicnum = cpu_to_be32(XFS_AGF_MAGIC);
	agf.agf_versionnum = cpu_to_be32(1);
	agf.agf_seqno = cpu_to_be32(agno);
	agf.agf_length = cpu_to_be32(20000);
	agf.agf_bno_root = cpu_to_be32(1);
	agf.agf_cnt_root = cpu_to_be32(2);
	agf.agf_bno_level = agf.agf_cnt_level = cpu_to_be32(1);
	agf.agf_fllast = cpu_to_be32((512 - (v5 ? 36 : 0)) / 4 - 1);
	memcpy(&agf.agf_uuid, &reader.sb.sb_meta_uuid, 16);
	memcpy(disk + 512, &agf, sizeof(agf));
	agi.agi_magicnum = cpu_to_be32(XFS_AGI_MAGIC);
	agi.agi_versionnum = cpu_to_be32(1);
	agi.agi_seqno = cpu_to_be32(agno);
	agi.agi_length = cpu_to_be32(20000);
	agi.agi_root = cpu_to_be32(3);
	agi.agi_level = cpu_to_be32(1);
	agi.agi_count = cpu_to_be32(64);
	agi.agi_freecount = cpu_to_be32(63);
	for (i = 0; i < XFS_AGI_UNLINKED_BUCKETS; i++)
		agi.agi_unlinked[i] = cpu_to_be32(NULLAGINO);
	memcpy(&agi.agi_uuid, &reader.sb.sb_meta_uuid, 16);
	memcpy(disk + 1024, &agi, sizeof(agi));
	if (v5) {
		agfl.agfl_magicnum = cpu_to_be32(XFS_AGFL_MAGIC);
		agfl.agfl_seqno = cpu_to_be32(agno);
		memcpy(&agfl.agfl_uuid, &reader.sb.sb_meta_uuid, 16);
		memcpy(disk + 1536, &agfl, sizeof(agfl));
		memset(disk + 1536 + sizeof(agfl), 0xff, 512 - sizeof(agfl));
		metadata_checksum(disk + 512, 512, XFS_AGF_CRC_OFF);
		metadata_checksum(disk + 1024, 512, XFS_AGI_CRC_OFF);
		metadata_checksum(disk + 1536, 512, XFS_AGFL_CRC_OFF);
	}
	h = tree(3, false, 0, 1);
	put32(disk + 3 * BS + h, 128);
	put32(disk + 3 * BS + h + 4, 63);
	put64(disk + 3 * BS + h + 8, UINT64_MAX - 1);
	tree_crc(3, false);
	d.di_magic = cpu_to_be16(XFS_DINODE_MAGIC);
	d.di_version = v5 ? 3 : 2;
	d.di_mode = cpu_to_be16(S_IFREG | 0644);
	d.di_nlink = cpu_to_be32(1);
	d.di_format = bmap ? XFS_DINODE_FMT_BTREE : XFS_DINODE_FMT_EXTENTS;
	d.di_aformat = XFS_DINODE_FMT_EXTENTS;
	d.di_size = cpu_to_be64((bmap ? 43 : 6) * BS - 17);
	d.di_nextents = cpu_to_be32(bmap ? 30 : 3);
	d.di_nblocks = cpu_to_be64(bmap ? 34 : 4);
	d.di_ino = cpu_to_be64(number);
	memcpy(&d.di_uuid, &reader.sb.sb_meta_uuid, 16);
	core = xfs_dinode_size(d.di_version);
	memcpy(disk + 16 * BS, &d, core);
	p = disk + 16 * BS + core;
	if (!bmap) {
		extent(p, 0, agbase | 32, 2, false);
		extent(p + 16, 3, agbase | 34, 1, true);
		extent(p + 32, 5, agbase | 35, 1, false);
		for (i = 32; i <= 35; i++)
			memset(disk + i * BS, i, BS);
	} else {
		put16(p, 2);
		put16(p + 2, 2);
		put64(p + 4, 0);
		put64(p + 12, 30);
		put64(p + 4 + ((512 - core - 4) / 16) * 8, agbase | 40);
		put64(p + 12 + ((512 - core - 4) / 16) * 8, agbase | 41);
		for (i = 0; i < 2; i++) {
			h = tree(40 + i, true, 1, 1);
			put64(disk + (40 + i) * BS + (i ? 8 : 16), agbase | (i ? 40 : 41));
			put64(disk + (40 + i) * BS + h, i * 30);
			put64(disk + (40 + i) * BS + h + ((BS - h) / 16) * 8,
			    agbase | (42 + i));
			tree_crc(40 + i, true);
			h = tree(42 + i, true, 0, 15);
			put64(disk + (42 + i) * BS + (i ? 8 : 16), agbase | (i ? 42 : 43));
			for (j = 0; j < 15; j++) {
				extent(disk + (42 + i) * BS + h + j * 16,
				    (i * 15 + j) * 2, agbase | (64 + i * 15 + j), 1, false);
				memset(disk + (64 + i * 15 + j) * BS, 64 + i * 15 + j, BS);
			}
			tree_crc(42 + i, true);
		}
	}
	inode_crc();
	calls = 0;
}

static void
expect_inode(uint64_t ino, int expected, const char *message)
{
	struct xfs_inode_image before;
	const char *why;
	int error;

	memset(&inode, 0xa5, sizeof(inode));
	memcpy(&before, &inode, sizeof(inode));
	error = xfs_read_inode(&reader, ino, scratch + 1, BS, &inode, &why);
	if (error != expected)
		fprintf(stderr, "inode: expected %d, got %d (%s)\n", expected, error, why);
	CHECK(error == expected);
	CHECK((why == NULL) == (error == 0));
	if (error) {
		CHECK(memcmp(&inode, &before, sizeof(inode)) == 0);
		if (message)
			CHECK(strstr(why, message) != NULL);
	}
	checks++;
}

static void
expect_read(uint64_t offset, size_t length, int expected, size_t expected_done)
{
	const char *why;
	size_t done = 123;
	int error;

	memset(output, 0xa5, sizeof(output));
	error = xfs_read_file(&reader, &inode, offset, output + 1, length,
	    &done, scratch + 1, BS, &why);
	if (error != expected)
		fprintf(stderr, "file: expected %d, got %d (%s)\n", expected, error, why);
	CHECK(error == expected && done == expected_done);
	CHECK((why == NULL) == (error == 0));
	CHECK(output[0] == 0xa5 && output[length + 1] == 0xa5);
	checks++;
}

int
main(void)
{
	unsigned int v, i, j, h;
	uint8_t *p;
	const char *why;
	size_t done;

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
