/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
/* PID 1 in the disposable Linux fixture VM; never run on a host filesystem. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <linux/fiemap.h>
#include <linux/fs.h>
#include <linux/posix_acl_xattr.h>

#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <inttypes.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void
failed(int line, const char *test)
{
	fprintf(stderr, "XFS_WORKLOAD_ERROR line %d: %s: %s\n", line, test, strerror(errno));
	for (;;) pause(); /* The supervisor kills this VM and preserves diagnostics. */
}
#define CHECK(test) do { if (!(test)) failed(__LINE__, #test); } while (0)

static int
create(const char *path, mode_t mode)
{
	int fd = open(path, O_RDWR | O_CREAT | O_EXCL, mode);
	CHECK(fd != -1);
	CHECK(fchown(fd, 12, 34) == 0);
	return fd;
}

static void
fill(int fd, off_t offset, size_t length, unsigned int seed)
{
	unsigned char data[4096];
	size_t n, i;
	while (length != 0) {
		n = length < sizeof(data) ? length : sizeof(data);
		for (i = 0; i < n; i++) data[i] = (unsigned char)((offset + i) ^ ((offset + i) >> 12) ^ seed);
		CHECK(pwrite(fd, data, n, offset) == (ssize_t)n);
		offset += n; length -= n;
	}
}

static void
finish(int fd)
{
	struct timespec times[2] = {{1600000000, 123456789}, {1600000001, 987654321}};
	CHECK(futimens(fd, times) == 0);
	CHECK(fsync(fd) == 0);
	CHECK(close(fd) == 0);
}

static void
acl(const char *path, bool directory, bool default_acl)
{
	struct {
		struct posix_acl_xattr_header header;
		struct posix_acl_xattr_entry entries[7];
	} value;
	const unsigned int tags[] = {1, 2, 2, 4, 8, 16, 32};
	const unsigned int ids[] = {UINT32_MAX, 99, 100, UINT32_MAX, 77, UINT32_MAX, UINT32_MAX};
	unsigned int perms[] = {6, 4, 0, 0, 4, 4, 4};
	unsigned int i;
	if (directory) { perms[0] = 7; perms[1] = perms[4] = perms[5] = perms[6] = 5; }
	if (default_acl) perms[6] = 0;
	memset(&value, 0, sizeof(value));
	value.header.a_version = htole32(POSIX_ACL_XATTR_VERSION);
	for (i = 0; i < 7; i++) {
		value.entries[i].e_tag = htole16(tags[i]);
		value.entries[i].e_perm = htole16(perms[i]);
		value.entries[i].e_id = htole32(ids[i]);
	}
	CHECK(setxattr(path, default_acl ? "system.posix_acl_default" :
	    "system.posix_acl_access", &value, sizeof(value), 0) == 0);
}

