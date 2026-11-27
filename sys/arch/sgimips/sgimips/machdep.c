/*	$NetBSD: machdep.c,v 1.158 2026/06/10 21:24:11 andvar Exp $	*/

/*
 * Copyright (c) 2000 Soren S. Jorvang
 * Copyright (c) 2001, 2002, 2003 Rafal K. Boni
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *          This product includes software developed for the
 *          NetBSD Project.  See http://www.NetBSD.org/ for
 *          information about NetBSD.
 * 4. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD: machdep.c,v 1.158 2026/06/10 21:24:11 andvar Exp $");

#include "opt_ddb.h"
#include "opt_kgdb.h"
#include "opt_execfmt.h"
#include "opt_cputype.h"
#include "opt_mips_cache.h"
#include "opt_modular.h"
#include "opt_arcbios64.h"
#include "opt_xiri_ip30.h"

#define __INTR_PRIVATE

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/proc.h>
#include <sys/buf.h>
#include <sys/bus.h>
#include <sys/reboot.h>
#include <sys/conf.h>
#include <sys/file.h>
#include <sys/intr.h>
#include <sys/mbuf.h>
#include <sys/msgbuf.h>
#include <sys/device.h>
#include <sys/exec.h>
#include <sys/mount.h>
#include <sys/syscallargs.h>
#include <sys/kcore.h>
#include <sys/boot_flag.h>
#include <sys/ksyms.h>
#include <sys/cpu.h>

#include <uvm/uvm_extern.h>

#include <mips/locore.h>

#include <machine/reg.h>
#include <machine/psl.h>
#include <machine/autoconf.h>
#include <machine/machtype.h>
#include <machine/sysconf.h>
#include <machine/bootinfo.h>
#if defined(ARCBIOS64) || defined(XIRI_IP30)
#include <machine/arcs64.h>
#endif
#ifdef XIRI_IP30
#include <machine/ip30_arcs64.h>
#include <machine/xiri_ip30.h>
#include <mips/mips3_pte.h>
#define	arcbios_GetEnvironmentVariable	ip30_arcs64_getenv
#endif

#include <mips/cache.h>
#include <mips/cache_r5k.h>
#ifdef ENABLE_MIPS4_CACHE_R10K
#include <mips/cache_r10k.h>
#endif

#include <sgimips/dev/int2reg.h>
#include <sgimips/dev/crimevar.h>
#include <sgimips/hpc/haltworeg.h>
#include <sgimips/sgimips/arcemu.h>

#include <dev/arcbios/arcbios.h>
#include <dev/arcbios/arcbiosvar.h>

#include "ksyms.h"

#if NKSYMS || defined(DDB) || defined(MODULAR) || defined(KGDB)
#include <machine/db_machdep.h>
#include <ddb/db_access.h>
#include <ddb/db_sym.h>
#include <ddb/db_extern.h>
#include <sys/exec_elf.h>
#endif

#include "mcclock_mace.h"
#include "crime.h"

#if NMCCLOCK_MACE > 0
void mcclock_poweroff(void);
#endif

/* The optional audio personality can also load on machines without HAL2. */
struct haltwo_mute_hook_t haltwo_mute_hook;

struct sgimips_intrhand intrtab[NINTR];

/* Maps for VM objects. */
struct vm_map *phys_map = NULL;

int mach_type = 0;	/* IPxx type */
int mach_subtype = 0;	/* subtype: eg., Guinness/Fullhouse for IP22 */
int mach_boardrev = 0;	/* machine board revision, in case it matters */

int arcsmem;		/* Memory used by the ARCS firmware */

int ncpus;

#define IPL2SPL_TABLE_COMMON					\
	[IPL_NONE] =		0,				\
	[IPL_SOFTCLOCK] =	MIPS_SOFT_INT_MASK_0,		\
	[IPL_SOFTNET] =		MIPS_SOFT_INT_MASK,		\
	[IPL_DDB] =		MIPS_INT_MASK,			\
	[IPL_HIGH] =		MIPS_INT_MASK

/* CPU interrupt sr maps */
#if defined(MIPS1)
static const struct ipl_sr_map sgi_ip6_ipl_sr_map = {
    .sr_bits = {
	IPL2SPL_TABLE_COMMON,
	[IPL_VM] = MIPS_INT_MASK_1|MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
	[IPL_SCHED] = MIPS_INT_MASK_4|MIPS_INT_MASK_2|
	    MIPS_INT_MASK_1|MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
    },
};
static const struct ipl_sr_map sgi_ip12_ipl_sr_map = {
    .sr_bits = {
	IPL2SPL_TABLE_COMMON,
	[IPL_VM] = MIPS_INT_MASK_2|MIPS_INT_MASK_1|MIPS_INT_MASK_0|
	    MIPS_SOFT_INT_MASK,
	[IPL_SCHED] = MIPS_INT_MASK_4|MIPS_INT_MASK_3|MIPS_INT_MASK_2|
	    MIPS_INT_MASK_1|MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
    },
};
#endif /* defined(MIPS1) */

