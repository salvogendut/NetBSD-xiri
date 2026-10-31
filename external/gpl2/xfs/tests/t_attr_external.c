/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "inode_fixture.h"
#include "xfs_attr.h"
#include "xfs_attr_layout.h"

static uint8_t work[XFS_ATTR_WORKSPACE_SIZE + 16], saved[sizeof(disk)];
static unsigned int emitted, callback_error, value_length[256], expected_flags;
static uint8_t *forkp;
static unsigned int info, header;

static void
attr_crc(unsigned int block)
{
	if (crc)
		metadata_checksum(disk + block * BS, BS, 12);
}

static void
init_block(unsigned int block, bool node, unsigned int level)
{
	uint8_t *p = disk + block * BS;
	memset(p, 0, BS);
	put16(p + 8, node ? (crc ? XFS_DA3_NODE_MAGIC : XFS_DA_NODE_MAGIC) :
	    (crc ? XFS_ATTR3_LEAF_MAGIC : XFS_ATTR_LEAF_MAGIC));
	if (crc) {
		put64(p + 16, (base + block * BS) / 512);
		memcpy(p + 32, &reader.sb.sb_meta_uuid, 16);
		put64(p + 48, number);
	}
	if (node)
		put16(p + info + 2, level);
	else
		put16(p + info + 4, BS);
}

static void
remote(unsigned int block, unsigned int length, unsigned int name)
{
	unsigned int done = 0, part, i, h = crc ? 56 : 0;
	uint8_t *p;
	while (done < length) {
		p = disk + block * BS;
		memset(p, 0, BS);
		part = length - done < BS - h ? length - done : BS - h;
		if (crc) {
			put32(p, XFS_ATTR3_RMT_MAGIC);
			put32(p + 4, done);
			put32(p + 8, part);
			memcpy(p + 16, &reader.sb.sb_meta_uuid, 16);
			put64(p + 32, number);
			put64(p + 40, (base + block * BS) / 512);
			put64(p + 48, UINT64_MAX);
		}
		for (i = 0; i < part; i++)
			p[h + i] = (uint8_t)(name + done + i);
		attr_crc(block++);
		done += part;
	}
}

static void
add(unsigned int block, uint8_t name, unsigned int length, unsigned int rmt)
{
	uint8_t *p = disk + block * BS, *entry;
	unsigned int first = be16_to_cpu(*(const __be16 *)(p + info + 4));
	unsigned int n = be16_to_cpu(*(const __be16 *)(p + info));
	unsigned int size = rmt ? 12 : ((4 + length + 3) & ~3U), i;
	first -= size;
	entry = p + header + n * 8;
	put32(entry, name); /* one-byte names hash to themselves */
	put16(entry + 4, first);
	entry[6] = rmt ? 0 : XFS_ATTR_LOCAL;
	if (rmt) {
		put32(p + first, rmt);
		put32(p + first + 4, length);
		p[first + 8] = 1;
		p[first + 9] = name;
	} else {
		put16(p + first, length);
		p[first + 2] = 1;
		p[first + 3] = name;
		for (i = 0; i < length; i++)
			p[first + 4 + i] = (uint8_t)(name + i);
	}
	put16(p + info, n + 1);
	put16(p + info + 2, BS - first);
	put16(p + info + 4, first);
	put16(p + info + 8, header + (n + 1) * 8);
	put16(p + info + 10, first - header - (n + 1) * 8);
	value_length[name] = length;
	attr_crc(block);
}

static void
reset(bool v5, unsigned int blocks)
{
	uint8_t *d;
	setup(v5, 1, false); /* exercise nonzero AG physical address encoding */
	d = disk + 16 * BS;
	d[offsetof(struct xfs_dinode, di_forkoff)] = 8;
	forkp = d + xfs_dinode_size(v5 ? 3 : 2) + 64;
	memset(forkp, 0, d + 512 - forkp);
	extent(forkp, 0, agbase | 48, blocks, false);
	put16(d + offsetof(struct xfs_dinode, di_anextents), 1);
	put64(d + offsetof(struct xfs_dinode, di_nblocks), 4 + blocks);
	info = v5 ? 56 : 12;
	header = v5 ? 80 : 32;
	expected_flags = callback_error = 0;
	init_block(48, false, 0);
	attr_crc(48);
}

static int
emit(void *cookie, uint8_t flags, const uint8_t *name, size_t nlen,
    const uint8_t *value, size_t length)
{
	size_t i;
	CHECK(cookie == &emitted && flags == expected_flags && nlen == 1);
	CHECK(length == value_length[*name]);
	for (i = 0; i < length; i++)
		CHECK(value[i] == (uint8_t)(*name + i));
	emitted++;
	return callback_error;
}

