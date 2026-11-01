/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
/* Shared synthetic image setup for inode and namespace tests. */
#ifndef _XFS_TEST_INODE_FIXTURE_H_
#define _XFS_TEST_INODE_FIXTURE_H_
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


#endif