#if defined(MIPS3)
static const struct ipl_sr_map sgi_ip2x_ipl_sr_map = {
    .sr_bits = {
	IPL2SPL_TABLE_COMMON,
	[IPL_VM] = MIPS_INT_MASK_1|MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
	[IPL_SCHED] = MIPS_INT_MASK,
    },
};
static const struct ipl_sr_map sgi_ip3x_ipl_sr_map = {
    .sr_bits = {
	IPL2SPL_TABLE_COMMON,
	[IPL_VM] = MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
	[IPL_SCHED] = MIPS_INT_MASK_5|MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
    },
};
static const struct ipl_sr_map sgi_ip30_ipl_sr_map = {
    .sr_bits = {
	IPL2SPL_TABLE_COMMON,
	/*
	 * HEART drives IP2..IP6 for ISR levels 0..4.  Device handlers
	 * use IPL_VM in this tree, so mask all five HEART pins at IPL_VM
	 * and above; IP7 remains the CP0 Count/Compare timer.
	 */
	[IPL_VM] = MIPS_INT_MASK_4|MIPS_INT_MASK_3|MIPS_INT_MASK_2|
	    MIPS_INT_MASK_1|MIPS_INT_MASK_0|MIPS_SOFT_INT_MASK,
	[IPL_SCHED] = MIPS_INT_MASK|MIPS_SOFT_INT_MASK,
    },
};
#endif /* defined(MIPS3) */

phys_ram_seg_t mem_clusters[VM_PHYSSEG_MAX];
int mem_cluster_cnt;

#if defined(INDY_R4600_CACHE)
extern void	ip22_sdcache_disable(void);
extern void	ip22_sdcache_enable(void);
#endif

#if defined(MIPS3)
extern void mips3_clock_intr(vaddr_t, uint32_t, uint32_t);
#endif

#ifdef XIRI_IP30_M2_TLB_TEST
static void	ip30_tlb_refill_test(void);
#endif

void	mach_init(int, int32_t *, uintptr_t, uintptr_t);

void	sgimips_count_cpus(struct arcbios_component *,
	    struct arcbios_treewalk_context *);

#ifdef KGDB
void kgdb_port_init(void);
void kgdb_connect(int);
#endif

void mips_machdep_find_l2cache(struct arcbios_component *comp, struct arcbios_treewalk_context *atc);

/* Motherboard or system-specific initialization vector */
static void	unimpl_bus_reset(void);
static void	unimpl_cons_init(void);
static void	*unimpl_intr_establish(int, int, int (*)(void *), void *);
static void	unimpl_intr(vaddr_t, uint32_t, uint32_t);
static unsigned	long nulllong(void);
static void	nullvoid(void);

void ddb_trap_hook(int where);

static int badaddr_workaround(void *, size_t);

struct platform platform = {
	.badaddr		= badaddr_workaround,
	.bus_reset		= unimpl_bus_reset,
	.cons_init		= unimpl_cons_init,
	.intr_establish		= unimpl_intr_establish,
	.clkread		= nulllong,
	.watchdog_reset		= nullvoid,
	.watchdog_disable	= nullvoid,
	.watchdog_enable	= nullvoid,
	.intr0			= unimpl_intr,
	.intr1			= unimpl_intr,
	.intr2			= unimpl_intr,
	.intr3			= unimpl_intr,
	.intr4			= unimpl_intr,
	.intr5			= unimpl_intr
};

extern u_int32_t ssir;
extern char kernel_text[], edata[], end[];

#if defined(ARCBIOS64) || defined(XIRI_IP30)
char sgimips_arcs64_bootstack[SGIMIPS_ARCS64_BOOTSTACK_SIZE]
    __attribute__((__aligned__(16)));
#endif

uint8_t *bootinfo;			/* pointer to bootinfo structure */
#ifndef XIRI_IP30
static uint8_t bi_buf[BOOTINFO_SIZE];	/* buffer to store bootinfo data */
#endif
static const char *bootinfo_msg = NULL;

#define ARCS_VECTOR MIPS_PHYS_TO_KSEG0(0x00001000)

/*
 * Do all the stuff that locore normally does before calling main().
 * Process arguments passed to us by the ARCS firmware.
 */
