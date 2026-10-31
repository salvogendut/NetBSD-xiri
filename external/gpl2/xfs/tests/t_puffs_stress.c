/* SPDX-License-Identifier: GPL-2.0-only */
/* $NetBSD$ */
/* Native NetBSD lifecycle tests; use only disposable make_puffs_images fixtures. */
#include <sys/types.h>
#include <sys/extattr.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/wait.h>

#include <dirent.h>
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

static pid_t supervisor, server;
static const char *program, *image, *mountpoint, *logprefix;
static char logfile[1024];
static const char hello[] = "NetBSD XFS through PUFFS\n";

/* The supervisor owns a private process group. A timeout cannot leave a
 * stopped server or a blocked worker behind; the shell restores maxvnodes. */
static void
timeout_handler(int signo)
{
	static const char message[] = "PUFFS stress interrupted or timed out\n";
	(void)signo;
	(void)write(STDERR_FILENO, message, sizeof(message) - 1);
	(void)kill(0, SIGKILL);
	_exit(1);
}

static void
failed(int line, const char *test)
{
	fprintf(stderr, "pid %ld line %d: %s (errno %d: %s)\n",
	    (long)getpid(), line, test, errno, strerror(errno));
	if (getpid() == supervisor)
		(void)kill(0, SIGKILL);
	_exit(1);
}
#define CHECK(test) do { if (!(test)) failed(__LINE__, #test); } while (0)
#define FAILS(call, error) do { errno = 0; CHECK((call) == -1); \
    CHECK(errno == (error)); } while (0)

static pid_t
child(void)
{
	pid_t pid = fork();
	CHECK(pid != -1);
	if (pid == 0) {
		CHECK(signal(SIGTERM, SIG_DFL) != SIG_ERR);
		CHECK(signal(SIGINT, SIG_DFL) != SIG_ERR);
		CHECK(signal(SIGHUP, SIG_DFL) != SIG_ERR);
	}
	return pid;
}

static void
reap(pid_t pid, int signo)
{
	int status;
	CHECK(waitpid(pid, &status, 0) == pid);
	if (signo != 0)
		CHECK(WIFSIGNALED(status) && WTERMSIG(status) == signo);
	else
		CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
}

static int
open_file(const char *name)
{
	char path[1024];
	int fd;
	CHECK(snprintf(path, sizeof(path), "%s/%s", mountpoint, name) < (int)sizeof(path));
	fd = open(path, O_RDONLY | O_CLOEXEC);
	CHECK(fd != -1);
	return fd;
}

static void
check_hello(int fd)
{
	char data[sizeof(hello)];
	CHECK(pread(fd, data, sizeof(data), 0) == sizeof(hello) - 1);
	CHECK(memcmp(data, hello, sizeof(hello) - 1) == 0);
}

static void
check_attr(int fd)
{
	char data[16];
	CHECK(extattr_get_fd(fd, EXTATTR_NAMESPACE_USER, "fixture", data, sizeof(data)) == 9);
	CHECK(memcmp(data, "supported", 9) == 0);
}

static void
start_server(void)
{
	static unsigned int generation;
	char path[1024];
	struct stat st;
	int fd, status;
	unsigned int i;
	CHECK(snprintf(logfile, sizeof(logfile), "%s.%u", logprefix, ++generation) < (int)sizeof(logfile));
	server = child();
	if (server == 0) {
		fd = open(logfile, O_WRONLY | O_CREAT | O_TRUNC, 0600);
		CHECK(fd != -1);
		CHECK(dup2(fd, STDOUT_FILENO) == STDOUT_FILENO);
		CHECK(dup2(fd, STDERR_FILENO) == STDERR_FILENO);
		CHECK(close(fd) == 0);
		execl(program, program, "-d", "-o", "ro", image, mountpoint, (char *)NULL);
		failed(__LINE__, "exec mount_xfs");
	}
	CHECK(snprintf(path, sizeof(path), "%s/hello", mountpoint) < (int)sizeof(path));
	for (i = 0; i < 200; i++) {
		CHECK(waitpid(server, &status, WNOHANG) == 0);
		if (stat(path, &st) == 0)
			return;
		usleep(100000);
	}
	failed(__LINE__, "server ready within 20 seconds");
}

