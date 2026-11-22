/*	$NetBSD: boot.c,v 1.20 2014/03/26 16:16:06 christos Exp $	*/

/*-
 * Copyright (c) 1999 The NetBSD Foundation, Inc.
 * All rights reserved.
 *
 * This code is derived from software contributed to The NetBSD Foundation
 * by Jonathan Stone, Michael Hitch and Simon Burge.
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
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * Ralph Campbell.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)boot.c	8.1 (Berkeley) 6/10/93
 */

#include <lib/libsa/stand.h>
#include <lib/libsa/loadfile.h>
#include <lib/libkern/libkern.h>

#include <sys/param.h>
#include <sys/exec.h>
#include <sys/exec_elf.h>
#include <sys/boot_flag.h>

#include <dev/arcbios/arcbios.h>
#ifdef ARCBIOS64
#include <mips/cpuregs.h>
#endif

#include "common.h"
#include "bootinfo.h"

/*
 * We won't go overboard with gzip'd kernel names.  After all we can
 * still boot a gzip'd kernel called "netbsd.sgimips" - it doesn't need
 * the .gz suffix.
 *
 * For arcane reasons, the first byte of the first element of this struct will
 * contain a zero.  We therefore start from one.
 */

const char *kernelnames[] = {
	"placekeeper",
	"netbsd.sgimips",
	"netbsd",
	"netbsd.gz",
	"netbsd.bak",
	"netbsd.old",
	"onetbsd",
	"gennetbsd",
	NULL
};

static int debug = 0;

int main(int, char **);

/* Storage must be static. */
struct btinfo_symtab bi_syms;
struct btinfo_bootpath bi_bpath;

static uint8_t bootinfo[BOOTINFO_SIZE];

#ifdef ARCBIOS64
/* Firmware memory descriptors have 64-bit page numbers in ARCS64. */
struct arcs64_memory {
	uint32_t type;
	uint64_t base_page;
	uint64_t page_count;
};

static uint64_t
boot_physical(u_long address)
{
	if (MIPS_XKPHYS_P(address))
		return MIPS_XKPHYS_TO_PHYS(address);
	if (MIPS_KSEG0_P(address) || MIPS_KSEG1_P(address))
		return address & MIPS_PHYS_MASK;
	return UINT64_MAX;
}

static int
load_kernel(const char *file, u_long *marks)
{
	const struct arcs64_memory *mem = NULL;
	u_long counted[MARK_MAX] = { 0 };
	uint64_t first, last, cursor, begin, end;
	int fd;

	/* Reject a kernel linked for another machine before writing its RAM. */
	fd = loadfile(file, counted, COUNT_KERNEL);
	if (fd < 0)
		return fd;
	close(fd);
	first = boot_physical(counted[MARK_START]);
	last = boot_physical(counted[MARK_END] - 1);
	if (first == UINT64_MAX || last == UINT64_MAX || last < first)
		goto bad_address;
	cursor = first;
	/* Free descriptors can be adjacent; require coverage of the entire span. */
	while ((mem = arcbios_GetMemoryDescriptor((void *)mem)) != NULL) {
		if (mem->type != ARCBIOS_MEM_FreeContiguous &&
		    mem->type != ARCBIOS_MEM_FreeMemory)
			continue;
		begin = mem->base_page << 12;
		end = begin + (mem->page_count << 12);
		if (begin <= cursor && cursor < end) {
			if (last < end)
				return loadfile(file, marks, LOAD_KERNEL);
			cursor = end;
		}
	}
bad_address:
	printf("Kernel address range 0x%lx..0x%lx is outside usable firmware RAM.\n",
	    counted[MARK_START], counted[MARK_END]);
	printf("Use a kernel linked for this machine.\n");
	errno = EFTYPE;
	return -1;
}
#else
#define load_kernel(file, marks) loadfile((file), (marks), LOAD_KERNEL)
#endif

/*
 * This gets arguments from the ARCS monitor, calls ARCS routines to open
 * and load the program to boot, then transfers execution to the new program.
 *
 * argv[0] will be the ARCS path to the bootloader (i.e.,
 * "pci(0)scsi(0)disk(2)rdisk(0)partition(8)/boot.ip3").
 *
 * argv[1] through argv[n] will contain arguments passed from the PROM, if any.
 */