void
mach_init(int argc, int32_t argv32[], uintptr_t magic, uintptr_t bipval)
{
	paddr_t first, last;
	vsize_t size;
	uint32_t mem_type;
	uint64_t mem_base, mem_pages;
#ifndef XIRI_IP30
#ifdef ARCBIOS64
	void *bip = (void *)bipval;
#else
	void *bip = (void *)(intptr_t)(int32_t)bipval;
#endif
	struct arcbios_mem *mem;
#else
	const struct arcs64_mem *mem64;
	const uint64_t *argv64;
#endif
	const char *cpufreq, *osload;
	char *bootpath = NULL;
	vaddr_t kernend;
	u_int i;
	int rv;
#if NKSYMS || defined(DDB) || defined(MODULAR)
	int nsym = 0;
	char *ssym = NULL;
	char *esym = NULL;
	struct btinfo_symtab *bi_syms;
#endif /* NKSYMS || defined(DDB) || defined(MODULAR) */
#ifdef _LP64
	char *argv[20];
#else
	char **argv = (void *)argv32;
#endif

#ifdef XIRI_IP30
	/*
	 * The Octane firmware uses ARCS64, not the 32-bit interface below.
	 * Prove entry and firmware access before the legacy argv conversion
	 * or argument-count check.
	 */
	xiri_ip30_early();

	if (argc >= __arraycount(argv))
		panic("too many args");
	argv64 = (const uint64_t *)(uintptr_t)argv32;
	for (i = 0; i < argc; i++)
		argv[i] = (void *)(uintptr_t)argv64[i];
#elif defined(_LP64)
	if (argc >= __arraycount(argv))
		panic("too many args");

	for (i = 0; i < argc; i++) {
#ifdef ARCBIOS64
		argv[i] = (void *)(uintptr_t)((uint64_t *)argv32)[i];
#else
		argv[i] = (void *)(intptr_t)argv32[i];
#endif
	}
#endif

	/*
	 * Initialize firmware.  This will set up the bootstrap console.
	 * At this point we do not yet know the machine type, so we
	 * try to init real arcbios, and if that fails (return value 1),
	 * fall back to the emulator.  If the latter fails also we
	 * don't have much to panic with.
	 *
	 * The third argument (magic) is the environment variable array if
	 * there's no bootinfo.
	 */
#ifdef XIRI_IP30
	ip30_arcs64_init();
	printf("XIRI_M2_ARCS64_INIT\n");
#else
	if (arcbios_init(ARCS_VECTOR) == 1) {
#ifdef _LP64
		panic("no ARCS firmware");
#else
		if (magic == BOOTINFO_MAGIC)
			arcemu_init(NULL);	/* XXX - need some prom env */
		else
			arcemu_init((const char **)magic);
#endif
	}
#endif

	cpu_setmodel("%s", arcbios_system_identifier);
#ifdef ARCBIOS64
	printf("ARCS64: %s firmware initialized\n", arcbios_system_identifier);
#endif
#if defined(ARCBIOS64) || defined(XIRI_IP30)
	/*
	 * Vector code runs from the low SDRAM alias and cannot address
	 * cpu_info_store or mips3_slowfault in XKPHYS with a compact
	 * lui/%lo or 26-bit j sequence.  Publish both addresses in the
	 * reserved tail of the exception block before installing vectors.
	 */
	*SGIMIPS_ARCS64_CPU_INFO_CELL = (uintptr_t)&cpu_info_store;
	sgimips_arcs64_set_slowfault_cell();
#endif

	/*
	 * Copy exception-dispatch code down to exception vector.
	 * Initialize locore-function vector.
	 * Clear out the I and D caches.
	 */
	mips_vector_init(NULL, false);
#ifdef XIRI_IP30
	printf("XIRI_M2_VECTOR_DONE\n");
#endif

	uvm_md_init();

	/* set up bootinfo structures */
#ifdef XIRI_IP30
	bootinfo_msg = "no bootinfo. (ARCS64 direct boot)\n";
#else
	if (magic == BOOTINFO_MAGIC && bip != NULL) {
		struct btinfo_magic *bi_magic;
		struct btinfo_bootpath *bi_path;

		memcpy(bi_buf, bip, BOOTINFO_SIZE);
		bootinfo = bi_buf;
		bi_magic = lookup_bootinfo(BTINFO_MAGIC);
		if (bi_magic != NULL && bi_magic->magic == BOOTINFO_MAGIC) {
			bootinfo_msg = "bootinfo found.\n";
			bi_path = lookup_bootinfo(BTINFO_BOOTPATH);
			if (bi_path != NULL)
				bootpath = bi_path->bootpath;
		} else
			bootinfo_msg =
			    "invalid magic number in bootinfo structure.\n";
	} else
		bootinfo_msg = "no bootinfo found. (old bootblocks?)\n";
#endif

#if NKSYMS || defined(DDB) || defined(MODULAR)
	bi_syms = lookup_bootinfo(BTINFO_SYMTAB);

	/* check whether there is valid bootinfo symtab info */
	if (bi_syms != NULL) {
		nsym = bi_syms->nsym;
		ssym = (char *)bi_syms->ssym;
		esym = (char *)bi_syms->esym;
		kernend = mips_round_page(esym);
	} else
#endif /* NKSYMS || defined(DDB) || defined(MODULAR) */
	{
		kernend = mips_round_page(end);
	}

	cpufreq = arcbios_GetEnvironmentVariable("cpufreq");

	if (cpufreq == 0)
		panic("no $cpufreq");

	/*
	 * Note initial estimate of CPU speed... If we care enough, we'll
	 * use the RTC to get a better estimate later.
	 */
	curcpu()->ci_cpu_freq = strtoul(cpufreq, NULL, 10) * 1000000;

	/*
	 * Also initialize ci members for delay and clock by the temporary
	 * ci_cpu_freq value for early use of delay(9).
	 * These values will be calibrated later in MD code:
	 *  - int_attach() in dev/int.c for IP6/10/12/20/22
	 *  - crime_attach() in dev/crime.c for IP32
	 *
	 * XXX: ci_divisor_delay is for mips3_delay() in mips/mips3_clock.c
	 *      but sgimips abuse it as "instructions per microsecond"
	 *      for traditional delay(9) implementation derived from
	 *      4.4BSD/mips (also used in pmax and news3400).
	 *	(see sys/arch/mips/mips/mips_mcclock.c etc.)
	 *
	 * Note ci_cycles_per_hz is for mips3_clockintr.c for MIPS3 so
	 * there is no early use, but initialize it as a sane default.
	 */
	curcpu()->ci_cycles_per_hz = curcpu()->ci_cpu_freq / (2 * hz);
	curcpu()->ci_divisor_delay = curcpu()->ci_cpu_freq / (2 * 1000000);

	/*
	 * Check machine (IPn) type.
	 *
	 * Note even on IP12 (which doesn't have ARCBIOS),
	 * arcbios_system_identifier[] has been initialized
	 * in arcemu_ip12_init().
	 */
	for (i = 0; arcbios_system_identifier[i] != '\0'; i++) {
		if (mach_type == 0 &&
		    arcbios_system_identifier[i] >= '0' &&
		    arcbios_system_identifier[i] <= '9') {
			mach_type = strtoul(&arcbios_system_identifier[i],
			    NULL, 10);
			break;
		}
	}
	if (mach_type <= 0)
		panic("invalid architecture");

	/*
	 * Initialise the IP22 Indy/Indigo2 subtype early.
	 *
	 * This is needed for various subsystems that require mach_subtype
	 * to properly initialise.  It's also setup in ioc/ioc.c but
	 * that's far too late for some peripherals (notably Indigo2 INT2
	 * timer programming/calibration.)
	 */
	if (mach_type == MACH_SGI_IP22 || mach_type == MACH_SGI_IP28) {
		i = *(volatile u_int32_t *)MIPS_PHYS_TO_KSEG1(0x1fbd9858);
		if (i & 0x01)
			mach_subtype = MACH_SGI_IP22_FULLHOUSE; /* Indigo2 */
		else
			mach_subtype = MACH_SGI_IP22_GUINNESS; /* Indy */
	}

	/*
	 * Get boot device information.
	 */

	/* Try to get the boot device information from bootinfo first. */
	if (bootpath != NULL)
		makebootdev(bootpath);
	else {
		/*
		 * The old bootloader prior to 5.0 doesn't pass bootinfo.
		 * If argv[0] is the bootloader, then argv[1] might be
		 * the kernel that was loaded.
		 * If argv[1] isn't an environment string, try to use it
		 * to set the boot device.
		 */
		if (argc > 1 && strchr(argv[1], '=') != 0)
			makebootdev(argv[1]);

		/*
		 * If we are loaded directly by ARCBIOS,
		 * argv[0] is the path of the loaded kernel,
		 * but booted_partition could be SGIVOLHDR in such case,
		 * so assume root is partition a.
		 */
		if (argc > 0 && argv[0] != NULL) {
			makebootdev(argv[0]);
			booted_partition = 0;
		}
	}

	/*
	 * Also try to get the default bootpath from ARCBIOS environment
	 * because bootpath is not set properly by old bootloaders and
	 * argv[0] might be invalid on some machine.
	 */
	osload = arcbios_GetEnvironmentVariable("OSLoadPartition");
	if (osload != NULL)
		makebootdev(osload);

	/*
	 * The case where the kernel has been loaded by a
	 * boot loader will usually have been caught by
	 * the first makebootdev() case earlier on, but
	 * we still use OSLoadPartition to get the preferred
	 * root filesystem location, even if it's not
	 * actually the location of the loaded kernel.
	 */
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "OSLoadPartition=", 15) == 0)
			makebootdev(argv[i] + 16);
	}

	/*
	 * When the kernel is loaded directly by the firmware, and
	 * no explicit OSLoadPartition is set, we fall back on
	 * SystemPartition for the boot device.
	 */
	for (i = 0; i < argc; i++) {
		if (strncmp(argv[i], "SystemPartition", 15) == 0)
			makebootdev(argv[i] + 16);
	}

	/*
	 * Single- or multi-user ('auto' in SGI terms).
	 *
	 * Query ARCBIOS first, then default to environment variables.
	 */

	/* Set default to single user. */
	boothowto = RB_SINGLE;

	osload = arcbios_GetEnvironmentVariable("OSLoadOptions");
	if (osload != NULL && strcmp(osload, "auto") == 0)
		boothowto &= ~RB_SINGLE;

	for (i = 0; i < argc; i++) {
		if (strcmp(argv[i], "OSLoadOptions=auto") == 0)
			boothowto &= ~RB_SINGLE;
	}

	/*
	 * Pass the args again to check for flags -- This is done
	 * AFTER checking for OSLoadOptions to ensure that "auto"
	 * does not override the "-s" flag.
	 */

	for (i = 0; i < argc; i++) {
		/*
		 * Unfortunately, it appears that IP12's prom passes a '-a'
		 * flag when booting a kernel directly from a disk volume
		 * header. This corresponds to RB_ASKNAME in NetBSD, but
		 * appears to mean 'autoboot' in prehistoric SGI-speak.
		 */
		if (mach_type < MACH_SGI_IP20 && bootinfo == NULL &&
		    strcmp(argv[i], "-a") == 0)
			continue;

		/*
		 * Extract out any flags passed for the kernel in the
		 * argument string.  Warn for unknown/invalid flags,
		 * but silently skip non-flag arguments, as they are
		 * likely PROM environment values (if I knew those
		 * would always precede *any* flags, then I'd say we
		 * should warn about *all* unexpected values, but for
		 * now this should be fine).
		 *
		 * Use the MI boot-flag extractor since we don't use
		 * any special MD flags and to make sure we're up-to
		 * date with new MI flags whenever they're added.
		 */
		if (argv[i][0] == '-') {
			rv = 0;
			BOOT_FLAG(argv[i][1], rv);

			if (rv == 0) {
				printf("Unexpected option '%s' ignored",
				    argv[i]);
			} else {
				boothowto |= rv;
			}
		}
	}

