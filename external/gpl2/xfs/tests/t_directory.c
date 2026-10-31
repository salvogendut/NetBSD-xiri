/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "inode_fixture.h"
#include "xfs_directory.h"
#include "xfs_dir_layout.h"
#include "../netbsd/xfs_read_internal.h"

static uint8_t work[XFS_DIR_WORKSPACE_SIZE + 1];
static unsigned int entries;
static int callback_error;
static bool hash_collision;
static struct xfs_path_workspace path_work;

static int
collect(void *cookie, uint64_t ino, uint8_t type, const uint8_t *name, size_t length)
{
	CHECK(cookie == &entries);
	CHECK(ino != 0 && type < 9 && length != 0 && memchr(name, 0, length) == NULL);
	entries++;
	return callback_error;
}

static void
expect_directory(int expected, unsigned int count)
{
	const char *why;
	int error;
	entries = 0;
	error = xfs_read_directory(&reader, &inode, work + 1, sizeof(work) - 1,
	    collect, &entries, &why);
	if (error != expected)
		fprintf(stderr, "directory: expected %d, got %d (%s)\n", expected, error, why);
	CHECK(error == expected && entries == count);
	CHECK((why == NULL) == (error == 0));
	checks++;
}

static void
expect_lookup(const char *name, int expected, uint64_t want)
{
	uint64_t result = UINT64_MAX;
	const char *why;
	int error = xfs_directory_lookup(&reader, &inode, name, strlen(name),
	    work + 1, sizeof(work) - 1, &result, &why);
	if (error != expected)
		fprintf(stderr, "lookup: expected %d, got %d (%s)\n", expected, error, why);
	CHECK(error == expected);
	CHECK(result == (expected ? UINT64_MAX : want));
	checks++;
}

static void
make_inode(unsigned int mode, unsigned int format, uint64_t size, unsigned int blocks)
{
	uint8_t *p = disk + 16 * BS;
	unsigned int core = crc ? 176 : 100;
	memset(p + core, 0, 512 - core);
	put16(p + offsetof(struct xfs_dinode, di_mode), mode | 0755);
	p[offsetof(struct xfs_dinode, di_format)] = format;
	put64(p + offsetof(struct xfs_dinode, di_size), size);
	put64(p + offsetof(struct xfs_dinode, di_nblocks), blocks);
	put32(p + offsetof(struct xfs_dinode, di_nextents), blocks ? 1 : 0);
	if (blocks)
		extent(p + core, 0, 32, blocks, false);
}

static unsigned int
entsize(unsigned int n)
{
	return (9 + n + crc + 2 + 7) & ~7U;
}

static uint32_t
hashname(const char *name)
{
	uint32_t h = 0;
	for (; *name; name++) h = ((h << 7) | (h >> 25)) ^ (uint8_t)*name;
	return h;
}

static void
make_short(bool v5, bool wide)
{
	unsigned int core, pos, i, width = wide ? 8 : 4, off;
	uint8_t *p;
	const char *names[] = { "alpha", "beta" };
	setup(v5, 0, false);
	if (wide) {
		reader.sb.sb_agcount = 16385;
		reader.sb.sb_dblocks = UINT64_C(16385) * 20000;
		reader.io.size = reader.sb.sb_dblocks * BS;
	}
	core = v5 ? 176 : 100;
	make_inode(S_IFDIR, XFS_DINODE_FMT_LOCAL, 6, 0);
	p = disk + 16 * BS + core;
	p[0] = 2;
	p[1] = wide ? 1 : 0;
	if (wide) put64(p + 2, number); else put32(p + 2, number);
	pos = 2 + width;
	off = (v5 ? 64 : 16) + entsize(1) + entsize(2);
	for (i = 0; i < 2; i++) {
		unsigned int len = strlen(names[i]);
		uint64_t ino = number + 1 + i + (wide && i == 0 ? UINT64_C(1) << 32 : 0);
		p[pos] = len;
		put16(p + pos + 1, off);
		memcpy(p + pos + 3, names[i], len);
		if (v5) p[pos + 3 + len] = 1;
		if (wide) put64(p + pos + 3 + len + v5, ino);
		else put32(p + pos + 3 + len + v5, ino);
		pos += 3 + len + v5 + width;
		off += entsize(len);
	}
	put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_size), pos);
	inode_crc();
	expect_inode(number, 0, NULL);
}

static void
dir_crc(unsigned int block)
{
	if (crc) metadata_checksum(disk + block * BS, BS, XFS_DIR3_DATA_CRC_OFF);
}

