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

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/conf.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <sys/termios.h>
#include <sys/tty.h>

#include <dev/cons.h>
#include <dev/ic/comreg.h>
#include <dev/ic/comvar.h>
#include <machine/autoconf.h>
#include <machine/intr.h>
#include <machine/machtype.h>
#include <machine/sysconf.h>

#include <sgimips/dev/bridgereg.h>
#include <sgimips/dev/bridgevar.h>
#include <sgimips/dev/heartvar.h>

#ifndef CONMODE
#define	CONMODE	((TTYDEF_CFLAG & ~(CSIZE | CSTOPB | PARENB)) | CS8)
#endif

#define	IOC3_WINDOW_BASE	0x1f600000UL
#define	IOC3_WINDOW_SIZE	0x00040000
#define	IOC3_UART_A		0x1f620178UL
#define	IOC3_INT_ENABLE_SET	0x00000020
#define	IOC3_SERIAL_A		(1U << 6)
#define	IOC3_SUPERIO_PIN	4
#define	IOC3_HEART_VECTOR	16
#define	IOC3_UART_CRYSTAL	22118400U
#define	IOC3_UART_DEFAULT_FREQ	1843200
#define	IOC3_UART_DEFAULT_RATE	9600

extern const struct cdevsw com_cdevsw;

struct ioc3_com_softc {
	struct com_softc sc_com;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
	void *sc_ih;
};

static int	ioc3_com_match(device_t, cfdata_t, void *);
static void	ioc3_com_attach(device_t, device_t, void *);
static int	ioc3_com_uart_info(bus_space_tag_t, bus_space_handle_t,
		    int *, uint8_t *, uint8_t *, uint16_t *);
static uint16_t	ioc3_com_divisor(int, int);
static void	ioc3_com_failed(const char *);
static void	ioc3_com_post_test(void *);

CFATTACH_DECL_NEW(ioc3_com, sizeof(struct ioc3_com_softc),
    ioc3_com_match, ioc3_com_attach, NULL, NULL);

static int
ioc3_com_match(device_t parent, cfdata_t match, void *aux)
{
	struct mainbus_attach_args *ma = aux;

	return mach_type == MACH_SGI_IP30 && ma->ma_addr == IOC3_UART_A;
}

static int
ioc3_com_uart_info(bus_space_tag_t iot, bus_space_handle_t ioh, int *freqp,
    uint8_t *predivp, uint8_t *lcrp, uint16_t *divp)
{
	uint8_t lcr, predivider, after;

	lcr = bus_space_read_1(iot, ioh, com_lcr);
	bus_space_write_1(iot, ioh, com_lcr, lcr | LCR_DLAB);
	predivider = bus_space_read_1(iot, ioh, com_scratch);
	*divp = bus_space_read_1(iot, ioh, com_dlbl) |
	    bus_space_read_1(iot, ioh, com_dlbh) << 8;
	bus_space_write_1(iot, ioh, com_lcr, lcr);
	after = bus_space_read_1(iot, ioh, com_lcr);
	*predivp = predivider;
	*lcrp = lcr;
	if (predivider >= 2 && (predivider & 1) == 0)
		*freqp = IOC3_UART_CRYSTAL / (predivider / 2);
	else
		*freqp = IOC3_UART_DEFAULT_FREQ;
	return lcr == after;
}

static uint16_t
ioc3_com_divisor(int frequency, int rate)
{

	return ((frequency / 16) * 2 / rate + 1) / 2;
}

static void
ioc3_com_failed(const char *what)
{

	printf("XIRI_IP30_M3C1_FAILED %s\n", what);
}

