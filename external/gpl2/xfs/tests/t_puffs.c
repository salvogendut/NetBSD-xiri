/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
/* Native NetBSD integration checks against an actual PUFFS mount. */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/wait.h>

#include <dirent.h>
#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(test) do { if (!(test)) errx(1, "line %d: %s (errno %d: %s)", \
    __LINE__, #test, errno, strerror(errno)); } while (0)
#define FAILS(call, expected) do { errno = 0; CHECK((call) == -1); \
    CHECK(errno == (expected)); } while (0)

static const char hello[] = "NetBSD XFS through PUFFS\n";

static void
read_hello(const char *path, bool allowed)
{
	char data[64];
	int fd = open(path, O_RDONLY);
	if (!allowed) {
		CHECK(fd == -1 && errno == EACCES);
		return;
	}
	CHECK(fd >= 0);
	CHECK(read(fd, data, sizeof(data)) == sizeof(hello) - 1);
	CHECK(memcmp(data, hello, sizeof(hello) - 1) == 0);
	CHECK(read(fd, data, sizeof(data)) == 0);
	CHECK(close(fd) == 0);
}

static void
permissions(uid_t uid, gid_t gid, bool owner, bool group)
{
	int status, inherited = open("private", O_RDONLY);
	pid_t pid;
	CHECK(inherited >= 0);
	pid = fork();
	CHECK(pid != -1);
	if (pid == 0) {
		char data[sizeof(hello)];
		int fd;
		CHECK(setgroups(0, NULL) == 0);
		CHECK(setgid(gid) == 0);
		CHECK(setuid(uid) == 0);
		read_hello("hello", owner || group);
		read_hello("private", owner);
		read_hello("locked/file", owner);
		fd = open("public", O_RDONLY);
		CHECK(fd >= 0);
		CHECK(close(fd) == 0);
		/* Existing descriptors retain access after credentials change. */
		CHECK(read(inherited, data, sizeof(data)) == sizeof(hello) - 1);
		CHECK(memcmp(data, hello, sizeof(hello) - 1) == 0);
		_exit(0);
	}
	CHECK(waitpid(pid, &status, 0) == pid);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
	CHECK(close(inherited) == 0);
}

static void
directories(void)
{
	DIR *dir = opendir("many");
	struct dirent *entry;
	bool seen[1800] = { false };
	unsigned int count = 0, dots = 0, i;
	int fd;
	ssize_t bytes;
	union { struct dirent alignment; char data[512]; } buffer;
	CHECK(dir != NULL);
	for (;;) {
		errno = 0;
		entry = readdir(dir);
		if (entry == NULL) {
			CHECK(errno == 0);
			break;
		}
		if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) {
			CHECK(entry->d_type == DT_DIR);
			dots++;
			continue;
		}
		CHECK(entry->d_namlen == 5 && entry->d_name[0] == 'f');
		i = (unsigned int)strtoul(entry->d_name + 1, NULL, 10);
		CHECK(i < 1800 && !seen[i] && entry->d_type == DT_REG);
		seen[i] = true;
		if (++count == 500) {
			char next[256];
			long position = telldir(dir);
			CHECK(position != -1);
			entry = readdir(dir);
			CHECK(entry != NULL);
			strlcpy(next, entry->d_name, sizeof(next));
			seekdir(dir, position);
			entry = readdir(dir);
			CHECK(entry != NULL && strcmp(next, entry->d_name) == 0);
			seekdir(dir, position);
		}
	}
	CHECK(count == 1800 && dots == 2);
	CHECK(readdir(dir) == NULL);
	rewinddir(dir);
	CHECK(readdir(dir) != NULL);
	CHECK(closedir(dir) == 0);
	/* Small buffers force multiple kernel/server requests and offset resumes. */
	fd = open("many", O_RDONLY | O_DIRECTORY);
	CHECK(fd >= 0);
	count = 0;
	while ((bytes = getdents(fd, buffer.data, sizeof(buffer.data))) > 0) {
		size_t offset = 0;
		while (offset < (size_t)bytes) {
			entry = (void *)(buffer.data + offset);
			CHECK(entry->d_reclen > 0 && entry->d_reclen <= (size_t)bytes - offset);
			offset += entry->d_reclen;
			count++;
		}
		CHECK(offset == (size_t)bytes);
	}
	CHECK(bytes == 0 && count == 1802);
	CHECK(getdents(fd, buffer.data, sizeof(buffer.data)) == 0);
	CHECK(lseek(fd, 0, SEEK_SET) == 0);
	CHECK(getdents(fd, buffer.data, sizeof(buffer.data)) > 0);
	CHECK(close(fd) == 0);
}