static void
make_data(unsigned int block, bool combined, bool first)
{
	const char *names[] = { ".", "..", hash_collision ? "aaaaa" : "alpha",
	    hash_collision ? "qaaa`" : "beta" };
	uint32_t hashes[4], offsets[4];
	uint8_t *p = disk + block * BS;
	unsigned int n = first ? 4 : 1, h = crc ? 64 : 16, best = crc ? 48 : 4;
	unsigned int i, j, pos = h, end = BS - (combined ? n * 8 + 8 : 0), len;
	memset(p, 0, BS);
	put32(p, combined ? (crc ? XFS_DIR3_BLOCK_MAGIC : XFS_DIR2_BLOCK_MAGIC) :
	    (crc ? XFS_DIR3_DATA_MAGIC : XFS_DIR2_DATA_MAGIC));
	if (crc) {
		put64(p + 8, block * BS / 512);
		memcpy(p + 24, &reader.sb.sb_meta_uuid, 16);
		put64(p + 40, number);
	}
	for (i = 0; i < n; i++) {
		const char *name = first ? names[i] : "gamma";
		unsigned int nl = strlen(name);
		len = entsize(nl);
		put64(p + pos, first && i < 2 ? number : number + 1);
		p[pos + 8] = nl;
		memcpy(p + pos + 9, name, nl);
		if (crc) p[pos + 9 + nl] = first && i < 2 ? 2 : 1;
		put16(p + pos + len - 2, pos);
		hashes[i] = hashname(name);
		offsets[i] = pos / 8;
		pos += len;
	}
	put16(p + pos, XFS_DIR2_DATA_FREE_TAG);
	put16(p + pos + 2, end - pos);
	put16(p + end - 2, pos);
	put16(p + best, pos);
	put16(p + best + 2, end - pos);
	if (combined) {
		for (i = 0; i < n; i++)
			for (j = i + 1; j < n; j++)
				if (hashes[i] > hashes[j]) {
					uint32_t t = hashes[i]; hashes[i] = hashes[j]; hashes[j] = t;
					t = offsets[i]; offsets[i] = offsets[j]; offsets[j] = t;
				}
		for (i = 0; i < n; i++) {
			put32(p + end + i * 8, hashes[i]);
			put32(p + end + i * 8 + 4, offsets[i]);
		}
		put32(p + BS - 8, n);
	}
	dir_crc(block);
}

static void
make_leaf(bool hole)
{
	uint8_t *p = disk + 34 * BS;
	unsigned int h = crc ? 64 : 16, info = crc ? 56 : 12, i, j;
	unsigned int pos = h, n = 5, bestcount = hole ? 3 : 2;
	uint32_t hashes[5], offsets[5];
	memset(p, 0, BS);
	put16(p + 8, crc ? XFS_DIR3_LEAF1_MAGIC : XFS_DIR2_LEAF1_MAGIC);
	if (crc) {
		put64(p + 16, 34 * BS / 512);
		memcpy(p + 32, &reader.sb.sb_meta_uuid, 16);
		put64(p + 48, number);
	}
	put16(p + info, n);
	for (i = 0; i < n; i++) {
		const uint8_t *entry = disk + (i == 4 ? 33 * BS + h : 32 * BS + pos);
		char name[256];
		memcpy(name, entry + 9, entry[8]);
		name[entry[8]] = 0;
		hashes[i] = hashname(name);
		offsets[i] = (i == 4 ? bestcount * BS - BS + h : pos) / 8;
		pos += entsize(entry[8]);
	}
	for (i = 0; i < n; i++)
		for (j = i + 1; j < n; j++)
			if (hashes[j] < hashes[i]) {
				uint32_t tmp = hashes[i]; hashes[i] = hashes[j]; hashes[j] = tmp;
				tmp = offsets[i]; offsets[i] = offsets[j]; offsets[j] = tmp;
			}
	for (i = 0; i < n; i++) {
		put32(p + h + i * 8, hashes[i]);
		put32(p + h + i * 8 + 4, offsets[i]);
	}
	put32(p + BS - 4, bestcount);
	memcpy(p + BS - 4 - bestcount * 2, disk + 32 * BS + (crc ? 50 : 6), 2);
	if (hole) put16(p + BS - 8, UINT16_MAX);
	memcpy(p + BS - 6, disk + 33 * BS + (crc ? 50 : 6), 2);
	if (crc) metadata_checksum(p, BS, 12);
}