#ifdef DEBUG
	boothowto |= AB_DEBUG;
#endif
	aprint_debug("argc = %d\n", argc);
	for (i = 0; i < argc; i++)
		aprint_debug("argv[%d] = %s\n", i, argv[i]);

#if NKSYMS || defined(DDB) || defined(MODULAR)
	/* init symbols if present */
	if (esym)
		ksyms_addsyms_elf(nsym, ssym, esym);
#endif /* NKSYMS || defined(DDB) || defined(MODULAR) */

#if defined(KGDB) || defined(DDB)
	/* Set up DDB hook to turn off watchdog on entry */
	db_trap_callback = ddb_trap_hook;

#ifdef DDB
	if (boothowto & RB_KDB)
		Debugger();
#endif

#ifdef KGDB
	kgdb_port_init();

	if (boothowto & RB_KDB)
		kgdb_connect(0);
#endif
#endif

	switch (mach_type) {
#if defined(MIPS1)
	case MACH_SGI_IP6 | MACH_SGI_IP10:
		platform.intr3 = mips1_fpu_intr;
		ipl_sr_map = sgi_ip6_ipl_sr_map;
		break;

	case MACH_SGI_IP12:
		i = *(volatile u_int32_t *)MIPS_PHYS_TO_KSEG1(0x1fbd0000);
        	mach_boardrev = (i & 0x7000) >> 12;

		if ((i & 0x8000) == 0) {
			if (mach_boardrev < 7)
				mach_subtype = MACH_SGI_IP12_4D_3X;
			else
				mach_subtype = MACH_SGI_IP12_VIP12;
		} else {
			if (mach_boardrev < 6)
				mach_subtype = MACH_SGI_IP12_HP1;
			else
				mach_subtype = MACH_SGI_IP12_HPLC;
                }

		ipl_sr_map = sgi_ip12_ipl_sr_map;
		platform.intr0 = mips1_fpu_intr;
		break;
#endif /* MIPS1 */

#if defined(MIPS3)
	case MACH_SGI_IP20:
		i = *(volatile u_int32_t *)MIPS_PHYS_TO_KSEG1(0x1fbd0000);
		mach_boardrev = (i & 0x7000) >> 12;
		ipl_sr_map = sgi_ip2x_ipl_sr_map;
		platform.intr5 = mips3_clock_intr;
		break;
	case MACH_SGI_IP22:
	case MACH_SGI_IP28:
		ipl_sr_map = sgi_ip2x_ipl_sr_map;
		platform.intr5 = mips3_clock_intr;
		break;
	case MACH_SGI_IP30:
		ipl_sr_map = sgi_ip30_ipl_sr_map;
		platform.intr5 = mips3_clock_intr;
		break;
	case MACH_SGI_IP32:
		ipl_sr_map = sgi_ip3x_ipl_sr_map;
		platform.intr5 = mips3_clock_intr;
		break;
#endif /* MIPS3 */
	default:
		panic("IP%d architecture not supported", mach_type);
		break;
	}

	physmem = arcsmem = 0;
	mem_cluster_cnt = 0;