struct counts { size_t created, reclaimed, freed, allocated_acl, freed_acl; };

static struct counts
counts(uint64_t wanted)
{
	struct counts result = {0};
	FILE *file = fopen(logfile, "r");
	char *line = NULL, action[16];
	size_t capacity = 0, acl;
	uint64_t ino;
	CHECK(file != NULL);
	while (getline(&line, &capacity, file) != -1) {
		if (sscanf(line, "xfs-node %15s ino=%" SCNu64 " acl=%zu", action, &ino, &acl) != 3)
			continue;
		if (wanted != 0 && ino != wanted)
			continue;
		if (strcmp(action, "create") == 0) {
			result.created++;
			result.allocated_acl += acl;
		} else {
			CHECK(strcmp(action, "reclaim") == 0 || strcmp(action, "shutdown") == 0);
			result.freed++;
			result.freed_acl += acl;
			if (strcmp(action, "reclaim") == 0) result.reclaimed++;
		}
	}
	CHECK(!ferror(file));
	free(line);
	CHECK(fclose(file) == 0);
	return result;
}

static void
stop_server(void)
{
	struct counts c;
	CHECK(unmount(mountpoint, 0) == 0);
	reap(server, 0);
	c = counts(0);
	CHECK(c.created > 0 && c.created == c.freed);
	CHECK(c.allocated_acl == c.freed_acl);
	printf("Released %zu nodes (%zu kernel reclaims), %zu retained ACL bytes\n",
	    c.freed, c.reclaimed, c.freed_acl);
}

static void
acl_read(const char *name, bool allow, bool attribute)
{
	int fd;
	if (!allow) {
		FAILS(open(name, O_RDONLY), EACCES);
		FAILS(access(name, R_OK), EACCES);
		if (attribute)
			FAILS(extattr_get_file(name, EXTATTR_NAMESPACE_USER, "fixture", NULL, 0), EACCES);
		return;
	}
	CHECK(access(name, R_OK) == 0);
	fd = open(name, O_RDONLY);
	CHECK(fd != -1);
	check_hello(fd);
	if (attribute) check_attr(fd);
	CHECK(close(fd) == 0);
}

static void
parallel_clients(void)
{
	static const struct {
		uid_t uid;
		gid_t gid, group;
		bool allow, masked;
	} cases[] = {
		{12, 34, 0, true, true}, {99, 99, 0, true, false},
		{100, 77, 0, false, false}, {101, 34, 0, false, false},
		{101, 99, 77, true, false}, {101, 99, 0, true, true}
	};
	pid_t workers[__arraycount(cases)];
	int gate[2], ready[2], fd;
	unsigned int i, j, k;
	char token;
	CHECK(pipe(gate) == 0 && pipe(ready) == 0);
	for (i = 0; i < __arraycount(cases); i++) {
		workers[i] = child();
		if (workers[i] != 0) continue;
		CHECK(close(gate[1]) == 0 && close(ready[0]) == 0);
		CHECK(chdir(mountpoint) == 0);
		CHECK(setgroups(cases[i].group != 0 ? 1 : 0, &cases[i].group) == 0);
		CHECK(setgid(cases[i].gid) == 0 && setuid(cases[i].uid) == 0);
		CHECK(write(ready[1], "R", 1) == 1);
		CHECK(read(gate[0], &token, 1) == 1);
		CHECK(close(gate[0]) == 0 && close(ready[1]) == 0);
		for (j = 0; j < 12; j++) {
			unsigned char data[8192];
			char next[256];
			struct dirent *entry;
			DIR *dir;
			long pos;
			off_t offset = (off_t)(i * 193 + j * 997);
			fd = open("public", O_RDONLY);
			CHECK(fd != -1);
			CHECK(pread(fd, data, sizeof(data), offset) == sizeof(data));
			for (k = 0; k < sizeof(data); k++) CHECK(data[k] == (unsigned char)(offset + k));
			CHECK(pread(fd, data, sizeof(data), 131072) == 3 && memcmp(data, "end", 3) == 0);
			CHECK(close(fd) == 0);
			acl_read("acls/access", cases[i].allow, true);
			acl_read("acls/external", cases[i].allow, true);
			acl_read("acls/large", cases[i].allow, true);
			acl_read("acls/masked", cases[i].masked, true);
			acl_read("acls/search/file", cases[i].allow, false);
			if (cases[i].allow) {
				CHECK(extattr_get_file("acls/external", EXTATTR_NAMESPACE_USER,
				    "big", data, 97 + i) == 97 + i);
				for (k = 0; k < 97 + i; k++) CHECK(data[k] == 'B');
			} else {
				FAILS(extattr_get_file("acls/external", EXTATTR_NAMESPACE_USER,
				    "big", data, 97 + i), EACCES);
			}
			dir = opendir("many");
			CHECK(dir != NULL);
			for (k = 0; k < 20 + i; k++) CHECK(readdir(dir) != NULL);
			pos = telldir(dir);
			CHECK(pos != -1);
			entry = readdir(dir);
			CHECK(entry != NULL);
			CHECK(strlcpy(next, entry->d_name, sizeof(next)) < sizeof(next));
			seekdir(dir, pos);
			entry = readdir(dir);
			CHECK(entry != NULL && strcmp(next, entry->d_name) == 0);
			CHECK(closedir(dir) == 0);
		}
		_exit(0);
	}
	CHECK(close(gate[0]) == 0 && close(ready[1]) == 0);
	for (i = 0; i < __arraycount(cases); i++) CHECK(read(ready[0], &token, 1) == 1);
	for (i = 0; i < __arraycount(cases); i++) CHECK(write(gate[1], "G", 1) == 1);
	CHECK(close(gate[1]) == 0 && close(ready[0]) == 0);
	for (i = 0; i < __arraycount(cases); i++) reap(workers[i], 0);
	puts("Passed six simultaneous credential/ACL, read, attribute and directory clients");
}

