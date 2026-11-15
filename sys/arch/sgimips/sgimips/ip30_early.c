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

/*
 * Octane/IP30 early entry checkpoint.  This runs from mach_init() before
 * any 32-bit ARCS structure or call is used, validates the ARCS64 firmware,
 * prints entry/CPU/memory evidence through the 64-bit Write vector and
 * deliberately stops.  It does not install vectors, enable interrupts,
 * build a pmap or attach devices.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_xiri_ip30.h"

#ifdef XIRI_IP30

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>

#include <mips/cpuregs.h>
#include <machine/xiri_ip30.h>

#define	ARC_GET_MEMORY_DESCRIPTOR	18
#define	ARC_WRITE			27

#define	ARCBIOS64_SPB_SIGNATURE		0x53435241U

#define	ARCBIOS64_MEM_EXCEPTION		0
#define	ARCBIOS64_MEM_SPB		1
#define	ARCBIOS64_MEM_FREE_CONTIGUOUS	2
#define	ARCBIOS64_MEM_FREE		3
#define	ARCBIOS64_MEM_BAD		4
#define	ARCBIOS64_MEM_LOADED_PROGRAM	5
#define	ARCBIOS64_MEM_FIRMWARE_TEMP	6
#define	ARCBIOS64_MEM_FIRMWARE_PERM	7

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

__CTASSERT(offsetof(struct arcs64_spb, vector) == 64);
__CTASSERT(sizeof(struct arcs64_spb) == 72);
__CTASSERT(offsetof(struct arcs64_mem, base_page) == 8);
__CTASSERT(sizeof(struct arcs64_mem) == 24);

#define	ARCBIOS64_SPB	((const volatile struct arcs64_spb *)0xffffffffa0001000UL)

typedef long (*arcs64_write_fn)(uint64_t, const void *, uint64_t, uint64_t *);
typedef const struct arcs64_mem *(*arcs64_getmem_fn)(
    const struct arcs64_mem *);

extern char kernel_text[];
extern char __bss_start[], _end[];

char xiri_ip30_bootstack[XIRI_IP30_BOOTSTACK_SIZE]
    __attribute__((__aligned__(16), __section__(".bss")));

volatile uint64_t xiri_ip30_bss_witness;

static void
arcs64_putstr(const char *s)
{
	arcs64_write_fn write;
	uint64_t done, n;

	write = (arcs64_write_fn)(uintptr_t)ARCBIOS64_SPB->vector[ARC_WRITE];
	for (n = 0; s[n] != '\0'; n++)
		continue;
	while (n != 0) {
		done = 0;
		if (write(1, s, n, &done) != 0 || done == 0 || done > n)
			return;
		s += done;
		n -= done;
	}
}

static void
arcs64_puthex(uint64_t value)
{
	char buf[19];
	const char *digits = "0123456789abcdef";
	unsigned int i;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++)
		buf[2 + i] = digits[(value >> (60 - i * 4)) & 0xf];
	buf[18] = '\0';
	arcs64_putstr(buf);
}

static void
arcs64_field(const char *name, uint64_t value)
{
	arcs64_putstr(name);
	arcs64_puthex(value);
	arcs64_putstr("\r\n");
}

static uint64_t
arcs64_cp0_read(unsigned int reg)
{
	uint64_t value;

	switch (reg) {
	case 12:
		__asm volatile("mfc0 %0, $12" : "=r"(value));
		break;
	case 15:
		__asm volatile("mfc0 %0, $15" : "=r"(value));
		break;
	default:
		value = 0;
		break;
	}
	return value;
}

static void __dead
arcs64_halt(void)
{
	for (;;)
		__asm volatile("nop");
}

void __dead
xiri_ip30_early(void)
{
	const volatile struct arcs64_spb *spb = ARCBIOS64_SPB;
	const struct arcs64_mem *mem = NULL;
	arcs64_getmem_fn getmem;
	uint64_t sp, total = 0, usable = 0, reserved = 0;
	uint64_t first_free = 0, loaded = 0;
	unsigned int descriptors = 0;

	if (spb->signature != ARCBIOS64_SPB_SIGNATURE ||
	    spb->length < sizeof(*spb) || spb->vector == NULL ||
	    spb->vector_length < 31 * sizeof(uint64_t)) {
		arcs64_halt();
	}

	arcs64_putstr("XIRI_IP30_KERNEL_ENTRY\r\n");
	arcs64_field("kernel.entry=", (uintptr_t)kernel_text);
	__asm volatile("move %0, $29" : "=r"(sp));
	arcs64_field("kernel.sp=", sp);
	arcs64_field("cp0.status=", arcs64_cp0_read(12));
	arcs64_field("cp0.prid=", arcs64_cp0_read(15));
	arcs64_field("kernel.bss=", (uintptr_t)__bss_start);
	arcs64_field("kernel.bss.end=", (uintptr_t)_end);
	arcs64_field("kernel.bss.size=",
	    (uintptr_t)_end - (uintptr_t)__bss_start);
	arcs64_field("kernel.bss.witness=", xiri_ip30_bss_witness);
	arcs64_field("kernel.stack.base=", (uintptr_t)xiri_ip30_bootstack);
	arcs64_field("kernel.stack.top=", (uintptr_t)xiri_ip30_bootstack +
	    XIRI_IP30_BOOTSTACK_SIZE);
	arcs64_field("spb.length=", spb->length);
	arcs64_field("spb.version=", spb->version);
	arcs64_field("spb.revision=", spb->revision);
	arcs64_field("spb.vector=", (uintptr_t)spb->vector);
	arcs64_field("spb.vector_length=", spb->vector_length);

	getmem = (arcs64_getmem_fn)(uintptr_t)spb->vector[
	    ARC_GET_MEMORY_DESCRIPTOR];
	while ((mem = getmem(mem)) != NULL) {
		if (++descriptors > 128) {
			arcs64_putstr("XIRI_IP30_MEMORY_DESCRIPTOR_LIMIT\r\n");
			arcs64_halt();
		}
		total += mem->page_count;
		switch (mem->type) {
		case ARCBIOS64_MEM_FREE_CONTIGUOUS:
		case ARCBIOS64_MEM_FREE:
			usable += mem->page_count;
			if (first_free == 0)
				first_free = mem->base_page;
			break;
		case ARCBIOS64_MEM_LOADED_PROGRAM:
			usable += mem->page_count;
			loaded = mem->base_page;
			break;
		default:
			reserved += mem->page_count;
			break;
		}
	}
	arcs64_field("memory.descriptors=", descriptors);
	arcs64_field("memory.total_pages=", total);
	arcs64_field("memory.usable_pages=", usable);
	arcs64_field("memory.reserved_pages=", reserved);
	arcs64_field("memory.first_free_page=", first_free);
	arcs64_field("memory.loaded_page=", loaded);
	arcs64_putstr("XIRI_IP30_KERNEL_ENTRY_OK\r\n");
	arcs64_halt();
}

#endif /* XIRI_IP30 */