static void
make_directory(bool v5, bool combined)
{
	setup(v5, 0, false);
	make_inode(S_IFDIR, XFS_DINODE_FMT_EXTENTS, combined ? BS : 2 * BS, combined ? 1 : 2);
	make_data(32, combined, true);
	if (!combined) {
		uint8_t *p = disk + 16 * BS;
		make_data(33, false, false);
		make_leaf(false);
		put32(p + offsetof(struct xfs_dinode, di_nextents), 2);
		put64(p + offsetof(struct xfs_dinode, di_nblocks), 3);
		extent(p + (v5 ? 176 : 100) + 16, XFS_DIR2_SPACE_SIZE / BS, 34, 1, false);
	}
	inode_crc();
	expect_inode(number, 0, NULL);
}

static void
index_crc(unsigned int block, bool freeblock)
{
	if (crc) metadata_checksum(disk + block * BS, BS, freeblock ? 4 : 12);
}

static void
index_header(unsigned int block, unsigned int magic, unsigned int count,
    unsigned int level, unsigned int forward, unsigned int back)
{
	uint8_t *p = disk + block * BS;
	memset(p, 0, BS);
	put32(p, forward); put32(p + 4, back); put16(p + 8, magic);
	if (crc) {
		put64(p + 16, block * BS / 512);
		memcpy(p + 32, &reader.sb.sb_meta_uuid, 16);
		put64(p + 48, number);
	}
	put16(p + (crc ? 56 : 12), count);
	put16(p + (crc ? 58 : 14), level);
}

static void
make_node(bool v5, bool deep)
{
	uint8_t saved[40], *p;
	unsigned int h = v5 ? 64 : 16, leafbase = XFS_DIR2_SPACE_SIZE / BS;
	unsigned int left = deep ? 37 : 35, right = left + 1, freeblock = right + 1;
	unsigned int split = 2, i;
	make_directory(v5, false);
	memcpy(saved, disk + 34 * BS + h, sizeof(saved));
	/* Put equal hashes on opposite sides of the leaf boundary. */
	for (i = 1; i < 5; i++)
		if (xfs_get32(saved + i * 8) == xfs_get32(saved + (i - 1) * 8)) split = i;
	index_header(left, v5 ? XFS_DIR3_LEAFN_MAGIC : XFS_DIR2_LEAFN_MAGIC,
	    split, 0, leafbase + right - 34, 0);
	index_header(right, v5 ? XFS_DIR3_LEAFN_MAGIC : XFS_DIR2_LEAFN_MAGIC,
	    5 - split, 0, 0, leafbase + left - 34);
	memcpy(disk + left * BS + h, saved, split * 8);
	memcpy(disk + right * BS + h, saved + split * 8, (5 - split) * 8);
	index_crc(left, false); index_crc(right, false);
	index_header(34, v5 ? XFS_DA3_NODE_MAGIC : XFS_DA_NODE_MAGIC, 2, deep ? 2 : 1, 0, 0);
	p = disk + 34 * BS + h;
	put32(p, xfs_get32(saved + (split - 1) * 8)); put32(p + 4, leafbase + 1);
	put32(p + 8, xfs_get32(saved + 32)); put32(p + 12, leafbase + 2);
	index_crc(34, false);
	if (deep) {
		for (i = 0; i < 2; i++) {
			index_header(35 + i, v5 ? XFS_DA3_NODE_MAGIC : XFS_DA_NODE_MAGIC,
			    1, 1, i == 0 ? leafbase + 2 : 0, i == 1 ? leafbase + 1 : 0);
			p = disk + (35 + i) * BS + h;
			put32(p, xfs_get32(saved + (i == 0 ? split - 1 : 4) * 8));
			put32(p + 4, leafbase + 3 + i);
			index_crc(35 + i, false);
		}
	}
	p = disk + freeblock * BS;
	memset(p, 0, BS);
	put32(p, v5 ? XFS_DIR3_FREE_MAGIC : XFS_DIR2_FREE_MAGIC);
	if (v5) {
		put64(p + 8, freeblock * BS / 512);
		memcpy(p + 24, &reader.sb.sb_meta_uuid, 16);
		put64(p + 40, number);
	}
	put32(p + (v5 ? 52 : 8), 2); put32(p + (v5 ? 56 : 12), 2);
	memcpy(p + h, disk + 32 * BS + (v5 ? 50 : 6), 2);
	memcpy(p + h + 2, disk + 33 * BS + (v5 ? 50 : 6), 2);
	index_crc(freeblock, true);
	p = disk + 16 * BS;
	put32(p + offsetof(struct xfs_dinode, di_nextents), 3);
	put64(p + offsetof(struct xfs_dinode, di_nblocks), freeblock - 31);
	extent(p + (v5 ? 176 : 100) + 16, leafbase, 34, freeblock - 34, false);
	extent(p + (v5 ? 176 : 100) + 32, leafbase * 2, freeblock, 1, false);
	inode_crc(); expect_inode(number, 0, NULL);
}