#ifdef XIRI_IP30
	mem64 = NULL;
#else
	mem = NULL;
#endif

#if defined(DEBUG) && !defined(XIRI_IP30)
	i = 0;
	mem = NULL;

	do {
		if ((mem = arcbios_GetMemoryDescriptor(mem)) != NULL) {
			i++;
			printf("Mem block %d: type %d, "
			    "base 0x%04"PRIx64", size 0x%04"PRIx64"\n",
			    i, mem->Type, (uint64_t)mem->BasePage,
			    (uint64_t)mem->PageCount);
		}
	} while (mem != NULL);
#endif

	/*
	 * XXX This code assumes that ARCS provides the memory
	 * XXX sorted in ascending order.
	 */
#ifdef XIRI_IP30
	mem64 = NULL;
#else
	mem = NULL;
#endif
	for (i = 0; mem_cluster_cnt < VM_PHYSSEG_MAX; i++) {
#ifdef XIRI_IP30
		mem64 = ip30_arcs64_getmem(mem64);
		if (mem64 == NULL)
			break;
		mem_type = mem64->type;
		mem_base = mem64->base_page;
		mem_pages = mem64->page_count;
#else
		mem = arcbios_GetMemoryDescriptor(mem);

		if (mem == NULL)
			break;
		mem_type = mem->Type;
		mem_base = mem->BasePage;
		mem_pages = mem->PageCount;
#endif

		first = round_page(mem_base * ARCBIOS_PAGESIZE);
		last = trunc_page((mem_base + mem_pages) * ARCBIOS_PAGESIZE);
		if (first >= last)
			continue;
		size = last - first;

		switch (mem_type) {
		case ARCBIOS_MEM_FreeContiguous:
		case ARCBIOS_MEM_FreeMemory:
		case ARCBIOS_MEM_LoadedProgram:
			mem_clusters[mem_cluster_cnt].start = first;
			mem_clusters[mem_cluster_cnt].size = size;
			mem_cluster_cnt++;
			break;

		case ARCBIOS_MEM_FirmwareTemporary:
		case ARCBIOS_MEM_FirmwarePermanent:
			arcsmem += btoc(size);
			break;

		case ARCBIOS_MEM_ExceptionBlock:
		case ARCBIOS_MEM_SystemParameterBlock:
		case ARCBIOS_MEM_BadMemory:
			break;

		default:
			panic("unknown memory descriptor %d type %d",
				i, mem_type);
		}

		physmem += btoc(size);

	}