int
main(int argc, char **argv)
{
	struct stat st, alias;
	struct statvfs vfs;
	unsigned char data[8195];
	char target[1024];
	time_t seconds;
	size_t i;
	int fd, other;
	CHECK(argc == 3);
	if (strcmp(argv[2], "stale") == 0) {
		char *path;
		CHECK(asprintf(&path, "%s/hello", argv[1]) != -1);
		FAILS(stat(path, &st), ESTALE);
		FAILS(stat(path, &st), ESTALE);
		free(path);
		puts("Passed persistent stale-image rejection");
		return 0;
	}
	CHECK(chdir(argv[1]) == 0);
	seconds = strcmp(argv[2], "5") == 0 ? INT64_C(4102444800) : -1;
	CHECK(stat("hello", &st) == 0 && stat("alias", &alias) == 0);
	CHECK(st.st_ino == alias.st_ino && st.st_dev == alias.st_dev);
	CHECK(st.st_uid == 12 && st.st_gid == 34 && st.st_nlink == 2);
	CHECK((st.st_mode & 07777) == 0640 && S_ISREG(st.st_mode));
	CHECK(st.st_size == sizeof(hello) - 1 && st.st_blocks == 8);
	CHECK(st.st_atim.tv_sec == seconds && st.st_atim.tv_nsec == 123456789);
	CHECK(st.st_mtim.tv_sec == seconds && st.st_mtim.tv_nsec == 123456789);
	CHECK(st.st_ctim.tv_sec == seconds && st.st_ctim.tv_nsec == 123456789);
	if (seconds > 0)
		CHECK(st.st_birthtim.tv_sec == seconds && st.st_birthtim.tv_nsec == 123456789);
	read_hello("hello", true);
	read_hello("alias", true);
	read_hello("link", true);
	read_hello("private", true);
	/* flock must contend through hard links: identical inode numbers alone
	 * would not detect separate PUFFS cookies for the same inode. */
	fd = open("hello", O_RDONLY);
	other = open("alias", O_RDONLY);
	CHECK(fd >= 0 && other >= 0);
	CHECK(flock(fd, LOCK_EX | LOCK_NB) == 0);
	FAILS(flock(other, LOCK_EX | LOCK_NB), EWOULDBLOCK);
	CHECK(flock(fd, LOCK_UN) == 0);
	CHECK(flock(other, LOCK_EX | LOCK_NB) == 0);
	CHECK(close(fd) == 0 && close(other) == 0);
	fd = open("public", O_RDONLY);
	CHECK(fd >= 0);
	CHECK(pread(fd, data, sizeof(data), 253) == sizeof(data));
	for (i = 0; i < sizeof(data); i++)
		CHECK(data[i] == (unsigned char)(253 + i));
	CHECK(pread(fd, data, sizeof(data), 131072) == 3 && memcmp(data, "end", 3) == 0);
	CHECK(pread(fd, data, sizeof(data), 131075) == 0);
	CHECK(pread(fd, data, sizeof(data), INT64_C(1) << 33) == 0);
	CHECK(close(fd) == 0);
	CHECK(readlink("link", target, sizeof(target)) == 5 && memcmp(target, "hello", 5) == 0);
	CHECK(readlink("longlink", target, sizeof(target)) == 990);
	for (i = 0; i < 990; i++)
		CHECK(target[i] == '/');
	CHECK(readlink("longlink", target, 3) == 3);
	FAILS(stat("attributes", &st), EOPNOTSUPP);
	FAILS(stat("missing", &st), ENOENT);
	FAILS(stat("hello/child", &st), ENOTDIR);
	permissions(12, 34, true, true);
	permissions(99, 34, false, true);
	permissions(99, 99, false, false);
	directories();
	CHECK(statvfs(".", &vfs) == 0);
	CHECK((vfs.f_flag & (ST_RDONLY | ST_NOSUID | ST_NODEV)) == (ST_RDONLY | ST_NOSUID | ST_NODEV));
	CHECK(vfs.f_bsize == 4096 && vfs.f_bfree <= vfs.f_blocks && vfs.f_bavail == 0);
	CHECK(vfs.f_ffree <= vfs.f_files && vfs.f_favail == 0);
	/* NetBSD overrides the callback's 255 with libpuffs' mount-time
	 * MAXNAMLEN; its public API has no setter for that initial value. */
	CHECK(vfs.f_namemax == MAXNAMLEN);
	FAILS(open("new", O_CREAT | O_WRONLY, 0600), EROFS);
	FAILS(open("hello", O_WRONLY), EROFS);
	FAILS(chmod("hello", 0777), EROFS);
	FAILS(truncate("hello", 0), EROFS);
	FAILS(unlink("hello"), EROFS);
	FAILS(rename("hello", "renamed"), EROFS);
	FAILS(link("hello", "newlink"), EROFS);
	FAILS(symlink("hello", "newsymlink"), EROFS);
	FAILS(mkdir("newdir", 0700), EROFS);
	FAILS(rmdir("locked"), EROFS);
	/* Our cwd holds the mount busy; failure must leave the server usable. */
	FAILS(unmount(argv[1], 0), EBUSY);
	read_hello("hello", true);
	printf("Passed native PUFFS v%s: attributes, permissions, reads, hard links, directory cookies, read-only and busy unmount\n", argv[2]);
	return 0;
}
