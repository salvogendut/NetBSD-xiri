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
 * Octane/IP30 early firmware support.  The entry checkpoint validates the
 * 64-bit ARCS interface and prints evidence.  With XIRI_IP30_M1_STOP it stops
 * there; otherwise the ARCS64 helpers below remain available for the memory
 * and vector bootstrap.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_xiri_ip30.h"

#ifdef XIRI_IP30

#include <sys/param.h>
#include <sys/types.h>
#include <sys/systm.h>

#include <dev/cons.h>
#include <dev/arcbios/arcbios.h>
#include <dev/arcbios/arcbiosvar.h>

#include <mips/cpuregs.h>
#include <mips/trap.h>

#include <machine/autoconf.h>
#include <machine/ip30_arcs64.h>
#include <machine/machtype.h>
#include <machine/xiri_ip30.h>

#define	ARCBIOS64_MEM_EXCEPTION		0
#define	ARCBIOS64_MEM_SPB		1
#define	ARCBIOS64_MEM_FREE_CONTIGUOUS	2
#define	ARCBIOS64_MEM_FREE		3
#define	ARCBIOS64_MEM_BAD		4
#define	ARCBIOS64_MEM_LOADED_PROGRAM	5
#define	ARCBIOS64_MEM_FIRMWARE_TEMP	6
#define	ARCBIOS64_MEM_FIRMWARE_PERM	7

__CTASSERT(offsetof(struct arcs64_spb, vector) == 64);
__CTASSERT(sizeof(struct arcs64_spb) == 72);
__CTASSERT(offsetof(struct arcs64_mem, base_page) == 8);
__CTASSERT(sizeof(struct arcs64_mem) == 24);

typedef long (*arcs64_read_fn)(uint64_t, void *, uint64_t, uint64_t *);
typedef long (*arcs64_write_fn)(uint64_t, const void *, uint64_t, uint64_t *);
typedef const struct arcs64_mem *(*arcs64_getmem_fn)(
    const struct arcs64_mem *);
typedef const char *(*arcs64_getenv_fn)(const char *);

/*
 * MIPS_CURLWP (t8) is a kernel convention, not an N64 ABI callee-saved
 * register.  ARCS64 firmware may use it as a scratch register, so preserve
 * it across every firmware call just as the 32-bit bridge does.
 */
#define	SAVE_CURLWP(saved)	do {					\
	__asm volatile("move %0, $24" : "=r"(saved));			\
} while (0)
#define	RESTORE_CURLWP(saved)	do {					\
	__asm volatile("move $24, %0" : : "r"(saved));			\
} while (0)

extern char kernel_text[];
extern char __bss_start[], _end[];

char xiri_ip30_bootstack[XIRI_IP30_BOOTSTACK_SIZE]
    __attribute__((__aligned__(16), __section__(".bss")));

volatile uint64_t xiri_ip30_bss_witness;

static long
ip30_arcs64_write(uint64_t fd, const void *buf, size_t len)
{
	arcs64_write_fn write;
	uint64_t done = 0;

	register_t saved_curlwp;

	write = (arcs64_write_fn)(uintptr_t)ARCBIOS64_SPB->vector[ARCBIOS64_WRITE];
	SAVE_CURLWP(saved_curlwp);
	const long error = write(fd, buf, len, &done);
	RESTORE_CURLWP(saved_curlwp);
	if (error != 0 || done > len)
		return -1;
	return done;
}

static long
ip30_arcs64_read(uint64_t fd, void *buf, size_t len)
{
	arcs64_read_fn read;
	uint64_t done = 0;

	register_t saved_curlwp;

	read = (arcs64_read_fn)(uintptr_t)ARCBIOS64_SPB->vector[ARCBIOS64_READ];
	SAVE_CURLWP(saved_curlwp);
	const long error = read(fd, buf, len, &done);
	RESTORE_CURLWP(saved_curlwp);
	if (error != 0 || done > len)
		return -1;
	return done;
}

const char *
ip30_arcs64_getenv(const char *name)
{
	arcs64_getenv_fn getenv;

	register_t saved_curlwp;
	const char *result;

	getenv = (arcs64_getenv_fn)(uintptr_t)ARCBIOS64_SPB->vector[
	    ARCBIOS64_GET_ENVIRONMENT_VARIABLE];
	SAVE_CURLWP(saved_curlwp);
	result = getenv(name);
	RESTORE_CURLWP(saved_curlwp);
	return result;
}

const struct arcs64_mem *
ip30_arcs64_getmem(const struct arcs64_mem *prev)
{
	arcs64_getmem_fn getmem;

	register_t saved_curlwp;
	const struct arcs64_mem *result;

	getmem = (arcs64_getmem_fn)(uintptr_t)ARCBIOS64_SPB->vector[
	    ARCBIOS64_GET_MEMORY_DESCRIPTOR];
	SAVE_CURLWP(saved_curlwp);
	result = getmem(prev);
	RESTORE_CURLWP(saved_curlwp);
	return result;
}

void
ip30_arcs64_putc(int c)
{
	char ch = c;

	(void)ip30_arcs64_write(1, &ch, 1);
}

int
ip30_arcs64_getc(void)
{
	char ch = 0;

	if (ip30_arcs64_read(0, &ch, 1) != 1)
		return -1;
	return ch;
}