#ifdef XIRI_IP30
	printf("XIRI_M2_MEMORY_SCAN_DONE clusters=%d physmem=%lu\n",
	    mem_cluster_cnt, (unsigned long)physmem);
#endif
	if (mem_cluster_cnt == 0)
		panic("no free memory descriptors found");

	/* Leave 1 page before kernel untouched as that's where our initial
	 * kernel stack is */
	/* XXX We could free it in cpu_startup() though XXX */
	mips_page_physload((vaddr_t)kernel_text - PAGE_SIZE, (vaddr_t)kernend,
	    mem_clusters, mem_cluster_cnt, NULL, 0);

	/* We can now no longer use bootinfo. */
	bootinfo = NULL;

	/*
	 * Initialize mips version-dependent DMA handlers.
	 */
	sgimips_bus_dma_init();

	/*
	 * Walk the component tree and count the number of CPUs
	 * present in the system.  ARCS64 tree walking is not implemented
	 * yet; ip30_arcs64_init() sets the known single CPU.
	 */
#ifndef XIRI_IP30
	arcbios_tree_walk(sgimips_count_cpus, NULL);
#endif

	/*
	 * Initialize error message buffer (at end of core).
	 */
	mips_init_msgbuf();

	pmap_bootstrap();
#ifdef XIRI_IP30
	printf("XIRI_M2_PMAP_DONE\n");
#endif

	/*
	 * Allocate uarea for lwp0 and set it.
	 */
	mips_init_lwp0_uarea();
#ifdef XIRI_IP30
	printf("XIRI_M2_UAREA_DONE\n");
#endif

#ifdef XIRI_IP30_M2_TLB_TEST
	ip30_tlb_refill_test();
#endif

#ifdef XIRI_IP30_M2
	printf("XIRI_IP30_M2_MEMORY_BEGIN\n");
	printf("machine=IP%d ncpus=%d physmem=%lu arcsmem=%d clusters=%d\n",
	    mach_type, ncpus, (unsigned long)physmem, arcsmem, mem_cluster_cnt);
	printf("kernel_text=%p kernend=%p\n", kernel_text, (void *)kernend);
	printf("xkphys_cached=%#lx pg_cached=%#x tlbmisses=%lu\n",
	    (unsigned long)mips_options.mips3_xkphys_cached,
	    (unsigned int)mips_options.mips3_pg_cached,
	    (unsigned long)curcpu()->ci_ev_tlbmisses.ev_count);
	printf("cpu_info_cell=%#lx slowfault_cell=%#lx\n",
	    (unsigned long)*SGIMIPS_ARCS64_CPU_INFO_CELL,
	    (unsigned long)*SGIMIPS_ARCS64_SLOWFAULT_CELL);
	for (i = 0; i < mem_cluster_cnt; i++) {
		printf("cluster[%u]=%#"PRIxPADDR"+%#"PRIxVSIZE"\n", i,
		    mem_clusters[i].start, mem_clusters[i].size);
	}
	printf("XIRI_IP30_M2_MEMORY_OK\n");
#ifdef XIRI_IP30_M2A_STOP
	xiri_ip30_halt();
#endif
#endif
}

#ifdef XIRI_IP30_M2_TLB_TEST
#ifdef XIRI_IP30_M2_SLOWFAULT_TEST
extern uint64_t xiri_ip30_slowfault_load(vaddr_t);
#endif

