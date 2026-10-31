/*-
 * Copyright (c) 2001 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Emmanuel Dreyfus.
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

/* Fixed-width layouts adapted from historical NetBSD irix_types.h.
 * Measured IRIX: version 2 selects the small layout for o32, the wide layout
 * for n32. Version 3 selects the wide layout for either supported ABI. */
#ifndef _IRIX_O32_STAT_H_
#define _IRIX_O32_STAT_H_

struct irix_stat_time { int32_t sec, nsec; };
struct irix_stat_small {
	uint32_t dev, pad1[3], ino, mode, nlink;
	int32_t uid, gid;
	uint32_t rdev, pad2[2];
	int32_t size;
	uint32_t pad3;
	struct irix_stat_time atim, mtim, ctim;
	int32_t blksize, blocks;
	char fstype[16];
	int32_t projid;
	uint32_t pad4[7];
};
struct irix_stat_wide {
	uint32_t dev, pad1[3];
	uint64_t ino;
	uint32_t mode, nlink;
	int32_t uid, gid;
	uint32_t rdev, pad2[2];
	int64_t size;
	uint32_t pad3;
	struct irix_stat_time atim, mtim, ctim;
	int32_t blksize;
	int64_t blocks;
	char fstype[16];
	int32_t projid;
	uint32_t pad4[7];
};
#endif
