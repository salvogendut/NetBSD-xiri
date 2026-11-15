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
 * Early polled IOC3 UART A console for Octane/IP30.
 *
 * This maps the Bridge's short window for PCI slot 2 while the PROM's
 * IOC3 BAR0/window assignment is still in place, validates that assignment,
 * emits a marker directly through the 16550 UART, then installs it as cn_tab.
 * XIRI_IP30_M3B3 adds the interrupt-driven receive path and its routing.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_xiri_ip30.h"

#include <sys/param.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/systm.h>

#include <sys/bus.h>
#include <dev/cons.h>
#include <machine/autoconf.h>
#include <machine/intr.h>
#include <machine/machtype.h>
#include <machine/sysconf.h>

#include <dev/pci/pcireg.h>

#include <sgimips/dev/bridgereg.h>
#include <sgimips/dev/bridgevar.h>
#include <sgimips/dev/heartvar.h>

#define	IOC3_WINDOW_BASE	0x1f600000
#define	IOC3_WINDOW_SIZE	0x00040000
#define	IOC3_UART_A		0x00020178
#define	IOC3_INT_STATUS		0x0000001c
#define	IOC3_INT_ENABLE_SET	0x00000020
#define	IOC3_SERIAL_A		(1U << 6)

#define	COM_RBR			0
#define	COM_THR			0
#define	COM_IER			1
#define	COM_MCR			4
#define	COM_LSR			5
#define	COM_LSR_DR		0x01
#define	COM_LSR_THRE		0x20
#define	COM_IER_RX		0x01
#define	COM_MCR_LOOP		0x10

#ifdef XIRI_IP30_M3B3
#define	IOC3_SUPERIO_PIN	4
#define	IOC3_HEART_VECTOR	16
#define	IOC3_RX_RING_SIZE	256
#define	IOC3_RX_TEST_CHAR	0x5a
#define	IOC3_POLL_TEST_CHAR	0x59
#endif

struct ioc3early_softc {
	device_t sc_dev;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
#ifdef XIRI_IP30_M3B3
	void *sc_ih;
	uint64_t sc_ticks_base;
	unsigned int sc_intr_count;
	unsigned int sc_rx_head;
	unsigned int sc_rx_tail;
	unsigned int sc_rx_overruns;
	uint8_t sc_rx_ring[IOC3_RX_RING_SIZE];
#endif
};

static int	ioc3early_match(device_t, cfdata_t, void *);
static void	ioc3early_attach(device_t, device_t, void *);
static int	ioc3early_getc(dev_t);
static void	ioc3early_putc(dev_t, int);
static void	ioc3early_puts(struct ioc3early_softc *, const char *);
#ifdef XIRI_IP30_M3B3
static unsigned int ioc3early_drain(struct ioc3early_softc *);
static int	ioc3early_intr(void *);
static void	ioc3early_rx_test(void *);
static void	ioc3early_m3b3_failed(const char *);
#endif

CFATTACH_DECL_NEW(ioc3early, sizeof(struct ioc3early_softc),
    ioc3early_match, ioc3early_attach, NULL, NULL);

static struct ioc3early_softc *ioc3early_sc;

static struct consdev ioc3early_cn = {
	.cn_getc = ioc3early_getc,
	.cn_putc = ioc3early_putc,
	.cn_pollc = nullcnpollc,
	.cn_dev = NODEV,
	.cn_pri = CN_NORMAL,
};

static int
ioc3early_match(device_t parent, cfdata_t match, void *aux)
{

	return mach_type == MACH_SGI_IP30;
}

static void
ioc3early_putc(dev_t dev, int c)
{
	struct ioc3early_softc *sc = ioc3early_sc;
	int i;

	if (sc == NULL)
		return;
	for (i = 0; i < 1000000; i++) {
		if (bus_space_read_1(sc->sc_st, sc->sc_sh,
		    IOC3_UART_A + COM_LSR) & COM_LSR_THRE)
			break;
	}
	bus_space_write_1(sc->sc_st, sc->sc_sh, IOC3_UART_A + COM_THR, c);
}

static int
ioc3early_getc(dev_t dev)
{
	struct ioc3early_softc *sc = ioc3early_sc;
#ifdef XIRI_IP30_M3B3
	int s, c;

	if (sc == NULL)
		return -1;
	s = splhigh();
	if (sc->sc_rx_head == sc->sc_rx_tail)
		(void)ioc3early_drain(sc);
	if (sc->sc_rx_head != sc->sc_rx_tail) {
		c = sc->sc_rx_ring[sc->sc_rx_tail];
		sc->sc_rx_tail = (sc->sc_rx_tail + 1) %
		    IOC3_RX_RING_SIZE;
	} else {
		c = -1;
	}
	splx(s);
	return c;
#else
	int i;

	if (sc == NULL)
		return -1;
	for (i = 0; i < 1000000; i++) {
		if (bus_space_read_1(sc->sc_st, sc->sc_sh,
		    IOC3_UART_A + COM_LSR) & COM_LSR_DR)
			return bus_space_read_1(sc->sc_st, sc->sc_sh,
			    IOC3_UART_A + COM_RBR);
	}
	return -1;
#endif
}