static void
ioc3_com_attach(device_t parent, device_t self, void *aux)
{
	struct ioc3_com_softc *msc = device_private(self);
	struct com_softc *sc = &msc->sc_com;
	struct mainbus_attach_args *ma = aux;
	struct com_regs regs;
	bus_space_handle_t dummy, ioh;
	devmajor_t maj;
	uint8_t lcr, lcr_after, predivider, predivider_after;
	uint16_t divisor, divisor_after;
	int frequency;

	sc->sc_dev = self;
	sc->sc_type = COM_TYPE_16550_NOERS;
	memset(&dummy, 0, sizeof(dummy));
	com_init_regs(&regs, normal_memt, dummy, ma->ma_addr);
	if (bus_space_map(normal_memt, ma->ma_addr, COM_NPORTS, 0, &ioh) != 0) {
		ioc3_com_failed("uart_map");
		return;
	}
	if (ioc3_com_uart_info(normal_memt, ioh, &frequency, &predivider,
	    &lcr, &divisor) == 0) {
		bus_space_unmap(normal_memt, ioh, COM_NPORTS);
		ioc3_com_failed("lcr");
		return;
	}
	bus_space_unmap(normal_memt, ioh, COM_NPORTS);
	sc->sc_frequency = frequency;
	if (comcnattach1(&regs, IOC3_UART_DEFAULT_RATE, frequency,
	    COM_TYPE_16550_NOERS, CONMODE) != 0) {
		ioc3_com_failed("console");
		return;
	}
	if (com_is_console(normal_memt, ma->ma_addr, &ioh) == 0) {
		ioc3_com_failed("console_handle");
		return;
	}
	com_init_regs(&regs, normal_memt, ioh, ma->ma_addr);
	sc->sc_regs = regs;
	com_attach_subr(sc);
	if (ioc3_com_uart_info(normal_memt, ioh, &frequency,
	    &predivider_after, &lcr_after, &divisor_after) == 0 ||
	    predivider_after != predivider) {
		ioc3_com_failed("lcr_clock");
		return;
	}
	if (divisor_after != ioc3_com_divisor(frequency,
	    IOC3_UART_DEFAULT_RATE)) {
		ioc3_com_failed("divisor");
		return;
	}
	if (sc->sc_tty == NULL || !ISSET(sc->sc_hwflags, COM_HW_DEV_OK)) {
		ioc3_com_failed("attach");
		return;
	}

	msc->sc_st = normal_memt;
	if (bus_space_map(msc->sc_st, IOC3_WINDOW_BASE, IOC3_WINDOW_SIZE,
	    BUS_SPACE_MAP_LINEAR, &msc->sc_sh) != 0) {
		ioc3_com_failed("window_map");
		return;
	}
	if (cn_tab == NULL || cn_tab->cn_dev == NODEV) {
		ioc3_com_failed("cn_dev");
		return;
	}
	maj = cdevsw_lookup_major(&com_cdevsw);
	if (maj <= 0 || cdevsw_lookup(cn_tab->cn_dev) != &com_cdevsw ||
	    minor(cn_tab->cn_dev) != device_unit(self) ||
	    sc->sc_tty->t_dev != cn_tab->cn_dev) {
		ioc3_com_failed("tty_dev");
		return;
	}
	printf("XIRI_IP30_M3C1_UART_OK lcr=%#x predivider=%u "
	    "frequency=%d divisor=%u rate=%d\n", lcr_after, predivider_after,
	    frequency, divisor_after, IOC3_UART_DEFAULT_RATE);
	printf("XIRI_IP30_M3C1_TTY_OK major=%d unit=%d cn_dev=%#lx\n",
	    (int)maj, device_unit(self), (unsigned long)cn_tab->cn_dev);
	heart_set_post_test(ioc3_com_post_test, msc);
}

static void
ioc3_com_post_test(void *arg)
{
	struct ioc3_com_softc *msc = arg;
	struct com_softc *sc = &msc->sc_com;

	if (xbridge_intr_route(IOC3_SUPERIO_PIN, IOC3_HEART_VECTOR) != 0) {
		ioc3_com_failed("route");
		return;
	}
	msc->sc_ih = platform.intr_establish(IOC3_HEART_VECTOR, IPL_VM,
	    comintr, sc);
	if (msc->sc_ih == NULL) {
		ioc3_com_failed("establish");
		return;
	}
	bus_space_write_4(msc->sc_st, msc->sc_sh, IOC3_INT_ENABLE_SET,
	    IOC3_SERIAL_A);
	if (xbridge_intr_enable(IOC3_SUPERIO_PIN, 1) != 0) {
		ioc3_com_failed("enable");
		return;
	}
	if ((xbridge_intr_status() & BRIDGE_INTR_ERROR_MASK) != 0) {
		ioc3_com_failed("bridge");
		return;
	}
	printf("XIRI_IP30_M3C1_IRQ_OK pin=%d vector=%d\n",
	    IOC3_SUPERIO_PIN, IOC3_HEART_VECTOR);
	printf("XIRI_IP30_M3C1_READY tty=com major=%d unit=%d\n",
	    (int)cdevsw_lookup_major(&com_cdevsw), device_unit(sc->sc_dev));
}
