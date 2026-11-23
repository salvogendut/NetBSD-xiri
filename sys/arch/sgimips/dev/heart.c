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
 * SGI Octane/IP30 HEART processor-interface interrupt controller.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/device.h>
#include <sys/callout.h>
#include <sys/evcnt.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <sys/bus.h>
#include <machine/autoconf.h>
#include <machine/intr.h>
#include <machine/machtype.h>
#include <machine/sysconf.h>

#include <sgimips/dev/heartreg.h>
#include <sgimips/dev/heartvar.h>

#define	HEART_NINTR		64

struct heart_intrhand {
	int	(*ih_fun)(void *);
	void	*ih_arg;
	int	ih_ipl;
	struct evcnt ih_evcnt;
	char	ih_evname[8];
};

struct heart_softc {
	device_t sc_dev;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
	struct heart_intrhand sc_intr[HEART_NINTR];
	struct callout sc_clock_callout;
	uint64_t sc_heart_base;
	int sc_heart_ok;
	volatile unsigned int sc_test_count;
};

static int	heart_match(device_t, cfdata_t, void *);
static void	heart_attach(device_t, device_t, void *);
static void	*heart_intr_establish(int, int, int (*)(void *), void *);
static void	heart_intr0(vaddr_t, uint32_t, uint32_t);
static void	heart_intr1(vaddr_t, uint32_t, uint32_t);
static void	heart_intr2(vaddr_t, uint32_t, uint32_t);
static void	heart_intr3(vaddr_t, uint32_t, uint32_t);
static void	heart_intr4(vaddr_t, uint32_t, uint32_t);
static void	heart_intr_dispatch(int, vaddr_t, uint32_t, uint32_t);
static void	heart_bus_reset(void);
static void	heart_watchdog_stub(void);
static void	heart_clock_check(void *);
static int	heart_test_intr(void *);
static void	heart_intr_delivery_test(struct heart_softc *);

CFATTACH_DECL_NEW(heart, sizeof(struct heart_softc),
    heart_match, heart_attach, NULL, NULL);

static struct heart_softc *heart_sc;
#define	HEART_POST_TESTS	4
static struct {
	heart_post_test_fn func;
	void *arg;
} heart_post_test[HEART_POST_TESTS];

void
heart_set_post_test(heart_post_test_fn func, void *arg)
{
	int i;

	for (i = 0; i < HEART_POST_TESTS; i++) {
		if (heart_post_test[i].func == NULL) {
			heart_post_test[i].func = func;
			heart_post_test[i].arg = arg;
			return;
		}
	}
	panic("%s: no post-test slot", __func__);
}

int
heart_intr_pending(int vector)
{
	struct heart_softc *sc = heart_sc;
	uint64_t bit, isr, imr;

	if (sc == NULL || vector < 0 || vector >= HEART_NINTR)
		return 0;
	bit = 1ULL << vector;
	isr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_ISR);
	imr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMR0);
	return (isr & imr & bit) != 0;
}

void
heart_intr_set(int vector)
{
	struct heart_softc *sc = heart_sc;

	if (sc == NULL || vector < 0 || vector >= HEART_NINTR)
		return;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_SET,
	    1ULL << vector);
}

void
heart_intr_clear(int vector)
{
	struct heart_softc *sc = heart_sc;

	if (sc == NULL || vector < 0 || vector >= HEART_NINTR)
		return;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_CLEAR,
	    1ULL << vector);
}

static const uint64_t heart_level_mask[5] = {
	0x000000000000ffffULL,	/* ISR  0..15  -> IP2 */
	0x00000000ffff0000ULL,	/* ISR 16..31  -> IP3 */
	0x0003ffff00000000ULL,	/* ISR 32..49  -> IP4 */
	0x0004000000000000ULL,	/* ISR 50      -> IP5 */
	0xfff8000000000000ULL,	/* ISR 51..63  -> IP6 */
};

static int
heart_match(device_t parent, cfdata_t match, void *aux)
{

	return mach_type == MACH_SGI_IP30;
}