static void
ip30_tlb_refill_test(void)
{
	const vaddr_t va = VM_MIN_KERNEL_ADDRESS;
	const paddr_t pa = 0x30000000;
	const uint64_t magic = 0x5849524933305442ULL;
	uint64_t before, after, value;

	*(volatile uint64_t *)MIPS_PHYS_TO_XKPHYS_UNCACHED(pa) = magic;
	pmap_kenter_pa(va, pa, VM_PROT_READ | VM_PROT_WRITE, PMAP_NOCACHE);
	pmap_update(pmap_kernel());
	tlb_invalidate_addr(va, KERNEL_PID);

	before = curcpu()->ci_ev_tlbmisses.ev_count;
	printf("XIRI_IP30_M2_TLB_REFILL_BEGIN va=%p pa=%#lx\n",
	    (void *)va, (unsigned long)pa);
	value = *(volatile uint64_t *)va;
	after = curcpu()->ci_ev_tlbmisses.ev_count;
	printf("XIRI_IP30_M2_TLB_REFILL value=%#lx before=%lu after=%lu\n",
	    (unsigned long)value, (unsigned long)before,
	    (unsigned long)after);
	if (value == magic && after > before)
		printf("XIRI_IP30_M2_TLB_REFILL_OK\n");
	else
		printf("XIRI_IP30_M2_TLB_REFILL_FAILED\n");
	pmap_kremove(va, PAGE_SIZE);
	pmap_update(pmap_kernel());

#ifdef XIRI_IP30_M2_SLOWFAULT_TEST
	printf("XIRI_IP30_M2_SLOWFAULT_BEGIN va=%p\n",
	    (void *)(VM_MAX_KERNEL_ADDRESS - PAGE_SIZE));
	(void)xiri_ip30_slowfault_load(VM_MAX_KERNEL_ADDRESS - PAGE_SIZE);
	printf("XIRI_IP30_M2_SLOWFAULT_UNEXPECTED\n");
#endif
}
#endif

void
sgimips_count_cpus(struct arcbios_component *node,
    struct arcbios_treewalk_context *atc)
{

	switch (node->Class) {
	case COMPONENT_CLASS_ProcessorClass:
		if (node->Type == COMPONENT_TYPE_CPU)
			ncpus++;
		break;

	default:
		break;
	}
}

/*
 * Allocate memory for variable-sized tables.
 */
void
cpu_startup(void)
{
	vaddr_t minaddr, maxaddr;
	char pbuf[9];

#ifdef BOOTINFO_DEBUG
	if (bootinfo_msg)
		printf(bootinfo_msg);
#endif

	printf("%s%s", copyright, version);

	format_bytes(pbuf, sizeof(pbuf), ctob(physmem));
	printf("total memory = %s\n", pbuf);
	format_bytes(pbuf, sizeof(pbuf), ctob(arcsmem));
	printf("(%s reserved for ARCS)\n", pbuf);

	minaddr = 0;
	/*
	 * Allocate a submap for physio.
	 */
	phys_map = uvm_km_suballoc(kernel_map, &minaddr, &maxaddr,
				    VM_PHYS_SIZE, 0, false, NULL);

	/*
	 * (No need to allocate an mbuf cluster submap.  Mbuf clusters
	 * are allocated via the pool allocator, and we use KSEG to
	 * map those pages.)
	 */
	format_bytes(pbuf, sizeof(pbuf), ptoa(uvm_availmem(false)));
	printf("avail memory = %s\n", pbuf);
}

int	waittime = -1;

void
cpu_reboot(int howto, char *bootstr)
{
	/* Take a snapshot before clobbering any registers. */
	savectx(curpcb);

	if (cold) {
		howto |= RB_HALT;
		goto haltsys;
	}

	/* If "always halt" was specified as a boot flag, obey. */
	if (boothowto & RB_HALT)
		howto |= RB_HALT;

	boothowto = howto;
	if ((howto & RB_NOSYNC) == 0 && (waittime < 0)) {
		waittime = 0;
		vfs_shutdown();
	}

	/* Clear and disable watchdog timer. */
	(void)(*platform.watchdog_disable)();

	splhigh();

	if (howto & RB_DUMP)
		dumpsys();

haltsys:

	doshutdownhooks();

	pmf_system_shutdown(boothowto);

	/*
	 * Calling arcbios_PowerDown() results in a "CP1 unusable trap"
	 * which lands me back in DDB, at least on my Indy.  So, enable
	 * the FPU before asking the PROM to power down to avoid this..
	 * It seems to want the FPU to play the `poweroff tune' 8-/
	 */
	if ((howto & RB_POWERDOWN) == RB_POWERDOWN) {
		/* Set CP1 usable bit in SR */
	 	mips_cp0_status_write(mips_cp0_status_read() |
					MIPS_SR_COP_1_BIT);

		printf("powering off...\n\n");
		delay(500000);
#if NMCCLOCK_MACE > 0
		if (mach_type == MACH_SGI_IP32) {
			mcclock_poweroff();
		} else
#endif
#ifdef XIRI_IP30
			ip30_arcs64_powerdown();
#else
			arcbios_PowerDown();
#endif
		printf("WARNING: powerdown failed\n");
		/*
		 * RB_POWERDOWN implies RB_HALT... fall into it...
		 */
	}

	if (howto & RB_HALT) {
		printf("halting...\n\n");
#ifdef XIRI_IP30
		ip30_arcs64_enter();
#else
		arcbios_EnterInteractiveMode();
#endif
	}

	printf("rebooting...\n\n");
#if NCRIME > 0
	if (mach_type == MACH_SGI_IP32) {
		crime_reboot();
	} else
#endif
#ifdef XIRI_IP30
		ip30_arcs64_reboot();
#else
		arcbios_Reboot();
#endif

	for (;;);
}

void delay(unsigned long n)
{
	register int __N = curcpu()->ci_divisor_delay * n;

	do {
		__asm("addiu %0,%1,-1" : "=r" (__N) : "0" (__N));
	} while (__N > 0);
}

