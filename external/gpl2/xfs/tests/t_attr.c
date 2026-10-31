/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "inode_fixture.h"
#include "xfs_attr.h"

static uint8_t *attrs;
static size_t used;
static unsigned int entries, emitted;
static int callback_error;

static void
reset_attrs(bool v5, bool root)
{
	uint8_t *d;
	unsigned int core;
	setup(v5, 0, false);
	d = disk + 16 * BS;
	core = xfs_dinode_size(v5 ? 3 : 2);
	d[offsetof(struct xfs_dinode, di_forkoff)] = 8;
	d[offsetof(struct xfs_dinode, di_aformat)] = XFS_DINODE_FMT_LOCAL;
	attrs = d + core + 64;
	memset(attrs, 0, 512 - core - 64);
	if (root) {
		reader.sb.sb_rootino = number;
		put16(d + offsetof(struct xfs_dinode, di_mode), S_IFDIR | 0755);
		d[offsetof(struct xfs_dinode, di_format)] = XFS_DINODE_FMT_LOCAL;
		put64(d + offsetof(struct xfs_dinode, di_size), 6);
		put64(d + offsetof(struct xfs_dinode, di_nblocks), 0);
		put32(d + offsetof(struct xfs_dinode, di_nextents), 0);
		memset(d + core, 0, 64);
		put32(d + core + 2, number);
	}
	used = 4;
	entries = 0;
	put16(attrs, used);
}

static void
add_attr(uint8_t flags, const void *name, size_t namelen,
    const void *value, size_t valuelen)
{
	CHECK(namelen <= 255 && valuelen <= 255 && entries < 255);
	CHECK(used + 3 + namelen + valuelen <=
	    (size_t)(disk + 16 * BS + reader.sb.sb_inodesize - attrs));
	attrs[used] = namelen;
	attrs[used + 1] = valuelen;
	attrs[used + 2] = flags;
	memcpy(attrs + used + 3, name, namelen);
	if (valuelen != 0)
		memcpy(attrs + used + 3 + namelen, value, valuelen);
	used += 3 + namelen + valuelen;
	attrs[2] = ++entries;
	put16(attrs, used);
}

static int
emit(void *cookie, uint8_t flags, const uint8_t *name, size_t namelen,
    const uint8_t *value, size_t valuelen)
{
	const uint8_t *entry = attrs + 4;
	unsigned int i;
	CHECK(cookie == &emitted);
	for (i = 0; i < emitted; i++)
		entry += 3 + entry[0] + entry[1];
	CHECK(flags == entry[2] && namelen == entry[0] && valuelen == entry[1]);
	CHECK(memcmp(name, entry + 3, namelen) == 0);
	CHECK(memcmp(value, entry + 3 + namelen, valuelen) == 0);
	emitted++;
	return callback_error;
}

static void
check_attrs(int expected, unsigned int count, int policy_expected)
{
	struct xfs_inode_image before;
	const char *why;
	unsigned int reads;
	int error;
	if (crc)
		metadata_checksum(disk + 16 * BS, reader.sb.sb_inodesize, XFS_DINODE_CRC_OFF);
	expect_inode(number, 0, NULL);
	memcpy(&before, &inode, sizeof(inode));
	reads = calls;
	emitted = 0;
	error = xfs_read_attrs(&reader, &inode, NULL, 0, emit, &emitted, &why);
	if (error != expected)
		fprintf(stderr, "attributes: expected %d, got %d (%s)\n", expected, error, why);
	CHECK(error == expected && emitted == count);
	CHECK((why == NULL) == (error == 0));
	error = xfs_attrs_check_readonly(&reader, &inode, NULL, 0, &why);
	if (error != policy_expected)
		fprintf(stderr, "policy: expected %d, got %d (%s), case %u\n",
		    policy_expected, error, why, checks);
	CHECK(error == policy_expected);
	CHECK((why == NULL) == (error == 0));
	CHECK(memcmp(&inode, &before, sizeof(inode)) == 0 && calls == reads);
	checks++;
}