static void
fragmented_index(bool v5)
{
	uint8_t saved[3 * BS], *p;
	unsigned int h = v5 ? 64 : 16, bestoff = v5 ? 48 : 4, i, offset, n;
	unsigned int core = v5 ? 176 : 100;
	make_directory(v5, false);
	memcpy(saved, disk + 32 * BS, sizeof(saved));
	reader.sb.sb_dirblklog = 1;
	/* Two 8 KiB data blocks and one 8 KiB leaf; both a data block and the
	 * index cross noncontiguous filesystem-block extents. */
	for (i = 0; i < 2; i++) {
		p = disk + (32 + i * 2) * BS;
		memcpy(p, saved + i * BS, BS);
		memset(p + BS, 0, BS);
		offset = xfs_get16(p + bestoff);
		put16(p + bestoff + 2, 2 * BS - offset);
		put16(p + offset + 2, 2 * BS - offset);
		put16(p + 2 * BS - 2, offset);
		if (v5) {
			put64(p + 8, (32 + i * 2) * BS / 512);
			metadata_checksum(p, 2 * BS, 4);
		}
	}
	p = disk + 36 * BS;
	memcpy(p, saved + 2 * BS, BS);
	memset(p + BS, 0, BS);
	for (i = 0; i < 5; i++) {
		n = xfs_get32(p + h + i * 8 + 4);
		if (n >= BS / 8) put32(p + h + i * 8 + 4, n + BS / 8);
	}
	put32(p + 2 * BS - 4, 2);
	memcpy(p + 2 * BS - 8, disk + 32 * BS + bestoff + 2, 2);
	memcpy(p + 2 * BS - 6, disk + 34 * BS + bestoff + 2, 2);
	if (v5) {
		put64(p + 16, 36 * BS / 512);
		metadata_checksum(p, 2 * BS, 12);
	}
	memcpy(disk + 40 * BS, disk + 37 * BS, BS);
	memcpy(disk + 42 * BS, disk + 33 * BS, BS);
	p = disk + 16 * BS;
	put64(p + offsetof(struct xfs_dinode, di_size), 4 * BS);
	put64(p + offsetof(struct xfs_dinode, di_nblocks), 6);
	put32(p + offsetof(struct xfs_dinode, di_nextents), 5);
	extent(p + core, 0, 32, 1, false);
	extent(p + core + 16, 1, 42, 1, false);
	extent(p + core + 32, 2, 34, 2, false);
	extent(p + core + 48, XFS_DIR2_SPACE_SIZE / BS, 36, 1, false);
	extent(p + core + 64, XFS_DIR2_SPACE_SIZE / BS + 1, 40, 1, false);
	inode_crc(); expect_inode(number, 0, NULL);
	expect_directory(0, 5); expect_lookup("gamma", 0, number + 1);
	/* Removing half of the leaf must fail, even when its header is intact. */
	put32(p + offsetof(struct xfs_dinode, di_nextents), 4);
	memset(p + core + 64, 0, 16);
	inode_crc(); expect_inode(number, 0, NULL);
	expect_directory(EINVAL, 0); expect_lookup("alpha", EINVAL, 0);
}