static void
churn(bool pressure)
{
	ino_t inos[1800], pinned_ino, closed_ino;
	struct stat st;
	struct counts before, after;
	size_t closed_reclaims;
	char path[64];
	unsigned int pass, i;
	int pinned = open_file("acls/large"), fd;
	CHECK(fstat(pinned, &st) == 0);
	pinned_ino = st.st_ino;
	before = counts(pinned_ino);
	fd = open_file("acls/access");
	CHECK(fstat(fd, &st) == 0);
	closed_ino = st.st_ino;
	closed_reclaims = counts(closed_ino).reclaimed;
	CHECK(close(fd) == 0);
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < __arraycount(inos); i++) {
			CHECK(snprintf(path, sizeof(path), "many/f%04u", i) < (int)sizeof(path));
			fd = open_file(path);
			CHECK(fstat(fd, &st) == 0);
			if (pass == 0) inos[i] = st.st_ino;
			else CHECK(inos[i] == st.st_ino);
			check_hello(fd);
			CHECK(close(fd) == 0);
		}
		if (pass == 0) {
			check_hello(pinned);
			check_attr(pinned);
			CHECK(counts(pinned_ino).reclaimed == before.reclaimed);
			if (pressure) CHECK(counts(closed_ino).reclaimed > closed_reclaims);
			CHECK(close(pinned) == 0);
		}
	}
	after = counts(pinned_ino);
	if (pressure) {
		CHECK(after.reclaimed > before.reclaimed);
		CHECK(after.freed_acl > before.freed_acl);
	}
	fd = open_file("acls/large");
	CHECK(fstat(fd, &st) == 0 && st.st_ino == pinned_ino);
	check_hello(fd);
	check_attr(fd);
	CHECK(close(fd) == 0);
	if (pressure) CHECK(counts(pinned_ino).created > before.created);
	printf("Passed 3600 inode visits, pinned ACL lifetime%s\n",
	    pressure ? " and observed kernel reclamation/reload" : " (no forced vnode pressure)");
}

/* Observe the kernel wait channel, rather than assuming a sleep means a
 * request was submitted. These children each have exactly one LWP. */
