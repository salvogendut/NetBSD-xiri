/* $NetBSD: t_wait.c,v 1.10 2021/07/17 14:03:35 martin Exp $ */

/*-
 * Copyright (c) 2016 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Christos Zoulas.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE NETBSD FOUNDATION, INC. AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */
#include <sys/cdefs.h>
__RCSID("$NetBSD: t_wait.c,v 1.10 2021/07/17 14:03:35 martin Exp $");

#include <sys/wait.h>
#include <sys/resource.h>

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <atf-c.h>

ATF_TC(wait6_invalid);
ATF_TC_HEAD(wait6_invalid, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that wait6(2) returns EINVAL with 0 options");
}

ATF_TC_BODY(wait6_invalid, tc)
{
	siginfo_t si;
	struct wrusage wru;
	int st;
	ATF_REQUIRE(wait6(P_ALL, 0, &st, 0, &wru, &si) == -1
	    && errno == EINVAL);
}

ATF_TC(wait6_exited);
ATF_TC_HEAD(wait6_exited, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that wait6(2) handled exiting process and code");
}

ATF_TC_BODY(wait6_exited, tc)
{
	siginfo_t si;
	struct wrusage wru;
	int st;
	pid_t pid;

	switch (pid = fork()) {
	case 0:
		exit(0x5a5a5a5a);
		/*NOTREACHED*/
	case -1:
		ATF_REQUIRE(pid > 0);
		__unreachable();
	default:
		ATF_REQUIRE(wait6(P_PID, pid, &st, WEXITED, &wru, &si) == pid);
		ATF_REQUIRE(WIFEXITED(st) && WEXITSTATUS(st) == 0x5a);
		ATF_REQUIRE(si.si_status == 0x5a5a5a5a);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_EXITED);
		printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
		    (uintmax_t)si.si_utime);
		break;
	}
}

ATF_TC(wait6_terminated);
ATF_TC_HEAD(wait6_terminated, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that wait6(2) handled terminated process and code");
}

ATF_TC_BODY(wait6_terminated, tc)
{
	siginfo_t si;
	struct wrusage wru;
	int st;
	pid_t pid;

	switch (pid = fork()) {
	case 0:
		sleep(100);
		/*FALLTHROUGH*/
	case -1:
		ATF_REQUIRE(pid > 0);
		__unreachable();
	default:
		ATF_REQUIRE(kill(pid, SIGTERM) == 0);
		ATF_REQUIRE(wait6(P_PID, pid, &st, WEXITED, &wru, &si) == pid);
		ATF_REQUIRE(WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM);
		ATF_REQUIRE(si.si_status == SIGTERM);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_KILLED);
		printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
		    (uintmax_t)si.si_utime);
		break;
	}
}

ATF_TC(wait6_coredumped);
ATF_TC_HEAD(wait6_coredumped, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that wait6(2) handled coredumped process and code");
}

ATF_TC_BODY(wait6_coredumped, tc)
{
	siginfo_t si;
	struct wrusage wru;
	int st;
	pid_t pid;
	static const struct rlimit rl = { RLIM_INFINITY, RLIM_INFINITY };

	switch (pid = fork()) {
	case 0:
		ATF_REQUIRE(setrlimit(RLIMIT_CORE, &rl) == 0);
		*(char *)8 = 0;
		/*FALLTHROUGH*/
	case -1:
		ATF_REQUIRE(pid > 0);
		__unreachable();
	default:
		ATF_REQUIRE(wait6(P_PID, pid, &st, WEXITED, &wru, &si) == pid);
		ATF_REQUIRE(WIFSIGNALED(st) && WTERMSIG(st) == SIGSEGV
		    && WCOREDUMP(st));
		ATF_REQUIRE(si.si_status == SIGSEGV);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_DUMPED);
		printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
		    (uintmax_t)si.si_utime);
		break;
	}
}

ATF_TC(wait6_stop_and_go);
ATF_TC_HEAD(wait6_stop_and_go, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that wait6(2) handled stopped/continued process and code");
}