static void
heart_attach(device_t parent, device_t self, void *aux)
{
	struct mainbus_attach_args *ma = aux;
	struct heart_softc *sc = device_private(self);
	uint64_t bit, imsr, isr, status;
	unsigned int i;
	int ok;

	sc->sc_dev = self;
	sc->sc_st = normal_memt;
	if (bus_space_map(sc->sc_st, ma->ma_addr, HEART_PIU_SIZE,
	    BUS_SPACE_MAP_LINEAR, &sc->sc_sh) != 0)
		panic("%s: unable to map HEART registers", __func__);
	heart_sc = sc;

	status = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_STATUS);
	aprint_normal(": HEART, status %#" PRIx64 "\n", status);

	/* Start with every HEART interrupt masked and no stale pending bits. */
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR0, 0);
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR1, 0);
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR2, 0);
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR3, 0);
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_CLEAR, ~0ULL);

	for (i = 0; i < HEART_NINTR; i++) {
		sc->sc_intr[i].ih_fun = NULL;
		sc->sc_intr[i].ih_arg = NULL;
	}
	for (i = 0; i < HEART_NINTR; i++) {
		snprintf(sc->sc_intr[i].ih_evname,
		    sizeof(sc->sc_intr[i].ih_evname), "%u", i);
		evcnt_attach_dynamic(&sc->sc_intr[i].ih_evcnt,
		    EVCNT_TYPE_INTR, NULL, "heart",
		    sc->sc_intr[i].ih_evname);
	}

	/*
	 * Mask/pending/ackself-test.  Interrupts are still disabled during
	 * autoconfiguration.  Leave IMR0 clear and ISR clear on return.
	 */
	bit = 1ULL << 0;
	ok = 1;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_SET, bit);
	isr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_ISR);
	imsr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMSR);
	if ((isr & bit) == 0 || (imsr & bit) != 0)
		ok = 0;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR0, bit);
	imsr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMSR);
	if ((imsr & bit) == 0)
		ok = 0;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR0, 0);
	imsr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMSR);
	if ((imsr & bit) != 0)
		ok = 0;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_CLEAR, bit);
	isr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_ISR);
	if ((isr & bit) != 0)
		ok = 0;

	platform.bus_reset = heart_bus_reset;
	platform.watchdog_reset = heart_watchdog_stub;
	platform.watchdog_disable = heart_watchdog_stub;
	platform.watchdog_enable = heart_watchdog_stub;
	sc->sc_heart_ok = ok;
	platform.intr_establish = heart_intr_establish;
	platform.intr0 = heart_intr0;
	platform.intr1 = heart_intr1;
	platform.intr2 = heart_intr2;
	platform.intr3 = heart_intr3;
	platform.intr4 = heart_intr4;

	sc->sc_heart_base = bus_space_read_8(sc->sc_st, sc->sc_sh,
	    HEART_COUNT) & HEART_COUNT_MASK;
	callout_init(&sc->sc_clock_callout, 0);
	callout_setfunc(&sc->sc_clock_callout, heart_clock_check, sc);
	callout_schedule(&sc->sc_clock_callout, hz / 20);

	aprint_normal_dev(self, "mask/pending/ack %s\n",
	    ok ? "passed" : "FAILED");
	if (ok)
		printf("XIRI_IP30_M2B_HEART_OK\n");
	else
		printf("XIRI_IP30_M2B_HEART_FAILED\n");
}

static void *
heart_intr_establish(int vector, int ipl, int (*func)(void *), void *arg)
{
	struct heart_softc *sc = heart_sc;
	uint64_t mask;

	if (sc == NULL)
		panic("%s: not attached", __func__);
	if (vector < 0 || vector >= HEART_NINTR)
		panic("%s: invalid vector %d", __func__, vector);
	if (ipl != IPL_VM) {
		aprint_error_dev(sc->sc_dev,
		    "vector %d: unsupported IPL %d (use IPL_VM)\n",
		    vector, ipl);
		return NULL;
	}
	if (sc->sc_intr[vector].ih_fun != NULL)
		return NULL;

	sc->sc_intr[vector].ih_fun = func;
	sc->sc_intr[vector].ih_arg = arg;
	sc->sc_intr[vector].ih_ipl = ipl;

	mask = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMR0);
	mask |= 1ULL << vector;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR0, mask);

	return &sc->sc_intr[vector];
}

void
heart_intr_disestablish(void *cookie)
{
	struct heart_softc *sc = heart_sc;
	struct heart_intrhand *ih = cookie;
	uint64_t mask;
	int vector;

	if (sc == NULL || ih == NULL || ih < sc->sc_intr ||
	    ih >= sc->sc_intr + HEART_NINTR)
		return;
	vector = ih - sc->sc_intr;
	mask = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMR0);
	mask &= ~(1ULL << vector);
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_IMR0, mask);
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_CLEAR,
	    1ULL << vector);
	ih->ih_fun = NULL;
	ih->ih_arg = NULL;
	ih->ih_ipl = 0;
}