int
main(int argc, char **argv)
{
	const char      *kernel = NULL;
	const char      *bootpath = NULL;
	char            bootfile[PATH_MAX];
	void            (*entry) (int, char *[], int, void *);
	u_long          marks[MARK_MAX];
	int             win = 0;
	int             i;
	int             ch;

	/* print a banner */
	printf("\n");
	printf("%s " NETBSD_VERS " Bootstrap, Revision %s\n",
	    bootprog_name, bootprog_rev);
	printf("\n");

	memset(marks, 0, sizeof marks);

	/* initialise bootinfo structure early */
	bi_init(bootinfo);

	/* Parse arguments, if present.  */
	while ((ch = getopt(argc, argv, "v")) != -1) {
		switch (ch) {
		case 'v':
			debug = 1;
			break;
		}
	}

	/*
	 * How to find partition and file to load?
	 *
	 * If argv[0] contains the string "cdrom(", we're probably doing an
	 * install.  The bootpath will therefore be partition 0 of whatever
	 * device we've booted from.  Derive the install kernel name from
	 * the bootloader name ("ip3xboot", "ip2xboot", or "aoutboot").
	 */

	if (strstr(argv[0], "cdrom(")) {
		char *ep =
		strcpy(bootfile, argv[0]);
		ep = strrchr(bootfile, ')');
		i =  ep - bootfile;
		bootfile[i - 1] = '0';
		if (strstr(bootfile, "ip3x"))
			kernel = "ip3x";
		else
			kernel = "ip2x";
		strcpy(ep + 1, kernel);
		if (load_kernel(bootfile, marks) >= 0)
			goto finish;
	}

	bootpath = arcbios_GetEnvironmentVariable("OSLoadPartition");

	if (bootpath == NULL) {
		/* XXX need to actually do the fixup */
		printf("\nPlease set the OSLoadPartition "
		    "environment variable.\n");
		return 0;
	}

	/*
	 * Grab OSLoadFilename from ARCS.
	 */

	kernel = arcbios_GetEnvironmentVariable("OSLoadFilename");

	/*
	 * argv[1] is assumed to contain the name of the kernel to boot,
	 * if it a) does not start with a hyphen and b) does not contain
	 * an equals sign.
	 */

	if (argc > 1 && argv[1] != NULL &&
	    strchr(argv[1], '=') == NULL && argv[1][0] != '-')
		kernel = argv[1];

	if (kernel != NULL) {
		/*
		 * if the name contains parenthesis, we assume that it
		 * contains the bootpath and ignore anything passed through
		 * the environment
		 */
		if (strchr(kernel, '('))
			win = load_kernel(kernel, marks);
		else {
			strcpy(bootfile, bootpath);
			strcat(bootfile, kernel);
			win = load_kernel(bootfile, marks);
		}

	} else {
		i = 1;
		while (kernelnames[i] != NULL) {
			strcpy(bootfile, bootpath);
			strcat(bootfile, kernelnames[i]);
			kernel = kernelnames[i];
			win = load_kernel(bootfile, marks);
			if (win != -1)
				break;
			i++;
		}
	}

	if (win < 0) {
		printf("Boot failed!  Halting...\n");
		return 0;
	}

finish:
	strlcpy(bi_bpath.bootpath, bootfile, BTINFO_BOOTPATH_LEN);
	bi_add(&bi_bpath, BTINFO_BOOTPATH, sizeof(bi_bpath));

	bi_syms.nsym = marks[MARK_NSYM];
	bi_syms.ssym = marks[MARK_SYM];
	bi_syms.esym = marks[MARK_END];
	bi_add(&bi_syms, BTINFO_SYMTAB, sizeof(bi_syms));
	entry = (void *)marks[MARK_ENTRY];

	if (debug) {
		printf("Starting at %p\n\n", entry);
		printf("nsym 0x%lx ssym 0x%lx esym 0x%lx\n", marks[MARK_NSYM],
		       marks[MARK_SYM], marks[MARK_END]);
	}
#ifdef ARCBIOS64
	arcbios_FlushAllCaches();
#endif
	(*entry)(argc, argv, BOOTINFO_MAGIC, bootinfo);

	printf("Kernel returned!  Halting...\n");
	return 0;
}