ATF_TC_BODY(wait6_stop_and_go, tc)
{
	siginfo_t si;
	struct wrusage wru;
	int st;
	pid_t pid;
	static const struct rlimit rl = { 0, 0 };

	ATF_REQUIRE(setrlimit(RLIMIT_CORE, &rl) == 0);
	switch (pid = fork()) {
	case 0:
		sleep(100);
		/*FALLTHROUGH*/
	case -1:
		ATF_REQUIRE(pid > 0);
		__unreachable();
	default:
		ATF_REQUIRE(kill(pid, SIGSTOP) == 0);
		ATF_REQUIRE(wait6(P_PID, pid, &st, WSTOPPED, &wru, &si) == pid);
		ATF_REQUIRE(!WIFEXITED(st));
		ATF_REQUIRE(!WIFSIGNALED(st));
		ATF_REQUIRE(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
		ATF_REQUIRE(!WIFCONTINUED(st));
		ATF_REQUIRE(si.si_status == SIGSTOP);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_STOPPED);
		printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
		    (uintmax_t)si.si_utime);

		ATF_REQUIRE(kill(pid, SIGCONT) == 0);
		ATF_REQUIRE(wait6(P_PID, pid, &st, WCONTINUED, &wru, &si) == pid);
		ATF_REQUIRE(!WIFEXITED(st));
		ATF_REQUIRE(!WIFSIGNALED(st));
		ATF_REQUIRE(WIFCONTINUED(st));
		ATF_REQUIRE(!WIFSTOPPED(st));
		ATF_REQUIRE(si.si_status == SIGCONT);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_CONTINUED);
		printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
		    (uintmax_t)si.si_utime);

		ATF_REQUIRE(kill(pid, SIGQUIT) == 0);
		ATF_REQUIRE(wait6(P_PID, pid, &st, WEXITED, &wru, &si) == pid);
		ATF_REQUIRE(!WIFEXITED(st));
		ATF_REQUIRE(WIFSIGNALED(st) && WTERMSIG(st) == SIGQUIT);
		ATF_REQUIRE(!WIFSTOPPED(st));
		ATF_REQUIRE(!WIFCONTINUED(st));
		ATF_REQUIRE(si.si_status == SIGQUIT);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_KILLED);
		printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
		    (uintmax_t)si.si_utime);
		break;
	}
}

ATF_TC(wait6_stopgo_loop);
ATF_TC_HEAD(wait6_stopgo_loop, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Test that wait6(2) handled stopped/continued process loop");
}

ATF_TC_BODY(wait6_stopgo_loop, tc)
{
	siginfo_t si;
	struct wrusage wru;
	int st;
	pid_t pid;
	static const struct rlimit rl = { 0, 0 };
	size_t N = 100;

	ATF_REQUIRE(setrlimit(RLIMIT_CORE, &rl) == 0);
	switch (pid = fork()) {
	case 0:
		sleep(100);
		/*FALLTHROUGH*/
	case -1:
		ATF_REQUIRE(pid > 0);
		__unreachable();
	}

	printf("Before loop of SIGSTOP/SIGCONT sequence %zu times\n", N);
	while (N --> 0) {
		ATF_REQUIRE(kill(pid, SIGSTOP) == 0);
		ATF_REQUIRE(wait6(P_PID, pid, &st, WSTOPPED, &wru, &si) == pid);
		ATF_REQUIRE(!WIFEXITED(st));
		ATF_REQUIRE(!WIFSIGNALED(st));
		ATF_REQUIRE(WIFSTOPPED(st) && WSTOPSIG(st) == SIGSTOP);
		ATF_REQUIRE(!WIFCONTINUED(st));
		ATF_REQUIRE(si.si_status == SIGSTOP);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_STOPPED);

		ATF_REQUIRE(kill(pid, SIGCONT) == 0);
		ATF_REQUIRE(wait6(P_PID, pid, &st, WCONTINUED, &wru, &si) == pid);
		ATF_REQUIRE(!WIFEXITED(st));
		ATF_REQUIRE(!WIFSIGNALED(st));
		ATF_REQUIRE(WIFCONTINUED(st));
		ATF_REQUIRE(!WIFSTOPPED(st));
		ATF_REQUIRE(si.si_status == SIGCONT);
		ATF_REQUIRE(si.si_pid == pid);
		ATF_REQUIRE(si.si_uid == getuid());
		ATF_REQUIRE(si.si_code == CLD_CONTINUED);
	}
	ATF_REQUIRE(kill(pid, SIGQUIT) == 0);
	ATF_REQUIRE(wait6(P_PID, pid, &st, WEXITED, &wru, &si) == pid);
	ATF_REQUIRE(!WIFEXITED(st));
	ATF_REQUIRE(WIFSIGNALED(st) && WTERMSIG(st) == SIGQUIT);
	ATF_REQUIRE(!WIFSTOPPED(st));
	ATF_REQUIRE(!WIFCONTINUED(st));
	ATF_REQUIRE(si.si_status == SIGQUIT);
	ATF_REQUIRE(si.si_pid == pid);
	ATF_REQUIRE(si.si_uid == getuid());
	ATF_REQUIRE(si.si_code == CLD_KILLED);
	printf("user: %ju system: %ju\n", (uintmax_t)si.si_utime,
	    (uintmax_t)si.si_utime);
}


/* Keep another live child at the head of the list: single-child waits do not
 * exercise the scan shortcut or expose damaged stopped-child accounting. */
static pid_t wait_children[3];

static void
wait_children_cleanup(void)
{
	unsigned i;
	int st;

	for (i = 0; i < __arraycount(wait_children); i++)
		if (wait_children[i] > 0)
			(void)kill(wait_children[i], SIGKILL);
	for (i = 0; i < __arraycount(wait_children); i++) {
		if (wait_children[i] <= 0)
			continue;
		while (waitpid(wait_children[i], &st, 0) == -1 && errno == EINTR)
			continue;
		wait_children[i] = 0;
	}
}