static void
sha256(const char *path, char *hash)
{
	char output[1024];
	int pipefd[2], status;
	ssize_t n;
	pid_t pid;
	CHECK(pipe(pipefd) == 0);
	pid = fork(); CHECK(pid != -1);
	if (pid == 0) {
		CHECK(close(pipefd[0]) == 0);
		CHECK(dup2(pipefd[1], STDOUT_FILENO) == STDOUT_FILENO);
		execl("/sha256sum", "sha256sum", path, (char *)NULL);
		_exit(127);
	}
	CHECK(close(pipefd[1]) == 0);
	n = read(pipefd[0], output, sizeof(output));
	CHECK(n >= 65 && output[64] == ' ');
	memcpy(hash, output, 64); hash[64] = 0;
	CHECK(close(pipefd[0]) == 0);
	CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static void
reference(const char *path)
{
	struct stat st;
	char hash[65];
	unsigned char value[8192];
	static const char *names[] = {"user.fixture", "user.large", "system.posix_acl_access", "system.posix_acl_default"};
	ssize_t n, i;
	unsigned int j;
	CHECK(lstat(path, &st) == 0);
	if (S_ISREG(st.st_mode)) sha256(path, hash); else strcpy(hash, "-");
	printf("XFSREF {\"type\":\"file\",\"path\":\"%s\",\"ino\":%ju,\"mode\":%u,"
	    "\"uid\":%u,\"gid\":%u,\"size\":%jd,\"nlink\":%ju,\"allocated_bytes\":%jd,"
	    "\"mtime_sec\":%jd,\"mtime_nsec\":%ld,\"sha256\":\"%s\"}\n", path,
	    (uintmax_t)st.st_ino, st.st_mode, st.st_uid, st.st_gid, (intmax_t)st.st_size,
	    (uintmax_t)st.st_nlink, (intmax_t)st.st_blocks * 512,
	    (intmax_t)st.st_mtim.tv_sec, st.st_mtim.tv_nsec, hash);
	for (j = 0; j < sizeof(names) / sizeof(names[0]); j++) {
		n = getxattr(path, names[j], value, sizeof(value));
		if (n == -1) { CHECK(errno == ENODATA); continue; }
		printf("XFSREF {\"type\":\"attr\",\"path\":\"%s\",\"name\":\"%s\",\"value\":\"", path, names[j]);
		for (i = 0; i < n; i++) printf("%02x", value[i]);
		puts("\"}");
	}
}

static void
mapping(const char *path)
{
	struct fiemap *map = calloc(1, sizeof(*map) + 256 * sizeof(struct fiemap_extent));
	unsigned int i;
	int fd = open(path, O_RDONLY);
	CHECK(map != NULL && fd != -1);
	map->fm_length = UINT64_MAX; map->fm_flags = FIEMAP_FLAG_SYNC; map->fm_extent_count = 256;
	CHECK(ioctl(fd, FS_IOC_FIEMAP, map) == 0 && map->fm_mapped_extents < 256);
	printf("XFSREF {\"type\":\"fiemap\",\"path\":\"%s\",\"extents\":[", path);
	for (i = 0; i < map->fm_mapped_extents; i++) {
		struct fiemap_extent *e = &map->fm_extents[i];
		printf("%s{\"logical\":%llu,\"physical\":%llu,\"length\":%llu,\"flags\":%u}",
		    i ? "," : "", e->fe_logical, e->fe_physical, e->fe_length, e->fe_flags);
	}
	puts("]}");
	CHECK(close(fd) == 0); free(map);
}

static void
permissions(void)
{
	static const unsigned int cases[][3] = {{12,34,0}, {99,99,0}, {100,77,0}, {101,34,0}, {101,99,77}, {101,99,0}};
	static const char *paths[] = {"acl.txt", "mode.txt", "search/child", "defaults/child"};
	unsigned int i, j;
	int status, fd;
	pid_t pid;
	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		pid = fork(); CHECK(pid != -1);
		if (pid == 0) {
			gid_t group = cases[i][2];
			CHECK(setgroups(group ? 1 : 0, &group) == 0);
			CHECK(setgid(cases[i][1]) == 0 && setuid(cases[i][0]) == 0);
			for (j = 0; j < sizeof(paths) / sizeof(paths[0]); j++) {
				fd = open(paths[j], O_RDONLY);
				CHECK(fd != -1 || errno == EACCES);
				CHECK((access(paths[j], R_OK) == 0) == (fd != -1));
				printf("XFSREF {\"type\":\"access\",\"uid\":%u,\"gid\":%u,\"group\":%u,"
				    "\"path\":\"%s\",\"allowed\":%d}\n", cases[i][0], cases[i][1], group, paths[j], fd != -1);
				if (fd != -1) CHECK(close(fd) == 0);
			}
			_exit(0);
		}
		CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
	}
}

