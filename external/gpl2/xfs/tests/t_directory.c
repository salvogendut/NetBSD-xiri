/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "inode_fixture.h"
#include "xfs_directory.h"
#include "xfs_dir_layout.h"

static uint8_t work[2 * BS + 1];
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
	error = xfs_read_directory(&reader, &inode, work + 1, 2 * BS,
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
	    work + 1, 2 * BS, &result, &why);
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
make_directory(bool v5, bool combined)
{
	setup(v5, 0, false);
	make_inode(S_IFDIR, XFS_DINODE_FMT_EXTENTS, combined ? BS : 2 * BS, combined ? 1 : 2);
	make_data(32, combined, true);
	if (!combined) make_data(33, false, false);
	inode_crc();
	expect_inode(number, 0, NULL);
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
		put32(p + offsetof(struct xfs_dinode, di_nextents), 2);
		put64(p + offsetof(struct xfs_dinode, di_size), 3 * BS);
		extent(p + core, 0, 32, 1, false);
		extent(p + core + 16, 2, 33, 1, false);
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
	expect_directory(EINVAL, 4);
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
	make_short(true, false);
	CHECK(xfs_read_directory(&reader, &inode, work, 2 * BS - 1, collect, &entries, &why) == EMSGSIZE);
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