static void
check(int expected, unsigned int count, int policy)
{
	const char *why;
	int error;
	inode_crc();
	expect_inode(number, 0, NULL);
	emitted = 0;
	work[0] = work[sizeof(work) - 1] = 0xa5;
	error = xfs_read_attrs(&reader, &inode, work + 1, sizeof(work) - 2,
	    emit, &emitted, &why);
	if (error != expected)
		fprintf(stderr, "external case %u: expected %d, got %d (%s)\n", checks, expected, error, why);
	CHECK(error == expected && emitted == count);
	CHECK((why == NULL) == (error == 0));
	CHECK(work[0] == 0xa5 && work[sizeof(work) - 1] == 0xa5);
	CHECK(xfs_attrs_check_readonly(&reader, &inode, work + 1, sizeof(work) - 2, &why) == policy);
	checks++;
}

static void
node(unsigned int block, unsigned int level, unsigned int child, uint32_t hash)
{
	unsigned int h = crc ? 64 : 16;
	init_block(block, true, level);
	put16(disk + block * BS + info, 1);
	put32(disk + block * BS + h, hash);
	put32(disk + block * BS + h + 4, child);
	attr_crc(block);
}

int
main(void)
{
	unsigned int v, i, b, n, h, reads, size, flags;
	uint8_t *p;
	const char *why;
	(void)expect_read; /* fixture helpers shared with data-fork tests */
	for (v = 0; v <= 1; v++) {
		reset(v, 1);
		check(0, 0, 0); /* empty retained leaf */
		add(48, 'a', 0, 0);
		add(48, 'b', 511, 0);
		check(0, 2, 0);
		memcpy(saved, disk, sizeof(disk));
		put16(disk + 48 * BS + info + 4, 1024);
		put16(disk + 48 * BS + info + 10, 1024 - header - 16);
		disk[48 * BS + info + 6] = 1;
		attr_crc(48); check(0, 2, 0); /* firstused may lag after a split */
		disk[48 * BS + info + 6] = 0;
		attr_crc(48); check(EINVAL, 0, EINVAL);
		/* Every namespace/flag combination, including policy separation. */
		for (flags = 0; flags <= 255; flags++) {
			memcpy(disk, saved, sizeof(disk));
			disk[48 * BS + header + 6] = flags | XFS_ATTR_LOCAL;
			attr_crc(48);
			n = flags & 14;
			if ((flags & ~143U) || (n && (n & (n - 1))))
				check(EINVAL, 0, EINVAL);
			else if (flags & (XFS_ATTR_PARENT | XFS_ATTR_INCOMPLETE))
				check(EOPNOTSUPP, 0, EOPNOTSUPP);
			else if (n == 0)
				check(0, 2, 0);
			else {
				/* Both entries use this namespace for callback assertions. */
				disk[48 * BS + header + 14] = n | XFS_ATTR_LOCAL;
				attr_crc(48);
				expected_flags = n;
				check(0, 2, EOPNOTSUPP);
				expected_flags = 0;
			}
		}
		/* Semantic damage with fresh CRCs; no earlier valid entry escapes. */
		for (i = 0; i < 8; i++) {
			memcpy(disk, saved, sizeof(disk));
			p = disk + 48 * BS;
			switch (i) {
			case 0: put16(p + info + 2, 0); break;
			case 1: put16(p + info + 4, BS); break;
			case 2: put16(p + header + 12, BS - 2); break;
			case 3: put32(p + header + 8, 'a'); break;
			case 4: put16(p + info + 8, 0); break;
			case 5: put32(p, 1); break;
			case 6: put32(p + 4, 1); break;
			case 7: p[BS - 1] = 0; break; /* first entry name */
			}
			attr_crc(48);
			check(EINVAL, 0, EINVAL);
		}
		/* Largest remote value, partial final block and two staging users. */
		n = (65536 + (v ? BS - 56 : BS) - 1) / (v ? BS - 56 : BS);
		reset(v, n + 2);
		add(48, 'a', 65536, 1); remote(49, 65536, 'a');
		add(48, 'b', 7, n + 1); remote(49 + n, 7, 'b');
		check(0, 2, 0);
		callback_error = ECANCELED;
		check(ECANCELED, 1, 0);
		callback_error = 0;
		memcpy(saved, disk, sizeof(disk));
		/* Missing remote block, unwritten extent, unreferenced allocation. */
		extent(forkp, 0, agbase | 48, n + 1, false);
		check(EINVAL, 0, EINVAL);
		memcpy(disk, saved, sizeof(disk));
		extent(forkp, 0, agbase | 48, n + 2, true);
		check(EINVAL, 0, EINVAL);
		memcpy(disk, saved, sizeof(disk));
		extent(forkp, 0, agbase | 48, n + 3, false);
		put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_nblocks), n + 7);
		check(EINVAL, 0, EINVAL);
		if (v) {
			for (i = 0; i < 8; i++) {
				memcpy(disk, saved, sizeof(disk));
				p = disk + (49 + n) * BS;
				p[(unsigned int[]){0, 4, 8, 16, 32, 40, 56, 4095}[i]] ^= 1;
				if (i < 6) attr_crc(49 + n);
				check(EINVAL, 0, EINVAL);
			}
			for (i = 0; i < 4; i++) {
				memcpy(disk, saved, sizeof(disk));
				disk[48 * BS + (unsigned int[]){16, 32, 48, 4095}[i]] ^= 1;
				if (i < 3) attr_crc(48);
				check(EINVAL, 0, EINVAL);
			}
		}
		memcpy(disk, saved, sizeof(disk));
		inode_crc(); expect_inode(number, 0, NULL);
		for (i = 0; i < 32; i++) {
			emitted = 0;
			CHECK(xfs_read_attrs(&reader, &inode, work + (i % 8),
			    BS + 65536 + i - 16, emit, &emitted, &why) == EMSGSIZE);
			CHECK(emitted == 0);
		}
		reads = calls;
		emitted = 0; fail_call = reads + n + 2;
		CHECK(xfs_read_attrs(&reader, &inode, work, sizeof(work), emit, &emitted, &why) == EACCES);
		CHECK(emitted == 0); fail_call = 0;
		short_call = calls + 2;
		CHECK(xfs_read_attrs(&reader, &inode, work, sizeof(work), emit, &emitted, &why) == EIO);
		CHECK(emitted == 0); short_call = 0;
		/* Maximum supported tree depth; decreasing levels bound recursion. */
		reset(v, 6);
		for (i = 0; i < 5; i++) node(48 + i, 5 - i, i + 1, 'a');
		init_block(53, false, 0); add(53, 'a', 10, 0);
		check(0, 1, 0);
		node(50, 3, 0, 'a'); check(EINVAL, 0, EINVAL);
		/* Sibling chain, duplicate names across leaves, parent maxima. */
		reset(v, 3);
		node(48, 1, 1, 'a');
		h = v ? 64 : 16; p = disk + 48 * BS;
		put16(p + info, 2); put32(p + h + 8, 'b'); put32(p + h + 12, 2);
		attr_crc(48);
		init_block(49, false, 0); add(49, 'a', 1, 0);
		init_block(50, false, 0); add(50, 'b', 2, 0);
		put32(disk + 49 * BS, 2); put32(disk + 50 * BS + 4, 1);
		attr_crc(49); attr_crc(50); check(0, 2, 0);
		put32(p + h + 8, 'c'); attr_crc(48); check(EINVAL, 0, EINVAL);
		put32(p + h + 8, 'a'); attr_crc(48);
		init_block(50, false, 0); add(50, 'a', 2, 0);
		put32(disk + 50 * BS + 4, 1); attr_crc(50); check(EINVAL, 0, EINVAL);
		/* Attribute bmap root: 23 sparse extents, 22 tiny remote values. */
		reset(v, 23);
		p = disk + 16 * BS;
		p[offsetof(struct xfs_dinode, di_aformat)] = XFS_DINODE_FMT_BTREE;
		put16(p + offsetof(struct xfs_dinode, di_anextents), 23);
		put64(p + offsetof(struct xfs_dinode, di_nblocks), 28);
		size = p + 512 - forkp;
		memset(forkp, 0, size);
		put16(forkp, 1); put16(forkp + 2, 1);
		put64(forkp + 4 + ((size - 4) / 16) * 8, agbase | 90);
		h = tree(90, true, 0, 23);
		for (i = 0; i < 23; i++) {
			b = 48 + i;
			extent(disk + 90 * BS + h + i * 16, i * 2, agbase | b, 1, false);
			if (i) { add(48, 'a' + i - 1, 7, i * 2); remote(b, 7, 'a' + i - 1); }
		}
		tree_crc(90, true); check(0, 22, 0);
		/* Physical alias across otherwise ordered logical extents. */
		extent(disk + 90 * BS + h + 22 * 16, 44, agbase | 49, 1, false);
		tree_crc(90, true); check(EINVAL, 0, EINVAL);
	}
	printf("Passed %u external attribute checks (v4/v5, trees, bmaps, remote values)\n", checks);
	return 0;
}
