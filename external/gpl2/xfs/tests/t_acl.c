/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
#include "fixture.h"
#include "xfs_acl.h"
#include <sys/stat.h>
#include <errno.h>

static uint8_t buffer[65538], save[128];
static uint8_t *acl = buffer + 1; /* deliberately unaligned */
static struct xfs_sb sb;
static unsigned int checks;

static void
word(unsigned int offset, uint32_t value)
{
	__be32 disk = cpu_to_be32(value);
	memcpy(acl + offset, &disk, 4);
}

static void
entry(unsigned int i, uint32_t tag, uint32_t id, unsigned int perm)
{
	__be16 disk = cpu_to_be16(perm);
	word(4 + 12 * i, tag);
	word(8 + 12 * i, id);
	memcpy(acl + 12 + 12 * i, &disk, 2);
}

static void
valid(size_t length, uint16_t mode, bool defaults, int expected)
{
	const char *why;
	int error = xfs_acl_validate(&sb, acl, length, mode, defaults, &why);
	if (error != expected)
		fprintf(stderr, "ACL case %u: got %d, expected %d (%s)\n", checks, error, expected, why);
	CHECK(error == expected && (why == NULL) == (error == 0));
	checks++;
}

static bool
groups(void *cookie, uint32_t gid)
{
	unsigned int mask = *(unsigned int *)cookie;
	return (gid == 34 && (mask & 1)) || (gid == 77 && (mask & 2)) || (gid == 78 && (mask & 4));
}

static void
make(unsigned int a, unsigned int b, unsigned int mask)
{
	memset(buffer, 0xa5, sizeof(buffer));
	word(0, 8);
	entry(0, XFS_ACL_USER_OBJ, UINT32_MAX, 6);
	entry(1, XFS_ACL_USER, 99, a);
	entry(2, XFS_ACL_USER, 100, 0);
	entry(3, XFS_ACL_GROUP_OBJ, UINT32_MAX, 2);
	entry(4, XFS_ACL_GROUP, 77, a);
	entry(5, XFS_ACL_GROUP, 78, b);
	entry(6, XFS_ACL_MASK, UINT32_MAX, mask);
	entry(7, XFS_ACL_OTHER, UINT32_MAX, 5);
}