static void
wait_children_start(void)
{
	unsigned i;

	ATF_REQUIRE(atexit(wait_children_cleanup) == 0);
	for (i = 0; i < __arraycount(wait_children); i++) {
		pid_t pid = fork();
		ATF_REQUIRE(pid != -1);
		if (pid == 0) {
			alarm(20);
			for (;;)
				pause();
		}
		wait_children[i] = pid;
	}
}

static void
wait_child_event(pid_t pid, int options, int code)
{
	siginfo_t si;
	int st;

	ATF_REQUIRE(wait6(P_PID, pid, &st, options, NULL, &si) == pid);
	ATF_REQUIRE_EQ(si.si_pid, pid);
	ATF_REQUIRE_EQ(si.si_code, code);
}

static void
wait_child_scan(pid_t pid, int options, int code)
{
	siginfo_t si;
	int st;
	idtype_t types[] = { P_ALL, P_PGID };
	unsigned i;

	for (i = 0; i < __arraycount(types); i++) {
		pid_t found = wait6(types[i], types[i] == P_ALL ? 0 : getpgrp(),
		    &st, options | WNOHANG | WNOWAIT, NULL, &si);
		ATF_CHECK_EQ_MSG(found, pid, "selector %u options %#x", i, options);
		if (found > 0) {
			ATF_CHECK_EQ(si.si_pid, pid);
			ATF_CHECK_EQ(si.si_code, code);
		}
	}
}

ATF_TC(wait6_continued_multiple);
ATF_TC_HEAD(wait6_continued_multiple, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Find continued children behind live siblings, including WNOWAIT");
	atf_tc_set_md_var(tc, "timeout", "15");
}

ATF_TC_BODY(wait6_continued_multiple, tc)
{
	unsigned i;
	pid_t pid;

	wait_children_start();
	pid = wait_children[0];
	for (i = 0; i < 4; i++) {
		ATF_REQUIRE(kill(pid, SIGSTOP) == 0);
		/* Both collected and uncollected stops must balance on resume. */
		wait_child_event(pid, WSTOPPED | (i % 2 ? WNOWAIT : 0), CLD_STOPPED);
		ATF_REQUIRE(kill(pid, SIGCONT) == 0);
		wait_child_event(pid, WCONTINUED | WNOWAIT, CLD_CONTINUED);
		wait_child_scan(pid, WCONTINUED, CLD_CONTINUED);
		wait_child_scan(pid, WCONTINUED, CLD_CONTINUED);
		wait_child_event(pid, WCONTINUED, CLD_CONTINUED);
		wait_child_scan(0, WCONTINUED, 0);
	}
	wait_children_cleanup();
}

ATF_TC(wait6_stop_exit_after_continue);
ATF_TC_HEAD(wait6_stop_exit_after_continue, tc)
{
	atf_tc_set_md_var(tc, "descr",
	    "Consuming a continuation must not hide another child's stop or exit");
	atf_tc_set_md_var(tc, "timeout", "15");
}

ATF_TC_BODY(wait6_stop_exit_after_continue, tc)
{
	pid_t first, second;

	wait_children_start();
	first = wait_children[0];
	second = wait_children[1];
	ATF_REQUIRE(kill(first, SIGSTOP) == 0);
	wait_child_event(first, WSTOPPED, CLD_STOPPED);
	ATF_REQUIRE(kill(first, SIGCONT) == 0);
	wait_child_event(first, WCONTINUED, CLD_CONTINUED);

	ATF_REQUIRE(kill(second, SIGSTOP) == 0);
	wait_child_event(second, WSTOPPED | WNOWAIT, CLD_STOPPED);
	wait_child_scan(second, WSTOPPED, CLD_STOPPED);
	wait_child_event(second, WSTOPPED, CLD_STOPPED);
	wait_child_scan(0, WSTOPPED, 0);

	ATF_REQUIRE(kill(first, SIGKILL) == 0);
	wait_child_event(first, WEXITED | WNOWAIT, CLD_KILLED);
	wait_child_scan(first, WEXITED, CLD_KILLED);
	wait_child_event(first, WEXITED, CLD_KILLED);
	wait_children[0] = 0;
	wait_children_cleanup();
}

ATF_TP_ADD_TCS(tp)
{

	ATF_TP_ADD_TC(tp, wait6_invalid);
	ATF_TP_ADD_TC(tp, wait6_exited);
	ATF_TP_ADD_TC(tp, wait6_terminated);
	ATF_TP_ADD_TC(tp, wait6_coredumped);
	ATF_TP_ADD_TC(tp, wait6_stop_and_go);
	ATF_TP_ADD_TC(tp, wait6_stopgo_loop);
	ATF_TP_ADD_TC(tp, wait6_continued_multiple);
	ATF_TP_ADD_TC(tp, wait6_stop_exit_after_continue);

	return atf_no_error();
}
