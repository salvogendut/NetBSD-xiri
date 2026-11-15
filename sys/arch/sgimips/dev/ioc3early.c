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
 * The interrupt-driven IOC3 path follows in a later checkpoint.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <sys/bus.h>
#include <dev/cons.h>
#include <machine/autoconf.h>
#include <machine/machtype.h>

#include <dev/pci/pcireg.h>

#include <sgimips/dev/bridgereg.h>

#define	IOC3_WINDOW_BASE	0x1f600000
#define	IOC3_WINDOW_SIZE	0x00040000
#define	IOC3_UART_A		0x00020178

#define	COM_LSR			5
#define	COM_LSR_DR		0x01
#define	COM_LSR_THRE		0x20
#define	COM_RBR			0
#define	COM_THR			0

struct ioc3early_softc {
	device_t sc_dev;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
};

static int	ioc3early_match(device_t, cfdata_t, void *);
static void	ioc3early_attach(device_t, device_t, void *);
static int	ioc3early_getc(dev_t);
static void	ioc3early_putc(dev_t, int);
static void	ioc3early_puts(struct ioc3early_softc *, const char *);

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
}

static void
ioc3early_puts(struct ioc3early_softc *sc, const char *s)
{

	while (*s != '\0')
		ioc3early_putc(0, *s++);
}

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
}