static void
await_request(pid_t pid)
{
	struct kinfo_proc2 info;
	int mib[] = {CTL_KERN, KERN_PROC2, KERN_PROC_PID, pid, sizeof(info), 1};
	size_t length;
	unsigned int i;
	for (i = 0; i < 1000; i++) {
		length = sizeof(info);
		CHECK(sysctl(mib, __arraycount(mib), &info, &length, NULL, 0) == 0);
		CHECK(length == sizeof(info));
		/* A full KI_WMESGLEN-byte wait message has no terminator. */
		if (memcmp(info.p_wmesg, "puffsrpl", sizeof(info.p_wmesg)) == 0) return;
		usleep(10000);
	}
	failed(__LINE__, "client waiting for PUFFS reply within 10 seconds");
}

static void
pause_server(void)
{
	int status;
	CHECK(kill(server, SIGSTOP) == 0);
	CHECK(waitpid(server, &status, WUNTRACED) == server);
	CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
}

static void
server_death(void)
{
	pid_t workers[3], cancelled;
	int datafd = open_file("public"), attrfd = open_file("acls/large");
	int dirfd = open_file("many");
	unsigned int i;
	char data[4096];
	pause_server();
	cancelled = child();
	if (cancelled == 0) {
		(void)pread(datafd, data, sizeof(data), 0);
		failed(__LINE__, "cancelled read returned before signal");
	}
	await_request(cancelled);
	CHECK(kill(cancelled, SIGTERM) == 0);
	reap(cancelled, SIGTERM);
	CHECK(kill(server, SIGCONT) == 0);
	CHECK(pread(datafd, data, sizeof(data), 0) == sizeof(data));
	check_attr(attrfd);
	puts("Passed cancellation of a confirmed pending request and continued service");
	pause_server();
	for (i = 0; i < __arraycount(workers); i++) {
		workers[i] = child();
		if (workers[i] == 0) {
			switch (i) {
			case 0: FAILS(pread(datafd, data, sizeof(data), 0), ENXIO); break;
			case 1: FAILS(extattr_get_fd(attrfd, EXTATTR_NAMESPACE_USER,
			    "fixture", data, sizeof(data)), ENXIO); break;
			case 2: FAILS(getdents(dirfd, data, sizeof(data)), ENXIO); break;
			}
			_exit(0);
		}
		await_request(workers[i]);
	}
	CHECK(kill(server, SIGKILL) == 0);
	reap(server, SIGKILL);
	for (i = 0; i < __arraycount(workers); i++) reap(workers[i], 0);
	CHECK(close(datafd) == 0 && close(attrfd) == 0 && close(dirfd) == 0);
	/* Closing the PUFFS device may already have detached the dead mount. */
	if (unmount(mountpoint, MNT_FORCE) == -1) CHECK(errno == EINVAL || errno == ENOENT);
	puts("Passed server SIGKILL with three confirmed pending operations returning ENXIO");
}

int
main(int argc, char **argv)
{
	bool pressure;
	int fd;
	if (argc != 6 || geteuid() != 0) {
		fprintf(stderr, "usage (root): t_puffs_stress SERVER IMAGE MOUNT LOG PRESSURE(0|1)\n");
		return 2;
	}
	if (setpgid(0, 0) == -1) { perror("private process group"); return 1; }
	supervisor = getpid();
	CHECK(getpgrp() == supervisor);
	CHECK(signal(SIGALRM, timeout_handler) != SIG_ERR);
	CHECK(signal(SIGTERM, timeout_handler) != SIG_ERR);
	CHECK(signal(SIGINT, timeout_handler) != SIG_ERR);
	CHECK(signal(SIGHUP, timeout_handler) != SIG_ERR);
	alarm(240);
	CHECK(setvbuf(stdout, NULL, _IOLBF, 0) == 0);
	program = argv[1]; image = argv[2]; mountpoint = argv[3]; logprefix = argv[4];
	CHECK(strcmp(argv[5], "0") == 0 || strcmp(argv[5], "1") == 0);
	pressure = strcmp(argv[5], "1") == 0;
	start_server();
	parallel_clients();
	churn(pressure);
	stop_server();
	puts("Passed graceful unmount with balanced node and retained ACL lifetimes");
	start_server();
	server_death();
	start_server();
	fd = open_file("hello");
	check_hello(fd);
	CHECK(close(fd) == 0);
	parallel_clients();
	stop_server();
	puts("Passed fresh mount and concurrent access after server death");
	alarm(0);
	return 0;
}
