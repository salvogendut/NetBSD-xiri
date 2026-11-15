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

#ifndef _SGIMIPS_XIRI_IP30_H_
#define	_SGIMIPS_XIRI_IP30_H_

/*
 * Octane/IP30 early-entry definitions.  The kernel is linked in cached
 * XKPHYS at physical 0x20010000; the first 512 MiB of the physical map
 * contains no RAM, so the KSEG0/KSEG1 direct map cannot be used for the
 * kernel image.
 */

#define	XIRI_IP30_BOOTSTACK_SIZE	(16 * 1024)

/*
 * Exception vectors are copied to the low 16-KiB SDRAM alias and cannot
 * reach XKPHYS kernel data with a compact lui/%lo address.  This cell, in
 * the unused tail of the exception block, holds &cpu_info_store.  Vector
 * code loads the full pointer from here before reading a CPUVAR field.
 */
#ifdef _LOCORE
#define	XIRI_IP30_CPU_INFO_CELL		0x80000ff0
#define	XIRI_IP30_SLOWFAULT_CELL	0x80000ff8
#else
#define	XIRI_IP30_CPU_INFO_CELL 	((volatile uint64_t *)(uintptr_t)0xffffffff80000ff0ULL)
#define	XIRI_IP30_SLOWFAULT_CELL 	((volatile uint64_t *)(uintptr_t)0xffffffff80000ff8ULL)
#endif

#if defined(_KERNEL) && !defined(_LOCORE)
void	xiri_ip30_early(void);
void	xiri_ip30_halt(void) __dead;
void	xiri_ip30_set_slowfault_cell(void);

extern char xiri_ip30_bootstack[];
#endif /* _KERNEL && !_LOCORE */

#endif /* _SGIMIPS_XIRI_IP30_H_ */
