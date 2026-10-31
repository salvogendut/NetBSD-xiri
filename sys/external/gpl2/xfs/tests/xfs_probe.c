/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#include "xfs_probe.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	uint8_t data[XFS_MAX_SECTORSIZE];
	struct xfs_sb sb;
	struct stat st;
	const char *why;
	size_t length = 0;
	ssize_t n;
	int fd, error;

	if (argc != 2) {
		fprintf(stderr, "usage: xfs_probe image\n");
		return 2;
	}
	fd = open(argv[1], O_RDONLY | O_NONBLOCK);
	if (fd == -1) {
		perror(argv[1]);
		return 2;
	}
	if (fstat(fd, &st) == -1) {
		perror("fstat");
		close(fd);
		return 2;
	}
	if (!S_ISREG(st.st_mode) || st.st_size < 0) {
		fprintf(stderr, "xfs_probe: input must be a regular image file\n");
		close(fd);
		return 2;
	}
	while (length < sizeof(data)) {
		n = read(fd, data + length, sizeof(data) - length);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			perror("read");
			close(fd);
			return 2;
		}
		if (n == 0)
			break;
		length += (size_t)n;
	}
	close(fd);
	error = xfs_probe_superblock(data, length, (uint64_t)st.st_size, &sb, &why);
	if (error != 0) {
		fprintf(stderr, "xfs_probe: %s\n", why);
		return 1;
	}
	printf("version = %u\n", XFS_SB_VERSION_NUM(&sb));
	printf("blocksize = %" PRIu32 "\n", sb.sb_blocksize);
	printf("dblocks = %" PRIu64 "\n", sb.sb_dblocks);
	printf("agblocks = %" PRIu32 "\n", sb.sb_agblocks);
	printf("agcount = %" PRIu32 "\n", sb.sb_agcount);
	printf("sectsize = %u\n", sb.sb_sectsize);
	printf("inodesize = %u\n", sb.sb_inodesize);
	printf("rootino = %" PRIu64 "\n", sb.sb_rootino);
	printf("logstart = %" PRIu64 "\n", sb.sb_logstart);
	printf("logblocks = %" PRIu32 "\n", sb.sb_logblocks);
	printf("features_incompat = %" PRIu32 "\n", sb.sb_features_incompat);
	printf("features_ro_compat = %" PRIu32 "\n", sb.sb_features_ro_compat);
	return 0;
}