static void
index_tests(void)
{
	unsigned int v, i, j, h, info, reads;
	uint8_t *p;
	const char *why;
	for (v = 0; v < 2; v++) {
		fragmented_index(v);
		for (j = 0; j < 3; j++) {
			hash_collision = true;
			if (j == 0) make_directory(v, false); else make_node(v, j == 2);
			expect_directory(0, 5);
			expect_lookup("aaaaa", 0, number + 1);
			expect_lookup("qaaa`", 0, number + 1);
			expect_lookup("gamma", 0, number + 1);
			expect_lookup("absent", ENOENT, 0);
			hash_collision = false;
		}
		h = v ? 64 : 16; info = v ? 56 : 12;
		/* A valid stale entry retains its sorted hash but no data pointer. */
		make_directory(v, false);
		p = disk + 34 * BS;
		memmove(p + h + 8, p + h, 40);
		put32(p + h + 4, 0); put16(p + info, 6); put16(p + info + 2, 1);
		index_crc(34, false);
		expect_directory(0, 5); expect_lookup(".", 0, number);
		for (i = 0; i < 14; i++) {
			make_directory(v, false);
			p = disk + 34 * BS;
			if (i == 0) put16(p + 8, 0);
			if (i == 1) put16(p + info, UINT16_MAX);
			if (i == 2) put16(p + info + 2, 1);
			if (i == 3) put32(p + BS - 4, UINT32_MAX);
			if (i == 4) put32(p + BS - 4, 1);
			if (i == 5) put16(p + BS - 6, UINT16_MAX);
			if (i == 6) put32(p, XFS_DIR2_SPACE_SIZE / BS);
			if (i == 7) put32(p + 4, XFS_DIR2_SPACE_SIZE / BS);
			if (i == 8) put32(p + h + 4, h / 8 + 1); /* inside a name */
			if (i == 9) put32(p + h + 12, xfs_get32(p + h + 4));
			if (i == 10) put32(p + h, UINT32_MAX);
			if (i == 11) put32(p + h + 4, UINT32_MAX);
			if (i == 12) put16(p + info, 4); /* omitted live entry */
			if (i == 13) put32(p + h + 4, (h + 4 * entsize(5)) / 8); /* free record */
			index_crc(34, false);
			expect_directory(EINVAL, 0); expect_lookup("gamma", EINVAL, 0);
		}
		for (i = 0; i < 18; i++) {
			make_node(v, false);
			p = disk + 34 * BS;
			if (i == 0) put16(p + info + 2, 0);
			if (i == 1) put16(p + info + 2, XFS_DA_NODE_MAXDEPTH + 1);
			if (i == 2) put16(p + info, 0);
			if (i == 3) put32(p + h + 4, XFS_DIR2_SPACE_SIZE / BS); /* cycle */
			if (i == 4) put32(p + h + 12, xfs_get32(p + h + 4)); /* shared leaf */
			if (i == 5) put32(p + h + 4, 0);
			if (i == 6) put32(p + h, xfs_get32(p + h) + 1); /* parent maximum */
			if (i == 7) put32(disk + 35 * BS, 0);
			if (i == 8) put32(disk + 36 * BS + 4, 0);
			if (i == 9) put32(disk + 36 * BS, XFS_DIR2_SPACE_SIZE / BS + 1);
			if (i == 10) put16(disk + 35 * BS + info, 0);
			if (i == 11) put32(disk + 37 * BS, 0);
			if (i == 12) put32(disk + 37 * BS + (v ? 48 : 4), 1);
			if (i == 13) put32(disk + 37 * BS + (v ? 52 : 8), UINT32_MAX);
			if (i == 14) put32(disk + 37 * BS + (v ? 56 : 12), 1);
			if (i == 15) put16(disk + 37 * BS + h, UINT16_MAX);
			if (i == 16) put32(disk + 37 * BS + (v ? 52 : 8), 1);
			if (i == 17) put32(p + h + 4, XFS_DIR2_SPACE_SIZE / BS + 7);
			for (j = 34; j <= 37; j++) index_crc(j, j == 37);
			expect_directory(EINVAL, 0); expect_lookup("alpha", EINVAL, 0);
		}
		make_node(v, true);
		calls = 0; expect_directory(0, 5); reads = calls;
		for (i = 1; i <= reads; i++) {
			calls = 0; fail_call = i;
			expect_directory(EACCES, 0);
			calls = 0; fail_call = 0; short_call = i;
			expect_lookup("alpha", EIO, 0);
			short_call = 0;
		}
		entries = 0;
		CHECK(xfs_read_dir_index(&reader, &inode, work + 1, 4 * BS,
		    collect, &entries, NULL, 0, &why) == EMSGSIZE && entries == 0);
		/* Three extents in an external bmap leaf, with a small inode root. */
		make_directory(v, false);
		p = disk + 16 * BS;
		memset(p + (v ? 176 : 100), 0, 512 - (v ? 176 : 100));
		p[offsetof(struct xfs_dinode, di_format)] = XFS_DINODE_FMT_BTREE;
		p[offsetof(struct xfs_dinode, di_forkoff)] = 4;
		put32(p + offsetof(struct xfs_dinode, di_nextents), 3);
		put64(p + offsetof(struct xfs_dinode, di_nblocks), 4);
		p += v ? 176 : 100;
		put16(p, 1); put16(p + 2, 1); put64(p + 4, 0); put64(p + 12, 50);
		j = tree(50, true, 0, 3);
		extent(disk + 50 * BS + j, 0, 32, 1, false);
		extent(disk + 50 * BS + j + 16, 1, 33, 1, false);
		extent(disk + 50 * BS + j + 32, XFS_DIR2_SPACE_SIZE / BS, 34, 1, false);
		tree_crc(50, true); inode_crc(); expect_inode(number, 0, NULL);
		expect_directory(0, 5); expect_lookup("gamma", 0, number + 1);
		/* A missing root and an orphan index block may not be ignored. */
		for (i = 0; i < 2; i++) {
			make_directory(v, false);
			p = disk + 16 * BS;
			put32(p + offsetof(struct xfs_dinode, di_nextents), i == 0 ? 1 : 2);
			if (i == 0) memset(p + (v ? 176 : 100) + 16, 0, 16);
			else {
				extent(p + (v ? 176 : 100) + 16, XFS_DIR2_SPACE_SIZE / BS, 34, 2, false);
				put64(p + offsetof(struct xfs_dinode, di_nblocks), 4);
			}
			inode_crc(); expect_inode(number, 0, NULL);
			expect_directory(EINVAL, 0); expect_lookup("alpha", EINVAL, 0);
		}
		/* Duplicate names with otherwise consistent index/data are rejected. */
		make_directory(v, false);
		memcpy(disk + 33 * BS + h + 9, "alpha", 5);
		dir_crc(33); make_leaf(false);
		expect_directory(0, 5); expect_lookup("alpha", EINVAL, 0);
	}
	/* CRC coverage includes unrequested leaves, internal nodes and free blocks. */
	make_node(true, true);
	for (j = 34; j <= 39; j++)
		for (i = 0; i < BS; i++) {
			disk[j * BS + i] ^= 1;
			expect_lookup(".", EINVAL, 0);
			disk[j * BS + i] ^= 1;
		}
	for (j = 34; j <= 39; j++) {
		for (i = 0; i < 3; i++) {
			unsigned int offset = (i == 0 ? 16 : i == 1 ? 32 : 48) - (j == 39 ? 8 : 0);
			make_node(true, true);
			disk[j * BS + offset] ^= 1;
			index_crc(j, j == 39);
			expect_lookup(".", EINVAL, 0);
		}
	}
}