static void
ioc3early_puts(struct ioc3early_softc *sc, const char *s)
{

	while (*s != '\0')
		ioc3early_putc(0, *s++);
}

#ifdef XIRI_IP30_M3B3
static unsigned int
ioc3early_drain(struct ioc3early_softc *sc)
{
	unsigned int n, next;
	uint8_t c;

	for (n = 0; n < IOC3_RX_RING_SIZE; n++) {
		if ((bus_space_read_1(sc->sc_st, sc->sc_sh,
		    IOC3_UART_A + COM_LSR) & COM_LSR_DR) == 0)
			break;
		c = bus_space_read_1(sc->sc_st, sc->sc_sh,
		    IOC3_UART_A + COM_RBR);
		next = (sc->sc_rx_head + 1) % IOC3_RX_RING_SIZE;
		if (next == sc->sc_rx_tail) {
			sc->sc_rx_overruns++;
		} else {
			sc->sc_rx_ring[sc->sc_rx_head] = c;
			sc->sc_rx_head = next;
		}
	}
	return n;
}

static int
ioc3early_intr(void *arg)
{
	struct ioc3early_softc *sc = arg;

	(void)ioc3early_drain(sc);
	sc->sc_intr_count++;
	return 1;
}

static void
ioc3early_m3b3_failed(const char *what)
{

	printf("XIRI_IP30_M3B3_FAILED %s\n", what);
}

static void
ioc3early_rx_test(void *arg)
{
	struct ioc3early_softc *sc = arg;
	unsigned int base, after;
	uint64_t ticks;
	uint8_t mcr;
	int s, c;

	ticks = getticks() - sc->sc_ticks_base;
	if (ticks == 0) {
		ioc3early_m3b3_failed("timer");
		return;
	}

	base = sc->sc_intr_count;
	s = splvm();
	mcr = bus_space_read_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_MCR);
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_MCR, mcr | COM_MCR_LOOP);
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_THR, IOC3_RX_TEST_CHAR);
	delay(1000);
	if ((bus_space_read_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_LSR) & COM_LSR_DR) == 0) {
		bus_space_write_1(sc->sc_st, sc->sc_sh,
		    IOC3_UART_A + COM_MCR, mcr);
		splx(s);
		ioc3early_m3b3_failed("inject");
		return;
	}
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_MCR, mcr & ~COM_MCR_LOOP);
	delay(1000);
	if (sc->sc_intr_count != base) {
		splx(s);
		ioc3early_m3b3_failed("masked");
		return;
	}
	if ((xbridge_intr_status() & (1U << IOC3_SUPERIO_PIN)) == 0) {
		splx(s);
		ioc3early_m3b3_failed("pending");
		return;
	}
	splx(s);
	delay(10000);
	after = sc->sc_intr_count;
	if (after == base) {
		ioc3early_m3b3_failed("delivered");
		return;
	}
	if (after != base + 1) {
		ioc3early_m3b3_failed("storm");
		return;
	}
	if ((xbridge_intr_status() & (1U << IOC3_SUPERIO_PIN)) != 0 ||
	    (bus_space_read_4(sc->sc_st, sc->sc_sh,
	    IOC3_INT_STATUS) & IOC3_SERIAL_A) != 0) {
		ioc3early_m3b3_failed("drain");
		return;
	}
	c = ioc3early_getc(0);
	if (c != IOC3_RX_TEST_CHAR) {
		ioc3early_m3b3_failed("console");
		return;
	}

	s = splvm();
	mcr = bus_space_read_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_MCR);
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_MCR, mcr | COM_MCR_LOOP);
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_THR, IOC3_POLL_TEST_CHAR);
	delay(1000);
	if ((bus_space_read_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_LSR) & COM_LSR_DR) == 0) {
		bus_space_write_1(sc->sc_st, sc->sc_sh,
		    IOC3_UART_A + COM_MCR, mcr);
		splx(s);
		ioc3early_m3b3_failed("polled_inject");
		return;
	}
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_MCR, mcr & ~COM_MCR_LOOP);
	c = ioc3early_getc(0);
	if (c != IOC3_POLL_TEST_CHAR) {
		splx(s);
		ioc3early_m3b3_failed("polled");
		return;
	}
	if ((xbridge_intr_status() & (1U << IOC3_SUPERIO_PIN)) != 0 ||
	    (bus_space_read_4(sc->sc_st, sc->sc_sh,
	    IOC3_INT_STATUS) & IOC3_SERIAL_A) != 0) {
		splx(s);
		ioc3early_m3b3_failed("polled_drain");
		return;
	}
	splx(s);
	delay(10000);
	if (sc->sc_intr_count != after) {
		ioc3early_m3b3_failed("storm");
		return;
	}
	if (sc->sc_rx_overruns != 0) {
		ioc3early_m3b3_failed("overrun");
		return;
	}
	if ((xbridge_intr_status() & BRIDGE_INTR_ERROR_MASK) != 0) {
		ioc3early_m3b3_failed("bridge");
		return;
	}
	printf("XIRI_IP30_M3B3_RX_OK masked=deferred delivered=1 "
	    "drained=1 polled=1 storm=0\n");
	printf("XIRI_IP30_M3B3_SERIAL_OK ticks=%llu bridge_errors=0 "
	    "overruns=0\n", (unsigned long long)ticks);
}
#endif

