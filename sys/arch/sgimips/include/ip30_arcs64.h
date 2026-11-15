/*	$NetBSD$	*/

/*-
 * Copyright (c) 2026 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by the Xiri project.
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

#ifndef _SGIMIPS_IP30_ARC64_H_
#define	_SGIMIPS_IP30_ARC64_H_

#include <sys/types.h>

#define	ARCBIOS64_GET_MEMORY_DESCRIPTOR	18
#define	ARCBIOS64_READ			25
#define	ARCBIOS64_WRITE			27
#define	ARCBIOS64_GET_ENVIRONMENT_VARIABLE 30

#define	ARCBIOS64_SPB_SIGNATURE		0x53435241U

struct arcs64_spb {
	uint64_t signature;
	uint64_t length;
	uint16_t version;
	uint16_t revision;
	uint64_t restart;
	uint64_t debug;
	uint64_t exception;
	uint64_t utlb;
	uint64_t vector_length;
	const uint64_t *vector;
};

struct arcs64_mem {
	uint32_t type;
	uint32_t pad;
	uint64_t base_page;
	uint64_t page_count;
};

#define	ARCBIOS64_SPB \
	((const volatile struct arcs64_spb *)0xffffffffa0001000UL)

const char *ip30_arcs64_getenv(const char *);
const struct arcs64_mem *ip30_arcs64_getmem(const struct arcs64_mem *);
void	ip30_arcs64_init(void);
void	ip30_arcs64_putc(int);
int	ip30_arcs64_getc(void);
void	ip30_arcs64_enter(void) __dead;
void	ip30_arcs64_reboot(void) __dead;
void	ip30_arcs64_powerdown(void) __dead;

#endif /* _SGIMIPS_IP30_ARC64_H_ */
