/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#include "xfs_io.h"

#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static int
image_read(void *cookie, uint64_t offset, void *data, size_t length, size_t *done)
{
	ssize_t n;

	do {
		n = pread(*(int *)cookie, data, length, (off_t)offset);
	} while (n < 0 && errno == EINTR);
	if (n < 0)
		return errno;
	*done = (size_t)n;
	return 0;
}

int
main(int argc, char **argv)
{
	uint8_t data[XFS_MAX_SECTORSIZE];
	struct xfs_sb sb;
	struct xfs_reader reader;
	struct xfs_io io;
	struct xfs_ag_headers ag;
	struct stat st;
	const char *why, *path;
	char *end;
	unsigned long long agno = 0;
	bool inspect_ag = false;
	int fd, error;

	if (argc == 4 && strcmp(argv[1], "-a") == 0) {
		errno = 0;
		agno = strtoull(argv[2], &end, 10);
		if (errno != 0 || *argv[2] == '\0' || *argv[2] == '-' ||
		    *end != '\0' || agno > UINT32_MAX) {
			fprintf(stderr, "xfs_probe: invalid allocation group number\n");
			return 2;
		}
		inspect_ag = true;
		path = argv[3];
	} else if (argc == 2) {
		path = argv[1];
	} else {
		fprintf(stderr, "usage: xfs_probe [-a agno] image\n");
		return 2;
	}
	fd = open(path, O_RDONLY | O_NONBLOCK);
	if (fd == -1) {
		perror(path);
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
	io.cookie = &fd;
	io.size = (uint64_t)st.st_size;
	io.read = image_read;
	error = xfs_reader_open(&io, data, sizeof(data), &reader, &why);
	if (error == 0 && inspect_ag)
		error = xfs_read_ag(&reader, (uint32_t)agno, data, sizeof(data), &ag, &why);
	close(fd);
	if (error != 0) {
		fprintf(stderr, "xfs_probe: %s (%s)\n", why, strerror(error));
		return 1;
	}
	if (inspect_ag) {
#define PRINT_AG(kind, field) \
	printf(#kind "." #field " = %" PRIu32 "\n", \
	    be32_to_cpu(ag.kind.kind ## _ ## field))
		PRINT_AG(agf, seqno);
		PRINT_AG(agf, length);
		PRINT_AG(agf, freeblks);
		PRINT_AG(agf, longest);
		PRINT_AG(agf, flcount);
		PRINT_AG(agi, seqno);
		PRINT_AG(agi, length);
		PRINT_AG(agi, count);
		PRINT_AG(agi, freecount);
		return 0;
	}
	sb = reader.sb;
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