static void
heart_watchdog_stub(void)
{
}

static int
heart_test_intr(void *arg)
{
	struct heart_softc *sc = arg;

	sc->sc_test_count++;
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_CLEAR, 1ULL << 0);
	return 1;
}

static void
heart_intr_delivery_test(struct heart_softc *sc)
{
	int s;
	void *cookie;

	sc->sc_test_count = 0;
	cookie = heart_intr_establish(0, IPL_VM, heart_test_intr, sc);
	if (cookie == NULL) {
		printf("XIRI_IP30_M2B_INTR_FAILED establish\n");
		return;
	}

	s = splvm();
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_SET, 1ULL << 0);
	delay(10000);
	if (sc->sc_test_count != 0) {
		splx(s);
		printf("XIRI_IP30_M2B_INTR_FAILED masked\n");
		return;
	}
	splx(s);
	delay(10000);
	if (sc->sc_test_count == 0) {
		printf("XIRI_IP30_M2B_INTR_FAILED delivered\n");
		return;
	}
	bus_space_write_8(sc->sc_st, sc->sc_sh, HEART_ISR_CLEAR, 1ULL << 0);
	printf("XIRI_IP30_M2B_INTR_OK masked=deferred delivered=%u\n",
	    sc->sc_test_count);
}

static void
heart_clock_check(void *arg)
{
	struct heart_softc *sc = arg;
	uint64_t count;
	int i;

	count = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_COUNT) &
	    HEART_COUNT_MASK;
	if (!sc->sc_heart_ok) {
		printf("XIRI_IP30_M2B_CLOCK_FAILED heart_test_failed\n");
		return;
	}
	printf("XIRI_IP30_M2B_CLOCK_OK heart_delta=%#" PRIx64
	    " clockints=%lu ticks=%llu\n", count - sc->sc_heart_base,
	    (unsigned long)curcpu()->ci_ev_count_compare.ev_count,
	    (unsigned long long)getticks());
	heart_intr_delivery_test(sc);
	for (i = 0; i < HEART_POST_TESTS; i++) {
		if (heart_post_test[i].func != NULL) {
			heart_post_test_fn func = heart_post_test[i].func;
			void *post_arg = heart_post_test[i].arg;

			heart_post_test[i].func = NULL;
			heart_post_test[i].arg = NULL;
			func(post_arg);
		}
	}
}

static void
heart_bus_reset(void)
{
	/*
	 * A real reset must only clear bus-error state.  Do not touch IMR
	 * or ISR here: autoconfiguration calls this hook after devices have
	 * established their masks.  Implement error clearing with hardware
	 * evidence before changing this.
	 */
}

static void
heart_intr_dispatch(int level, vaddr_t pc, uint32_t status, uint32_t ipending)
{
	struct heart_softc *sc = heart_sc;
	struct heart_intrhand *ih;
	uint64_t isr, imr, pending;
	int vector;

	isr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_ISR);
	imr = bus_space_read_8(sc->sc_st, sc->sc_sh, HEART_IMR0);
	pending = isr & imr & heart_level_mask[level];

	for (vector = 0; pending != 0; vector++, pending >>= 1) {
		if ((pending & 1) == 0)
			continue;
		ih = &sc->sc_intr[vector];
		ih->ih_evcnt.ev_count++;
		if (ih->ih_fun != NULL)
			(void)ih->ih_fun(ih->ih_arg);
		else
			printf("heart: unexpected interrupt %d\n", vector);
	}
}

static void
heart_intr0(vaddr_t pc, uint32_t status, uint32_t ipending)
{
	heart_intr_dispatch(0, pc, status, ipending);
}

static void
heart_intr1(vaddr_t pc, uint32_t status, uint32_t ipending)
{
	heart_intr_dispatch(1, pc, status, ipending);
}

static void
heart_intr2(vaddr_t pc, uint32_t status, uint32_t ipending)
{
	heart_intr_dispatch(2, pc, status, ipending);
}

static void
heart_intr3(vaddr_t pc, uint32_t status, uint32_t ipending)
{
	heart_intr_dispatch(3, pc, status, ipending);
}

static void
heart_intr4(vaddr_t pc, uint32_t status, uint32_t ipending)
{
	heart_intr_dispatch(4, pc, status, ipending);
}