static void
make_symlink(bool v5)
{
	uint8_t *p;
	setup(v5, 0, false);
	make_inode(S_IFLNK, XFS_DINODE_FMT_EXTENTS, 1000, 1);
	p = disk + 32 * BS;
	memset(p, 0, BS);
	if (v5) {
		put32(p, XFS_SYMLINK_MAGIC);
		put32(p + 8, 1000);
		memcpy(p + 16, &reader.sb.sb_meta_uuid, 16);
		put64(p + 32, number);
		put64(p + 40, 32 * BS / 512);
	}
	memset(p + (v5 ? 56 : 0), 'a', 1000);
	if (v5) metadata_checksum(p, BS, XFS_SYMLINK_CRC_OFF);
	inode_crc();
	expect_inode(number, 0, NULL);
}

static void
make_smallblock_symlink(bool fragmented)
{
	struct xfs_io io;
	uint8_t *p;
	unsigned int i;
	const char *why;
	setup(true, 0, false);
	primary.sb.sb_blocksize = cpu_to_be32(1024);
	primary.sb.sb_blocklog = 10;
	primary.sb.sb_inopblock = cpu_to_be16(2);
	primary.sb.sb_inopblog = 1;
	primary.sb.sb_agblocks = cpu_to_be32(80000);
	primary.sb.sb_agblklog = 17;
	primary.sb.sb_dblocks = cpu_to_be64(320000);
	checksum(&primary);
	io = reader.io;
	CHECK(xfs_reader_open(&io, scratch, sizeof(scratch), &reader, &why) == 0);
	put32(disk + 512 + 12, 80000);
	put32(disk + 512 + 16, 2);
	put32(disk + 512 + 20, 3);
	metadata_checksum(disk + 512, 512, XFS_AGF_CRC_OFF);
	put32(disk + 1024 + 12, 80000);
	put32(disk + 1024 + 20, 12);
	metadata_checksum(disk + 1024, 512, XFS_AGI_CRC_OFF);
	metadata_checksum(disk + 3 * BS, 1024, XFS_BTREE_SBLOCK_CRC_OFF);
	make_inode(S_IFLNK, XFS_DINODE_FMT_EXTENTS, 1000, 2);
	p = disk + 16 * BS;
	put32(p + offsetof(struct xfs_dinode, di_nextents), fragmented ? 2 : 1);
	extent(p + 176, 0, 128, fragmented ? 1 : 2, false);
	if (fragmented) extent(p + 192, 1, 132, 1, false);
	for (i = 0; i < (fragmented ? 2U : 1U); i++) {
		p = disk + (128 + i * 4) * 1024;
		unsigned int bytes = fragmented ? 1024 : 2048;
		unsigned int length = fragmented ? (i ? 32 : 968) : 1000;
		memset(p, 0, bytes);
		put32(p, XFS_SYMLINK_MAGIC);
		put32(p + 4, i ? 968 : 0);
		put32(p + 8, length);
		memcpy(p + 16, &reader.sb.sb_meta_uuid, 16);
		put64(p + 32, number);
		put64(p + 40, (128 + i * 4) * 2);
		memset(p + 56, 'a', length);
		metadata_checksum(p, bytes, XFS_SYMLINK_CRC_OFF);
	}
	inode_crc();
	expect_inode(number, 0, NULL);
}

