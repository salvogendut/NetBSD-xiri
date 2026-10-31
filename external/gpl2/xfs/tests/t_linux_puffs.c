/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
/* Compare an actual NetBSD mount with observations made by Linux syscalls. */
#include <sys/types.h>
#include <sys/extattr.h>
#include <sys/sha2.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) errx(1, "line %d: %s (errno %d: %s)", \
    __LINE__, #test, errno, strerror(errno)); } while (0)

static void
metadata(const char *line)
{
	char path[256], expected[65], hash[SHA256_DIGEST_STRING_LENGTH];
	uint64_t ino, size, allocated;
	int64_t seconds;
	unsigned int mode, uid, gid, links, nanos;
	struct stat st;
	CHECK(sscanf(line, "file %255s %" SCNu64 " %u %u %u %" SCNu64 " %u %" SCNu64
	    " %" SCNd64 " %u %64s", path, &ino, &mode, &uid, &gid, &size, &links,
	    &allocated, &seconds, &nanos, expected) == 11);
	CHECK(lstat(path, &st) == 0);
	CHECK(st.st_ino == ino && st.st_mode == mode && st.st_uid == uid && st.st_gid == gid);
	CHECK(st.st_size >= 0 && (uint64_t)st.st_size == size && st.st_nlink == links);
	CHECK(st.st_blocks >= 0 && (uint64_t)st.st_blocks * 512 == allocated);
	CHECK(st.st_mtim.tv_sec == seconds && (uint32_t)st.st_mtim.tv_nsec == nanos);
	if (S_ISREG(st.st_mode)) {
		CHECK(SHA256_File(path, hash) != NULL);
		CHECK(strcmp(hash, expected) == 0);
	}
}

static void
permission(const char *line)
{
	char path[256];
	unsigned int uid, gid, group;
	int expected, fd, status;
	pid_t child;
	CHECK(sscanf(line, "access %255s %u %u %u %d", path, &uid, &gid, &group, &expected) == 5);
	child = fork(); CHECK(child != -1);
	if (child == 0) {
		gid_t supplementary = group;
		CHECK(setgroups(group ? 1 : 0, &supplementary) == 0);
		CHECK(setgid(gid) == 0 && setuid(uid) == 0);
		fd = open(path, O_RDONLY);
		CHECK(fd != -1 || errno == EACCES);
		CHECK((fd != -1) == expected);
		CHECK((access(path, R_OK) == 0) == expected);
		if (!expected) CHECK(errno == EACCES);
		if (fd != -1) CHECK(close(fd) == 0);
		_exit(0);
	}
	CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
attribute(const char *line)
{
	char path[256], name[256], hex[16385];
	unsigned char data[8193];
	unsigned int flags, value;
	size_t i, length;
	int ns;
	CHECK(sscanf(line, "attr %255s %u %255s %16384s", path, &flags, name, hex) == 4);
	CHECK(flags == 0 || flags == 2);
	ns = flags == 0 ? EXTATTR_NAMESPACE_USER : EXTATTR_NAMESPACE_SYSTEM;
	length = strlen(hex) / 2;
	CHECK(extattr_get_file(path, ns, name, NULL, 0) == (ssize_t)length);
	CHECK(extattr_get_file(path, ns, name, data, sizeof(data)) == (ssize_t)length);
	for (i = 0; i < length; i++) {
		CHECK(sscanf(hex + i * 2, "%2x", &value) == 1);
		CHECK(data[i] == value);
	}
}

static void
directory(void)
{
	bool seen[768] = { false };
	DIR *dir = opendir("many");
	struct dirent *entry;
	unsigned int i, count = 0;
	char name[6];
	CHECK(dir != NULL);
	for (;;) {
		errno = 0; entry = readdir(dir);
		if (entry == NULL) { CHECK(errno == 0); break; }
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
		CHECK(sscanf(entry->d_name, "f%u", &i) == 1 && i < 768 && i % 4 && !seen[i]);
		CHECK(snprintf(name, sizeof(name), "f%04u", i) == 5 && !strcmp(name, entry->d_name));
		seen[i] = true; count++;
	}
	CHECK(count == 576 && closedir(dir) == 0);
	for (i = 0; i < 768; i++) CHECK(seen[i] == (i % 4 != 0));
}

int
main(int argc, char **argv)
{
	FILE *manifest;
	char line[18000], target[32];
	unsigned int files = 0, access_cases = 0, attrs = 0;
	struct stat file, alias;
	CHECK(argc == 3 && getuid() == 0);
	manifest = fopen(argv[2], "r"); CHECK(manifest != NULL);
	CHECK(chdir(argv[1]) == 0);
	while (fgets(line, sizeof(line), manifest)) {
		CHECK(strchr(line, '\n') != NULL);
		if (!strncmp(line, "file ", 5)) { metadata(line); files++; }
		else if (!strncmp(line, "access ", 7)) { permission(line); access_cases++; }
		else if (!strncmp(line, "attr ", 5)) { attribute(line); attrs++; }
		else CHECK(false);
	}
	CHECK(!ferror(manifest) && fclose(manifest) == 0);
	CHECK(files >= 12 && access_cases == 24 && attrs >= 6);
	directory();
	CHECK(readlink("link", target, sizeof(target)) == 9 && !memcmp(target, "plain.bin", 9));
	CHECK(stat("plain.bin", &file) == 0 && stat("alias", &alias) == 0);
	CHECK(file.st_ino == alias.st_ino && file.st_nlink == 2);
	errno = 0; CHECK(open("plain.bin", O_WRONLY) == -1 && errno == EROFS);
	printf("Passed native Linux references: %u files, %u permission cases, %u attributes, directory and links\n",
	    files, access_cases, attrs);
	return 0;
}