static void
ioc3early_attach(device_t parent, device_t self, void *aux)
{
	struct mainbus_attach_args *ma = aux;
	struct ioc3early_softc *sc = device_private(self);
	bus_space_tag_t bst;
	bus_space_handle_t bsh;
	uint32_t bar0, window;

	sc->sc_dev = self;
	sc->sc_st = normal_memt;
	if (bus_space_map(sc->sc_st, ma->ma_addr, IOC3_WINDOW_SIZE,
	    BUS_SPACE_MAP_LINEAR, &sc->sc_sh) != 0) {
		printf("XIRI_IP30_M3B2_IOC3_FAILED map\n");
		return;
	}
	ioc3early_sc = sc;

	/* Validate the PROM's IOC3 BAR0 and Bridge device window 2. */
	bst = normal_memt;
	if (bus_space_map(bst, 0x1f000000 + BRIDGE_CONFIG0_BASE +
	    2 * BRIDGE_SLOT_STRIDE, 0x100, BUS_SPACE_MAP_LINEAR, &bsh) == 0) {
		bar0 = bus_space_read_4(bst, bsh, PCI_BAR0);
		bus_space_unmap(bst, bsh, 0x100);
	} else {
		bar0 = 0xffffffffU;
	}
	if (bus_space_map(bst, 0x1f000000, 0x1000,
	    BUS_SPACE_MAP_LINEAR, &bsh) == 0) {
		window = bus_space_read_4(bst, bsh,
		    BRIDGE_DEVICE0 + 2 * BRIDGE_DEVICE_STRIDE);
		bus_space_unmap(bst, bsh, 0x1000);
	} else {
		window = 0xffffffffU;
	}
	aprint_normal(": IOC3 BAR0 %#x window2 %#x\n", bar0, window);
	if (bar0 == 0 || bar0 == 0xffffffffU || (bar0 & 0xf) != 0 ||
	    (window & 0xfff) != ((bar0 >> 20) & 0xfff) ||
	    (window & (1U << 12)) == 0) {
		printf("XIRI_IP30_M3B2_IOC3_FAILED bar/window\n");
		return;
	}

	/* Direct UART proof before changing cn_tab. */
	ioc3early_puts(sc, "XIRI_IP30_M3B2_IOC3_OK\r\n");

	cn_tab = &ioc3early_cn;
	printf("XIRI_IP30_M3B2_IOC3_CONSOLE\r\n");

#ifdef XIRI_IP30_M3B3
	if (xbridge_intr_route(IOC3_SUPERIO_PIN, IOC3_HEART_VECTOR) != 0) {
		ioc3early_m3b3_failed("route");
		return;
	}
	sc->sc_ih = platform.intr_establish(IOC3_HEART_VECTOR, IPL_VM,
	    ioc3early_intr, sc);
	if (sc->sc_ih == NULL) {
		ioc3early_m3b3_failed("establish");
		return;
	}
	(void)ioc3early_drain(sc);
	bus_space_write_1(sc->sc_st, sc->sc_sh,
	    IOC3_UART_A + COM_IER, COM_IER_RX);
	bus_space_write_4(sc->sc_st, sc->sc_sh,
	    IOC3_INT_ENABLE_SET, IOC3_SERIAL_A);
	if (xbridge_intr_enable(IOC3_SUPERIO_PIN, 1) != 0) {
		ioc3early_m3b3_failed("enable");
		return;
	}
	printf("XIRI_IP30_M3B3_ROUTE_OK pin=%d vector=%d\n",
	    IOC3_SUPERIO_PIN, IOC3_HEART_VECTOR);
	sc->sc_ticks_base = getticks();
	heart_set_post_test(ioc3early_rx_test, sc);
#endif
}
