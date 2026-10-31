/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#include "xfs_inode.h"

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

static uint64_t
number(const char *s)
{
	char *end;
	unsigned long long n;
	errno = 0;
	n = strtoull(s, &end, 10);
	if (errno != 0 || *s < '0' || *s > '9' || *end != '\0') {
		fprintf(stderr, "xfs_probe: invalid number: %s\n", s);
		exit(2);
	}
	return n;
}

int
main(int argc, char **argv)
{
	uint8_t data[XFS_MAX_BLOCKSIZE], output[XFS_MAX_BLOCKSIZE];
	struct xfs_inode_image inode;
	struct xfs_sb sb;
	struct xfs_reader reader;
	struct xfs_io io;
	struct xfs_ag_headers ag;
	struct stat st;
	const char *why, *path;
	uint64_t agno = 0, ino = 0, offset = 0, length = 0;
	size_t done, part;
	bool inspect_ag = false, inspect_inode = false, read_file = false;
	int fd, error;

	if (argc == 4 && strcmp(argv[1], "-a") == 0) {
		agno = number(argv[2]);
		if (agno > UINT32_MAX) {
			fprintf(stderr, "xfs_probe: invalid allocation group number\n");
			return 2;
		}
		inspect_ag = true;
		path = argv[3];
	} else if (argc == 4 && strcmp(argv[1], "-i") == 0) {
		inspect_inode = true;
		ino = number(argv[2]);
		path = argv[3];
	} else if (argc == 6 && strcmp(argv[1], "-r") == 0) {
		read_file = true;
		ino = number(argv[2]);
		offset = number(argv[3]);
		length = number(argv[4]);
		path = argv[5];
	} else if (argc == 2) {
		path = argv[1];
	} else {
		fprintf(stderr, "usage: xfs_probe [-a agno | -i ino] image\n"
		    "       xfs_probe -r ino offset length image\n");
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
	if (error == 0 && (inspect_inode || read_file))
		error = xfs_read_inode(&reader, ino, data, sizeof(data), &inode, &why);
	if (error == 0 && read_file) {
		do {
			part = length > sizeof(output) ? sizeof(output) : (size_t)length;
			error = xfs_read_file(&reader, &inode, offset, output, part,
			    &done, data, sizeof(data), &why);
			if (error != 0 || done == 0)
				break;
			if (fwrite(output, 1, done, stdout) != done) {
				error = EIO;
				why = "output write failed";
				break;
			}
			offset += done;
			length -= done;
		} while (length != 0);
		if (fflush(stdout) != 0 && error == 0) {
			error = EIO;
			why = "output flush failed";
		}
	}
	close(fd);
	if (error != 0) {
		fprintf(stderr, "xfs_probe: %s (%s)\n", why, strerror(error));
		return 1;
	}
	if (read_file)
		return 0;
	if (inspect_inode) {
		printf("mode = %u\n", inode.mode);
		printf("size = %" PRIu64 "\n", inode.size);
		printf("nextents = %" PRIu64 "\n", inode.nextents);
		printf("format = %u\n", inode.format);
		return 0;
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