static int
ip30_cngetc(dev_t dev)
{

	return ip30_arcs64_getc();
}

static void
ip30_cnputc(dev_t dev, int c)
{

	ip30_arcs64_putc(c);
}

static struct consdev ip30_cn = {
	.cn_getc = ip30_cngetc,
	.cn_putc = ip30_cnputc,
	.cn_pollc = nullcnpollc,
	.cn_dev = NODEV,
	.cn_pri = CN_NORMAL,
};

static void
arcs64_putstr(const char *s)
{
	uint64_t done, n;

	for (n = 0; s[n] != '\0'; n++)
		continue;
	while (n != 0) {
		done = ip30_arcs64_write(1, s, n);
		if ((long)done <= 0 || done > n)
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

#ifdef XIRI_IP30_M2_SLOWFAULT_TEST
extern uint64_t xiri_ip30_slowfault_load(vaddr_t);
void xiri_ip30_slowfault_check(vaddr_t, vaddr_t, uint32_t) __dead;

void __dead
xiri_ip30_slowfault_check(vaddr_t epc, vaddr_t badvaddr, uint32_t cause)
{
	const vaddr_t expected_badvaddr = VM_MAX_KERNEL_ADDRESS - PAGE_SIZE;
	const vaddr_t expected_epc = (vaddr_t)(uintptr_t)xiri_ip30_slowfault_load;
	const uint32_t code = (cause & MIPS3_CR_EXC_CODE) >>
	    MIPS_CR_EXC_CODE_SHIFT;

	arcs64_putstr("XIRI_IP30_M2_SLOWFAULT_CHECK\r\n");
	arcs64_field("epc=", epc);
	arcs64_field("epc.expected=", expected_epc);
	arcs64_field("badvaddr=", badvaddr);
	arcs64_field("badvaddr.expected=", expected_badvaddr);
	arcs64_field("cause=", cause);
	arcs64_field("exception=", code);
	if (epc == expected_epc && badvaddr == expected_badvaddr &&
	    code == T_TLB_LD_MISS)
		arcs64_putstr("XIRI_IP30_M2_SLOWFAULT_OK\r\n");
	else
		arcs64_putstr("XIRI_IP30_M2_SLOWFAULT_FAILED\r\n");
	xiri_ip30_halt();
}
#endif

void __dead
xiri_ip30_halt(void)
{

	for (;;)
		__asm volatile("nop");
}

void __dead
ip30_arcs64_enter(void)
{
	register_t saved_curlwp;
	void (*enter)(void);

	enter = (void (*)(void))(uintptr_t)ARCBIOS64_SPB->vector[7];
	SAVE_CURLWP(saved_curlwp);
	enter();
	RESTORE_CURLWP(saved_curlwp);
	xiri_ip30_halt();
}

void __dead
ip30_arcs64_reboot(void)
{
	register_t saved_curlwp;
	void (*reboot)(void);

	reboot = (void (*)(void))(uintptr_t)ARCBIOS64_SPB->vector[6];
	SAVE_CURLWP(saved_curlwp);
	reboot();
	RESTORE_CURLWP(saved_curlwp);
	xiri_ip30_halt();
}

void __dead
ip30_arcs64_powerdown(void)
{
	register_t saved_curlwp;
	void (*powerdown)(void);

	powerdown = (void (*)(void))(uintptr_t)ARCBIOS64_SPB->vector[4];
	SAVE_CURLWP(saved_curlwp);
	powerdown();
	RESTORE_CURLWP(saved_curlwp);
	xiri_ip30_halt();
}

void
ip30_arcs64_init(void)
{

	if (ARCBIOS64_SPB->signature != ARCBIOS64_SPB_SIGNATURE ||
	    ARCBIOS64_SPB->length < sizeof(*ARCBIOS64_SPB) ||
	    ARCBIOS64_SPB->vector == NULL ||
	    ARCBIOS64_SPB->vector_length < 31 * sizeof(uint64_t))
		xiri_ip30_halt();

	strcpy(arcbios_system_identifier, "IP30");
	strcpy(arcbios_sysid_vendor, "SGI");
	strcpy(arcbios_sysid_product, "IP30");
	mach_type = MACH_SGI_IP30;
	ncpus = 1;
	cn_tab = &ip30_cn;
}

void
xiri_ip30_early(void)
{
	const volatile struct arcs64_spb *spb = ARCBIOS64_SPB;
	const struct arcs64_mem *mem = NULL;
	uint64_t sp, total = 0, usable = 0, reserved = 0;
	uint64_t first_free = 0, loaded = 0;
	unsigned int descriptors = 0;

	if (spb->signature != ARCBIOS64_SPB_SIGNATURE ||
	    spb->length < sizeof(*spb) || spb->vector == NULL ||
	    spb->vector_length < 31 * sizeof(uint64_t)) {
		xiri_ip30_halt();
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

	while ((mem = ip30_arcs64_getmem(mem)) != NULL) {
		if (++descriptors > 128) {
			arcs64_putstr("XIRI_IP30_MEMORY_DESCRIPTOR_LIMIT\r\n");
			xiri_ip30_halt();
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

#ifdef XIRI_IP30_M1_STOP
	xiri_ip30_halt();
#endif
}

#endif /* XIRI_IP30 */