int
main(void)
{
	static const uint8_t binary[] = { 0, 255, 'x', 0 };
	static const char *const properties[] = { "none", "check", "optimize", "repair" };
	uint8_t large[255], save[512];
	unsigned int v, i, flags;
	size_t length;
	const char *why;

	for (v = 0; v <= 1; v++) {
		reset_attrs(v, false);
		check_attrs(0, 0, 0);
		add_attr(0, "fixture", 7, binary, sizeof(binary));
		add_attr(0, "empty", 5, NULL, 0);
		check_attrs(0, 2, 0);
		/* The reduced data fork must still return the original file bytes. */
		expect_read(0, 32, 0, 32);
		CHECK(output[1] == 32);
		length = used;
		memcpy(save, attrs, length);
		for (i = 0; i < length; i++) {
			put16(attrs, i);
			check_attrs(EINVAL, 0, EINVAL);
		}
		put16(attrs, length + 1);
		check_attrs(EINVAL, 0, EINVAL);
		put16(attrs, UINT16_MAX);
		check_attrs(EINVAL, 0, EINVAL);
		memcpy(attrs, save, length);
		attrs[2] = 0;
		check_attrs(EINVAL, 0, EINVAL);
		attrs[2] = 255;
		check_attrs(EINVAL, 0, EINVAL);
		memcpy(attrs, save, length);
		for (flags = 0; flags <= 255; flags++) {
			attrs[6] = flags;
			if (flags == 0 || flags == XFS_ATTR_ROOT || flags == XFS_ATTR_SECURE)
				check_attrs(0, 2, flags == 0 ? 0 : EOPNOTSUPP);
			else if (flags == XFS_ATTR_PARENT)
				check_attrs(EOPNOTSUPP, 0, EOPNOTSUPP);
			else
				check_attrs(EINVAL, 0, EINVAL);
		}
		memcpy(attrs, save, length);
		attrs[7] = 0;
		check_attrs(EINVAL, 0, EINVAL);
		memcpy(attrs, save, length);
		attrs[4] = 0;
		check_attrs(EINVAL, 0, EINVAL);
		memcpy(attrs, save, length);
		attrs[5] = 255;
		check_attrs(EINVAL, 0, EINVAL);
		memcpy(attrs, save, length);
		/* A bad later entry must not expose an earlier valid attribute. */
		attrs[length - 5] = 0;
		check_attrs(EINVAL, 0, EINVAL);
		memcpy(attrs, save, length);
		attrs[3] = 255; /* unused padding is not an attribute */
		check_attrs(0, 2, 0);
		callback_error = EACCES;
		check_attrs(EACCES, 1, 0);
		callback_error = 0;
		add_attr(0, "fixture", 7, "different", 9);
		check_attrs(EINVAL, 0, EINVAL);
		attrs[length + 2] = XFS_ATTR_ROOT;
		check_attrs(0, 3, EOPNOTSUPP);
		for (i = 0; i < sizeof(properties) / sizeof(properties[0]); i++) {
			reset_attrs(v, true);
			add_attr(XFS_ATTR_ROOT, "xfs:autofsck", 12, properties[i], strlen(properties[i]));
			check_attrs(0, 1, 0);
			reader.sb.sb_rootino++;
			check_attrs(0, 1, EOPNOTSUPP);
		}
		reset_attrs(v, false);
		reader.sb.sb_rootino = number; /* right number, wrong inode type */
		add_attr(XFS_ATTR_ROOT, "xfs:autofsck", 12, "repair", 6);
		check_attrs(0, 1, EOPNOTSUPP);
		reset_attrs(v, true);
		add_attr(XFS_ATTR_ROOT, "xfs:autofsck", 12, "repair\0", 7);
		check_attrs(0, 1, EOPNOTSUPP);
		reset_attrs(v, true);
		add_attr(XFS_ATTR_ROOT, "xfs:autofsck", 12, "unknown", 7);
		check_attrs(0, 1, EOPNOTSUPP);
		reset_attrs(v, true);
		add_attr(XFS_ATTR_ROOT, "SGI_ACL_FILE", 12, binary, sizeof(binary));
		check_attrs(0, 1, EOPNOTSUPP);
		reset_attrs(v, true);
		add_attr(XFS_ATTR_SECURE, "selinux", 7, binary, sizeof(binary));
		check_attrs(0, 1, EOPNOTSUPP);
		reset_attrs(v, true);
		add_attr(XFS_ATTR_ROOT, "xfs:autofsck", 12, "repair", 6);
		add_attr(XFS_ATTR_ROOT, "SGI_ACL_DEFAULT", 15, binary, sizeof(binary));
		check_attrs(0, 2, EOPNOTSUPP); /* no early success on the property */
		reset_attrs(v, false);
		add_attr(0, "SGI_ACL_FILE", 12, binary, sizeof(binary));
		check_attrs(0, 1, 0); /* namespace, not just spelling, defines ACLs */
		reset_attrs(v, false);
		memset(large, 255, sizeof(large));
		add_attr(0, large, sizeof(large), NULL, 0);
		check_attrs(0, 1, 0);
		reset_attrs(v, false);
		add_attr(0, "x", 1, large, sizeof(large));
		check_attrs(0, 1, 0);
		reset_attrs(v, false);
		disk[16 * BS + offsetof(struct xfs_dinode, di_aformat)] = XFS_DINODE_FMT_EXTENTS;
		check_attrs(0, 0, 0); /* a retained, empty fork */
		put16(disk + 16 * BS + offsetof(struct xfs_dinode, di_anextents), 1);
		put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_nblocks), 5);
		check_attrs(EMSGSIZE, 0, EMSGSIZE);
		disk[16 * BS + offsetof(struct xfs_dinode, di_aformat)] = XFS_DINODE_FMT_BTREE;
		put16(disk + 16 * BS + offsetof(struct xfs_dinode, di_anextents), 32);
		put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_nblocks), 36);
		check_attrs(EMSGSIZE, 0, EMSGSIZE);
		/* Largest on-disk count, in a 2 KiB inode: all 255 byte names. */
		reset_attrs(v, false);
		reader.sb.sb_inodesize = 2048;
		reader.sb.sb_inodelog = 11;
		reader.sb.sb_inopblock = 2;
		reader.sb.sb_inopblog = 1;
		number = 32; /* same disk block, coherent updated inode geometry */
		put32(disk + 3 * BS + (v ? XFS_BTREE_SBLOCK_CRC_LEN : XFS_BTREE_SBLOCK_LEN), 32);
		tree_crc(3, false);
		if (v) put64(disk + 16 * BS + offsetof(struct xfs_dinode, di_ino), number);
		for (i = 1; i <= 255; i++) {
			uint8_t name = i;
			add_attr(0, &name, 1, NULL, 0);
		}
		check_attrs(0, 255, 0);
		setup(v, 0, false);
		expect_inode(number, 0, NULL);
		emitted = 0;
		CHECK(xfs_read_attrs(&reader, &inode, NULL, 0, emit, &emitted, NULL) == 0 && emitted == 0);
	}
	CHECK(xfs_read_attrs(NULL, &inode, NULL, 0, emit, &emitted, &why) == EINVAL);
	CHECK(xfs_read_attrs(&reader, NULL, NULL, 0, emit, &emitted, &why) == EINVAL);
	CHECK(xfs_read_attrs(&reader, &inode, NULL, 0, NULL, NULL, &why) == EINVAL);
	CHECK(xfs_attrs_check_readonly(NULL, &inode, NULL, 0, NULL) == EINVAL);
	CHECK(xfs_attrs_check_readonly(&reader, NULL, NULL, 0, NULL) == EINVAL);
	printf("Passed %u inline attribute and mount-policy checks\n", checks);
	return 0;
}
