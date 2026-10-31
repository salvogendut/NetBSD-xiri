/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */

#include "xfs_directory.h"
#include "xfs_log.h"
#include "xfs_attr.h"

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

/* JSON escapes preserve arbitrary filename bytes without terminal controls. */
static int
print_entry(void *cookie, uint64_t ino, uint8_t type, const uint8_t *name, size_t length)
{
	size_t i;
	(void)cookie;
	printf("{\"ino\":%" PRIu64 ",\"type\":%u,\"name\":\"", ino, type);
	for (i = 0; i < length; i++) {
		if (name[i] < 32 || name[i] >= 127 || name[i] == '"' || name[i] == '\\')
			printf("\\u%04x", name[i]);
		else
			putchar(name[i]);
	}
	fputs("\"}\n", stdout);
	return ferror(stdout) ? EIO : 0;
}

static int
print_attr(void *cookie, uint8_t flags, const uint8_t *name, size_t nlen,
    const uint8_t *value, size_t vlen)
{
	size_t i;
	(void)cookie;
	printf("{\"flags\":%u,\"name\":\"", flags);
	for (i = 0; i < nlen; i++) printf("%02x", name[i]);
	fputs("\",\"value\":\"", stdout);
	for (i = 0; i < vlen; i++) printf("%02x", value[i]);
	fputs("\"}\n", stdout);
	return ferror(stdout) ? EIO : 0;
}

int
main(int argc, char **argv)
{
	static uint8_t data[XFS_DIR_WORKSPACE_SIZE];
	uint8_t output[XFS_MAX_BLOCKSIZE];
	struct xfs_path_workspace path_work;
	struct xfs_inode_image inode;
	struct xfs_sb sb;
	struct xfs_reader reader;
	struct xfs_io io;
	struct xfs_ag_headers ag;
	struct xfs_log_info log;
	struct stat st;
	const char *why, *path, *image_path = NULL;
	uint64_t agno = 0, ino = 0, offset = 0, length = 0;
	size_t done, part;
	bool inspect_ag = false, inspect_inode = false, read_file = false;
	bool list_directory = false, read_link = false;
	bool inspect_log = false, raw = false;
	bool read_attrs = false, attr_policy = false;
	int fd, error;

	if (argc > 1 && strcmp(argv[1], "--raw") == 0) {
		raw = true;
		argc--;
		argv++;
	}
	if (argc == 3 && strcmp(argv[1], "-j") == 0) {
		inspect_log = true;
		path = argv[2];
	} else if (argc == 4 && strcmp(argv[1], "-a") == 0) {
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
	} else if (argc == 4 && (strcmp(argv[1], "-p") == 0 ||
	    strcmp(argv[1], "-l") == 0 || strcmp(argv[1], "-s") == 0 ||
	    strcmp(argv[1], "-x") == 0 || strcmp(argv[1], "-X") == 0)) {
		inspect_inode = argv[1][1] == 'p';
		list_directory = argv[1][1] == 'l';
		read_link = argv[1][1] == 's';
		read_file = read_link;
		attr_policy = argv[1][1] == 'X';
		read_attrs = argv[1][1] == 'x' || attr_policy;
		length = XFS_SYMLINK_MAXLEN;
		image_path = argv[2];
		path = argv[3];
	} else if (argc == 6 && strcmp(argv[1], "-R") == 0) {
		read_file = true;
		image_path = argv[2];
		offset = number(argv[3]);
		length = number(argv[4]);
		path = argv[5];
	} else if (argc == 2) {
		path = argv[1];
	} else {
		fprintf(stderr, "usage: xfs_probe [--raw] [-j | -a agno | -i ino] image\n"
		    "       xfs_probe -r ino offset length image\n"
		    "       xfs_probe {-p|-l|-s|-x|-X} /path/in/image image\n"
		    "       -x dumps attributes as hex JSON; -X also applies mount policy\n"
		    "       xfs_probe -R /path/in/image offset length image\n"
		    "       --raw skips the clean-journal guard for image analysis\n");
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
	if (error == 0 && (inspect_log ||
	    (!raw && (inspect_inode || read_file || list_directory || read_attrs)))) {
		void *log_scratch = malloc(XFS_LOG_SCRATCH_SIZE);
		if (log_scratch == NULL) {
			error = ENOMEM;
			why = "cannot allocate journal workspace";
		} else {
			if (inspect_log)
				error = xfs_log_inspect(&reader, log_scratch,
				    XFS_LOG_SCRATCH_SIZE, &log, &why);
			else
				error = xfs_require_clean_log(&reader, log_scratch,
				    XFS_LOG_SCRATCH_SIZE, &why);
			free(log_scratch);
		}
	}
	if (error == 0 && inspect_ag)
		error = xfs_read_ag(&reader, (uint32_t)agno, data, sizeof(data), &ag, &why);
	if (error == 0 && image_path != NULL)
		error = xfs_lookup_path(&reader, image_path, strlen(image_path), !read_link,
		    &path_work, data, sizeof(data), &ino, &why);
	if (error == 0 && (inspect_inode || read_file || list_directory || read_attrs))
		error = xfs_read_inode(&reader, ino, data, sizeof(data), &inode, &why);
	if (error == 0 && read_attrs) {
		void *work = malloc(XFS_ATTR_WORKSPACE_SIZE);
		if (work == NULL) {
			error = ENOMEM;
			why = "cannot allocate attribute workspace";
		} else {
			if (attr_policy)
				error = xfs_attrs_check_readonly(&reader, &inode, work,
				    XFS_ATTR_WORKSPACE_SIZE, &why);
			if (error == 0)
				error = xfs_read_attrs(&reader, &inode, work, XFS_ATTR_WORKSPACE_SIZE,
				    print_attr, NULL, &why);
			free(work);
		}
		if (fflush(stdout) != 0 && error == 0) {
			error = EIO;
			why = "attribute output flush failed";
		}
	}
	if (error == 0 && read_link && !S_ISLNK(inode.mode)) {
		error = EINVAL;
		why = "readlink requires a symlink";
	}
	if (error == 0 && list_directory) {
		error = xfs_read_directory(&reader, &inode, data, sizeof(data), print_entry, NULL, &why);
		if (fflush(stdout) != 0 && error == 0) {
			error = EIO;
			why = "directory output flush failed";
		}
	}
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
	if (read_file || list_directory || read_attrs)
		return 0;
	if (inspect_log) {
		printf("state = %s\n", log.state == XFS_LOG_CLEAN ? "clean" : "needs-recovery");
		printf("head = %" PRIu32 "\n", log.head);
		printf("tail = %" PRIu32 "\n", log.tail);
		printf("record = %" PRIu32 "\n", log.record);
		printf("cycle = %" PRIu32 "\n", log.cycle);
		printf("initialized = %u\n", log.initialized);
		return 0;
	}
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