/*
 * IP12 appears to be buggy and unable to reliably support badaddr.
 * Approximately 1.8% of the time a false negative (bad address said to
 * be good) is generated and we stomp on invalid registers. Testing has
 * not shown false positives, nor consecutive false negatives to occur.
 */
static int
badaddr_workaround(void *addr, size_t size)
{
	int i, bad;

	for (i = bad = 0; i < 100; i++) {
		if (badaddr(addr, size))
			bad++;
	}

	/* false positives appear not to occur */
	return (bad != 0);
}

/*
 *  Ensure all platform vectors are always initialized.
 */
static void
unimpl_bus_reset(void)
{

	panic("target init didn't set bus_reset");
}

static void
unimpl_cons_init(void)
{

	panic("target init didn't set cons_init");
}

static void *
unimpl_intr_establish(int level, int ipl, int (*handler) (void *), void *arg)
{
	panic("target init didn't set intr_establish");
	return NULL;
}

static void
unimpl_intr(vaddr_t pc, uint32_t status, uint32_t pending)
{
	printf("spurious interrupt pending %#x\n", pending);
}

static unsigned long
nulllong(void)
{
	printf("nulllong\n");
	return (0);
}

static void
nullvoid(void)
{
	printf("nullvoid\n");
	return;
}

void *
lookup_bootinfo(int type)
{
	struct btinfo_common *bt;
	uint8_t *bip;

	/* check for a bootinfo record first */
	if (bootinfo == NULL)
		return NULL;

	bip = bootinfo;
	do {
		bt = (struct btinfo_common *)bip;
		if (bt->type == type)
			return (void *)bt;
		bip += bt->next;
	} while (bt->next != 0 &&
	    bt->next < BOOTINFO_SIZE /* sanity */ &&
	    (size_t)bip < (size_t)bootinfo + BOOTINFO_SIZE);

	return NULL;
}

#if defined(DDB) || defined(KGDB)

void ddb_trap_hook(int where)
{
	switch (where) {
	case 1:		/* Entry to DDB, turn watchdog off */
		(void)(*platform.watchdog_disable)();
		break;

	case 0:		/* Exit from DDB, turn watchdog back on */
		(void)(*platform.watchdog_enable)();
		break;
	}
}

#endif

void mips_machdep_cache_config(void)
{

#ifdef ARCBIOS64
	/* R10000 describes its secondary cache directly in CP0 Config. */
	if (MIPS_PRID_IMPL(mips_options.mips_cpu_id) == MIPS_R10000) {
		const uint32_t config = mips3_cp0_config_read();
		mips_cache_info.mci_sdcache_size = (512 * 1024) <<
		    __SHIFTOUT(config, MIPS4_CONFIG_SS_MASK);
		mips_cache_info.mci_sdcache_line_size =
		    MIPS4_CONFIG_CACHE_L2_LSIZE(config);
		mips_cache_info.mci_sdcache_ways = 2;
		mips_cache_info.mci_scache_unified = true;
		return;
	}
#endif
#ifndef XIRI_IP30
	arcbios_tree_walk(mips_machdep_find_l2cache, NULL);
#else
	/*
	 * The Octane PROM loads and enters the kernel at 0xa800... (CCA 5).
	 * Align NetBSD's cached XKPHYS and TLB aliases with that entry alias
	 * so cache and pmap code never mixes CCA 5 and CCA 3 for the same
	 * physical page.  The ARCS64 component tree is not parsed yet, so L2
	 * discovery is deferred.
	 */
	mips_options.mips3_xkphys_cached = MIPS_PHYS_TO_XKPHYS(5, 0);
	mips_options.mips3_pg_cached = MIPS3_CCA_TO_PG(5);
#endif

	switch (MIPS_PRID_IMPL(mips_options.mips_cpu_id)) {
#if defined(INDY_R4600_CACHE)
	case MIPS_R4600:
		/*
		 * R4600 is on Indy-class machines only.  Disable and
		 * flush pcache.
		 */
		mips_cache_info.mci_sdcache_size = 0;
		mips_cache_info.mci_sdcache_line_size = 0;
		ip22_sdcache_disable();
		break;
#endif
#if defined(MIPS3)
	case MIPS_R5000:
	case MIPS_RM5200:
		r5k_enable_sdcache();
		break;
#endif
	}
}

void
mips_machdep_find_l2cache(struct arcbios_component *comp, struct arcbios_treewalk_context *atc)
{
	struct mips_cache_info * const mci = &mips_cache_info;
	device_t self = atc->atc_cookie;

	if (comp->Class != COMPONENT_CLASS_CacheClass)
		return;

	switch (comp->Type) {
	case COMPONENT_TYPE_SecondaryICache:
		panic("%s: split L2 cache", device_xname(self));
	case COMPONENT_TYPE_SecondaryDCache:
	case COMPONENT_TYPE_SecondaryCache:
		mci->mci_sdcache_size = COMPONENT_KEY_Cache_CacheSize(comp->Key);
		mci->mci_sdcache_line_size =
		    COMPONENT_KEY_Cache_LineSize(comp->Key);
		/* XXX */
		mci->mci_sdcache_ways = 1;
		break;
	}
}