int
main(void)
{
	union sector disk;
	unsigned int v, i, a, b, m, want, membership, u, count;
	static const uint32_t users[] = { 12, 99, 100, 101, 0 };
	bool allow, group_allows;
	const char *why;
	for (v = 0; v <= 1; v++) {
		fixture(&disk, v);
		CHECK(xfs_probe_superblock(disk.bytes, 512, IMAGE_BYTES, &sb, &why) == 0);
		for (a = 0; a < 8; a++) for (b = 0; b < 8; b++) for (m = 0; m < 8; m++) {
			make(a, b, m);
			valid(100, S_IFREG | 0605 | (m << 3), false, 0);
			for (want = 0; want < 8; want++) for (membership = 0; membership < 8; membership++) {
				group_allows = ((membership & 1) && (2U & want) == want) ||
				    ((membership & 2) && (a & want) == want) ||
				    ((membership & 4) && (b & want) == want);
				for (u = 0; u < sizeof(users) / sizeof(users[0]); u++) {
					if (users[u] == 12) allow = (6U & want) == want;
					else if (users[u] == 99) allow = (a & m & want) == want;
					else if (users[u] == 100) allow = want == 0;
					else if (membership) allow = group_allows && (m & want) == want;
					else allow = (5U & want) == want;
					CHECK(xfs_acl_permission(acl, 100, 12, 34, users[u],
					    groups, &membership, want) == (allow ? 0 : EACCES));
					checks++;
				}
			}
			CHECK(buffer[0] == 0xa5 && buffer[sizeof(buffer) - 1] == 0xa5);
		}
		make(4, 1, 5);
		memcpy(save, acl, 100);
		for (i = 0; i < 128; i++) if (i != 100) valid(i, S_IFREG | 0655, false, EINVAL);
		valid(100, S_IFREG | 0654, false, EINVAL);
		valid(100, S_IFREG | 0755, false, EINVAL);
		valid(100, S_IFREG | 0645, false, EINVAL);
		valid(100, S_IFDIR | 0700, true, 0); /* defaults do not govern directory access */
		valid(100, S_IFREG | 0655, true, EINVAL);
		valid(100, S_IFLNK | 0655, false, EINVAL);
		for (i = 0; i < 8; i++) {
			entry(i, UINT32_MAX, UINT32_MAX, 7);
			valid(100, S_IFREG | 0655, false, EINVAL);
			memcpy(acl, save, 100);
			word(4 + i * 12, (uint32_t[]){1, 2, 2, 4, 8, 8, 16, 32}[i] | 0x10000);
			valid(100, S_IFREG | 0655, false, EINVAL); /* reject full-width tag, not truncated */
			memcpy(acl, save, 100);
			entry(i, (uint32_t[]){1, 2, 2, 4, 8, 8, 16, 32}[i], 99, 8);
			valid(100, S_IFREG | 0655, false, EINVAL);
			memcpy(acl, save, 100);
		}
		word(8 + 2 * 12, 99); valid(100, S_IFREG | 0655, false, EINVAL);
		word(8 + 2 * 12, 98); valid(100, S_IFREG | 0655, false, 0); /* unsorted IDs */
		word(8 + 2 * 12, UINT32_MAX); valid(100, S_IFREG | 0655, false, EINVAL);
		memcpy(acl, save, 100);
		word(8 + 5 * 12, 77); valid(100, S_IFREG | 0655, false, EINVAL);
		word(8 + 5 * 12, 99); valid(100, S_IFREG | 0655, false, 0); /* same numeric user/group */
		memcpy(acl, save, 100);
		word(4 + 6 * 12, XFS_ACL_GROUP); valid(100, S_IFREG | 0655, false, EINVAL);
		for (i = 0; i < 3; i++) { word(0, i); valid(4 + i * 12, S_IFREG | 0655, false, EINVAL); }
		word(0, UINT32_MAX); valid(100, S_IFREG | 0655, false, EINVAL);
		/* Minimal ACL, and mask without named entries. Ignore unused IDs/pad. */
		word(0, 3); entry(0, 1, 123, 6); entry(1, 4, 123, 4); entry(2, 32, 123, 0);
		valid(40, S_IFREG | 0640, false, 0);
		word(0, 4); entry(2, 16, 123, 0); entry(3, 32, 123, 0);
		valid(52, S_IFREG | 0600, false, 0);
		/* Full format limits, including a 64 KiB v5 ACL. */
		for (count = 25; count <= 5461; count = count == 25 ? 26 : 5461) {
			word(0, count); entry(0, 1, 0, 6);
			for (i = 1; i < count - 3; i++) entry(i, 2, 1000 + i, 4);
			entry(count - 3, 4, 0, 0); entry(count - 2, 16, 0, 4); entry(count - 1, 32, 0, 0);
			valid(4 + count * 12, S_IFREG | 0640, false, v || count == 25 ? 0 : EINVAL);
			if (count == 5461) break;
		}
	}
	CHECK(xfs_acl_validate(NULL, acl, 40, 0, false, NULL) == EINVAL);
	CHECK(xfs_acl_validate(&sb, NULL, 40, 0, false, NULL) == EINVAL);
	membership = 0;
	CHECK(xfs_acl_permission(NULL, 40, 0, 0, 0, groups, &membership, 4) == EINVAL);
	CHECK(xfs_acl_permission(acl, 65536, 0, 0, 0, NULL, NULL, 4) == EINVAL);
	CHECK(xfs_acl_permission(acl, 65536, 0, 0, 0, groups, &membership, 8) == EINVAL);
	printf("Passed %u POSIX ACL validation and permission checks\n", checks);
	return 0;
}