static void
workload(void)
{
	static const char *files[] = {"plain.bin", "sparse.bin", "prealloc.bin", "fragmented.bin",
	    "acl.txt", "mode.txt", "alias", "search/child", "defaults/child", "search", "defaults", "many"};
	unsigned char value[8192];
	char path[64];
	unsigned int i;
	int fd;
	fd = create("plain.bin", 0644); fill(fd, 0, 131075, 17); finish(fd);
	CHECK(link("plain.bin", "alias") == 0);
	CHECK(symlink("plain.bin", "link") == 0);
	fd = create("sparse.bin", 0644);
	CHECK(ftruncate(fd, 64 * 1024 * 1024 + 123) == 0);
	fill(fd, 0, 4096, 33); fill(fd, 4 * 1024 * 1024 + 17, 8192, 44);
	fill(fd, 64 * 1024 * 1024, 123, 55); finish(fd);
	fd = create("prealloc.bin", 0644);
	CHECK(fallocate(fd, 0, 0, 4 * 1024 * 1024) == 0);
	fill(fd, 65536, 4096, 66); fill(fd, 1024 * 1024, 8192, 77); finish(fd);
	fd = create("fragmented.bin", 0644);
	fill(fd, 0, 1024 * 1024, 88); CHECK(fsync(fd) == 0);
	CHECK(fallocate(fd, FALLOC_FL_INSERT_RANGE, 65536, 65536) == 0);
	fill(fd, 65536, 65536, 99); finish(fd);
	fd = create("acl.txt", 0644); fill(fd, 0, 1234, 111); finish(fd);
	acl("acl.txt", false, false);
	CHECK(setxattr("acl.txt", "user.fixture", "linux-xfs", 9, 0) == 0);
	memset(value, 'L', sizeof(value));
	CHECK(setxattr("acl.txt", "user.large", value, sizeof(value), 0) == 0);
	fd = create("mode.txt", 0640); fill(fd, 0, 123, 122); finish(fd);
	CHECK(mkdir("search", 0755) == 0 && chown("search", 12, 34) == 0);
	acl("search", true, false);
	fd = create("search/child", 0644); fill(fd, 0, 123, 133); finish(fd);
	CHECK(mkdir("defaults", 0755) == 0 && chown("defaults", 12, 34) == 0);
	acl("defaults", true, true);
	fd = create("defaults/child", 0644); fill(fd, 0, 123, 144); finish(fd);
	CHECK(mkdir("many", 0755) == 0);
	for (i = 0; i < 768; i++) {
		CHECK(snprintf(path, sizeof(path), "many/f%04u", i) < (int)sizeof(path));
		fd = create(path, 0644); fill(fd, 0, 17, i); finish(fd);
	}
	for (i = 0; i < 768; i += 4) {
		CHECK(snprintf(path, sizeof(path), "many/f%04u", i) < (int)sizeof(path));
		CHECK(unlink(path) == 0);
	}
	fd = open(".", O_RDONLY | O_DIRECTORY);
	CHECK(fd != -1 && syncfs(fd) == 0 && close(fd) == 0);
	for (i = 0; i < 4; i++) mapping(files[i]);
	for (i = 0; i < sizeof(files) / sizeof(files[0]); i++) reference(files[i]);
	for (i = 1; i < 768; i += 113) {
		if (i % 4 == 0) continue;
		CHECK(snprintf(path, sizeof(path), "many/f%04u", i) < (int)sizeof(path));
		reference(path);
	}
	permissions();
}

int
main(void)
{
	FILE *file;
	char line[4096];
	bool dirty;
	struct utsname uts;
	int fd, i;
	if (getpid() != 1) { fputs("linux_workload is only for PID 1 in its fixture VM\n", stderr); return 2; }
	CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	CHECK(mount("proc", "/proc", "proc", 0, NULL) == 0);
	file = fopen("/proc/cmdline", "r"); CHECK(file != NULL && fgets(line, sizeof(line), file) != NULL);
	CHECK(strstr(line, "xfs_fixture=") != NULL); CHECK(fclose(file) == 0);
	dirty = strstr(line, "xfs_fixture=dirty") != NULL;
	CHECK(mount("sysfs", "/sys", "sysfs", 0, NULL) == 0);
	CHECK(mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) == 0);
	file = fopen("/modules.list", "r"); CHECK(file != NULL);
	while (fgets(line, sizeof(line), file)) {
		line[strcspn(line, "\n")] = 0;
		fd = open(line, O_RDONLY); CHECK(fd != -1);
		CHECK(syscall(SYS_finit_module, fd, "", 0) == 0 || errno == EEXIST);
		CHECK(close(fd) == 0);
	}
	CHECK(fclose(file) == 0);
	for (i = 0; i < 100 && access("/dev/vda", F_OK) != 0; i++) usleep(100000);
	CHECK(mount("/dev/vda", "/mnt", "xfs", MS_NOATIME | MS_NODIRATIME, "nouuid") == 0);
	CHECK(chdir("/mnt") == 0 && uname(&uts) == 0);
	printf("XFSREF {\"type\":\"kernel\",\"release\":\"%s\",\"machine\":\"%s\"}\n", uts.release, uts.machine);
	if (dirty) {
		fd = create("crash-transaction", 0644);
		fill(fd, 0, 8192, 155);
		CHECK(fsync(fd) == 0);
		CHECK(rename("crash-transaction", "crash-renamed") == 0);
		CHECK(setxattr("crash-renamed", "user.crash", "journal", 7, 0) == 0);
		CHECK(fsync(fd) == 0);
		puts("XFS_WORKLOAD_DIRTY_READY");
		for (;;) pause();
	}
	workload();
	CHECK(chdir("/") == 0 && umount("/mnt") == 0);
	puts("XFS_WORKLOAD_CLEAN");
	CHECK(reboot(RB_POWER_OFF) == 0);
	return 0;
}