int
main(void)
{
	unsigned int v, i, core, h;
	uint8_t *p;
	uint64_t result;
	const char *why;
	int error;

	for (v = 0; v < 2; v++) {
		make_short(v, false);
		expect_directory(0, 4);
		expect_lookup(".", 0, number);
		expect_lookup("..", 0, number);
		expect_lookup("alpha", 0, number + 1);
		expect_lookup("missing", ENOENT, 0);
		expect_lookup("", EINVAL, 0);
		expect_lookup("a/b", EINVAL, 0);
		make_short(v, true);
		expect_directory(0, 4);
		expect_lookup("alpha", 0, (UINT64_C(1) << 32) + number + 1);
		make_directory(v, true);
		expect_directory(0, 4);
		expect_lookup("alpha", 0, number + 1);
		make_directory(v, false);
		expect_directory(0, 5);
		expect_lookup("gamma", 0, number + 1);
		/* A whole directory hole is skipped; a partial one is corrupt. */
		core = v ? 176 : 100;
		p = disk + 16 * BS;
		put32(p + offsetof(struct xfs_dinode, di_nextents), 3);
		put64(p + offsetof(struct xfs_dinode, di_size), 3 * BS);
		extent(p + core, 0, 32, 1, false);
		extent(p + core + 16, 2, 33, 1, false);
		extent(p + core + 32, XFS_DIR2_SPACE_SIZE / BS, 34, 1, false);
		make_leaf(true);
		inode_crc(); expect_inode(number, 0, NULL);
		expect_directory(0, 5);
		expect_lookup("gamma", 0, number + 1);
		make_symlink(v);
		expect_read(0, 1200, 0, 1000);
		for (i = 1; i <= 1000; i++) CHECK(output[i] == 'a');
		expect_read(997, 20, 0, 3);
	}
	for (i = 0; i < 9; i++) {
		make_short(true, false);
		p = disk + 16 * BS + 176;
		if (i == 0) p[0] = 3;
		if (i == 1) p[1] = 1;
		if (i == 2) put32(p + 2, 0);
		if (i == 3) p[6] = 255;
		if (i == 4) put16(p + 7, 64);
		if (i == 5) p[9] = 0;
		if (i == 6) p[9] = '/';
		if (i == 7) p[14] = 255;
		if (i == 8) put32(p + 15, 0);
		inode_crc(); expect_inode(number, 0, NULL);
		expect_directory(EINVAL, 0);
		expect_lookup("alpha", EINVAL, 0);
	}
	CHECK(hashname("aaaaa") == hashname("qaaa`"));
	hash_collision = true;
	make_directory(true, true);
	expect_directory(0, 4);
	expect_lookup("aaaaa", 0, number + 1);
	expect_lookup("qaaa`", 0, number + 1);
	hash_collision = false;
	make_directory(true, true);
	for (i = 0; i < BS; i++) {
		disk[32 * BS + i] ^= 1;
		expect_directory(EINVAL, 0);
		disk[32 * BS + i] ^= 1;
	}
	for (i = 0; i < 16; i++) {
		make_directory(true, true);
		p = disk + 32 * BS;
		if (i == 0) put64(p + 8, 0);
		if (i == 1) put64(p + 40, 0);
		if (i == 2) p[24] ^= 1;
		if (i == 3) put32(p + BS - 8, UINT32_MAX);
		if (i == 4) put32(p + BS - 4, 5);
		if (i == 5) put16(p + 48, 0);
		if (i == 6) put16(p + 50, 0);
		if (i == 7) put64(p + 64, 0);
		if (i == 8) p[73] = '/';
		if (i == 9) p[72] = 0;
		if (i == 10) p[74] = 9;
		if (i == 11) put16(p + 78, 0);
		if (i == 12) put32(p + BS - 40 + 4, 9);
		if (i == 13) put32(p + BS - 40, 0);
		if (i == 14) put16(p + 136 + 2, 0);
		if (i == 15) put16(p + BS - 42, 0);
		dir_crc(32);
		expect_directory(EINVAL, 0);
	}
	/* Later corrupt data must not turn a lookup into a false success. */
	make_directory(true, false);
	disk[33 * BS + 500] ^= 1;
	expect_directory(EINVAL, 0);
	expect_lookup("alpha", EINVAL, 0);
	/* A duplicate queried name is rejected across separate data blocks. */
	make_directory(true, false);
	memcpy(disk + 33 * BS + 64 + 9, "alpha", 5);
	dir_crc(33);
	expect_lookup("alpha", EINVAL, 0);
	for (i = 0; i < 3; i++) {
		make_directory(true, true);
		calls = 0;
		if (i == 0) fail_call = 1;
		if (i == 1) short_call = 1;
		if (i == 2) callback_error = EBUSY;
		expect_directory(i == 0 ? EACCES : i == 1 ? EIO : EBUSY, i == 2 ? 1 : 0);
		callback_error = 0;
	}
	make_directory(true, true);
	reader.sb.sb_versionnum |= XFS_SB_VERSION_BORGBIT;
	expect_directory(EOPNOTSUPP, 0);
	make_symlink(true);
	for (i = 0; i < BS; i++) {
		disk[32 * BS + i] ^= 1;
		expect_read(0, 1, EINVAL, 0);
		disk[32 * BS + i] ^= 1;
	}
	for (i = 0; i < 7; i++) {
		make_symlink(true);
		p = disk + 32 * BS;
		if (i == 0) put32(p, 0);
		if (i == 1) put32(p + 4, 1);
		if (i == 2) put32(p + 8, 999);
		if (i == 3) p[16] ^= 1;
		if (i == 4) put64(p + 32, 129);
		if (i == 5) put64(p + 40, 0);
		if (i == 6) p[56 + 999] = 0;
		metadata_checksum(p, BS, XFS_SYMLINK_CRC_OFF);
		expect_read(0, 1, EINVAL, 0);
	}
	make_symlink(true);
	calls = 0; fail_call = 1;
	expect_read(0, 1, EACCES, 0);
	make_symlink(true);
	calls = 0; short_call = 1;
	expect_read(0, 1, EIO, 0);
	for (v = 0; v < 2; v++) {
		make_smallblock_symlink(v);
		expect_read(960, 100, 0, 40);
		for (i = 1; i <= 40; i++) CHECK(output[i] == 'a');
		if (v) {
			disk[132 * 1024 + 56 + 31] ^= 1;
			expect_read(0, 1, EINVAL, 0); /* verify also unrequested tail */
		} else {
			size_t done = 123;
			CHECK(xfs_read_file(&reader, &inode, 0, output, 1, &done,
			    scratch, 1024, &why) == EMSGSIZE && done == 0);
		}
	}
	/* API arguments, callback normalization and output preservation. */
	index_tests();
	make_short(true, false);
	CHECK(xfs_read_directory(&reader, &inode, work, XFS_DIR_WORKSPACE_SIZE - 1, collect, &entries, &why) == EMSGSIZE);
	CHECK(xfs_read_directory(NULL, &inode, work, sizeof(work), collect, &entries, &why) == EINVAL);
	callback_error = -1; expect_directory(EIO, 1); callback_error = 0;
	result = UINT64_MAX;
	error = xfs_lookup_path(&reader, "/alpha", 6, true, &path_work,
	    work, sizeof(work), &result, &why);
	CHECK(error == ENOENT && result == UINT64_MAX); /* inode 129 is free */
	CHECK(xfs_lookup_path(&reader, "relative", 8, true, &path_work,
	    work, sizeof(work), &result, &why) == EINVAL);
	CHECK(xfs_lookup_path(&reader, "/a\0b", 4, true, &path_work,
	    work, sizeof(work), &result, &why) == EINVAL);
	CHECK(xfs_lookup_path(&reader, "/", XFS_IMAGE_PATH_MAX, true, &path_work,
	    work, sizeof(work), &result, &why) == ENAMETOOLONG);
	h = checks;
	printf("Passed %u directory, lookup and remote-symlink checks\n", h);
	return 0;
}
