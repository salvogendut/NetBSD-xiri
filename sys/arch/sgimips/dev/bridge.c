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
 * SGI Octane/IP30 XIO Bridge inventory checkpoint.
 *
 * This driver proves access to the Crossbow link and the Bridge's
 * memory-mapped PCI configuration space.  It does not attach pci(4), claim
 * BARs, program DMA or take over IOC3 yet.
 */

#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_xiri_ip30.h"

#include <sys/param.h>
#include <sys/device.h>
#include <sys/disk.h>
#include <sys/errno.h>
#include <sys/kthread.h>
#include <sys/malloc.h>
#include <sys/proc.h>
#include <sys/systm.h>

#include <sys/bus.h>
#include <machine/autoconf.h>
#include <machine/intr.h>
#include <machine/machtype.h>
#include <machine/sysconf.h>

#include "pci.h"

#if NPCI > 0
#include <dev/pci/pcivar.h>
#endif
#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>

#include <sgimips/dev/bridgereg.h>
#include <sgimips/dev/bridgevar.h>
#include <sgimips/dev/heartvar.h>
#ifdef XIRI_IP30_M3D2
#include <dev/ic/isp_netbsd.h>
#endif
#ifdef XIRI_IP30_M3D3
#include <dev/scsipi/scsiconf.h>
#include <dev/scsipi/scsipi_all.h>
#include <dev/scsipi/scsipi_disk.h>
#include <dev/scsipi/sdvar.h>
#endif

struct xbridge_intrhand {
	int (*ih_fun)(void *);
	void *ih_arg;
	void *ih_heart;
	int ih_vector;
	unsigned int ih_count;
	int ih_test;
};

struct bridge_softc {
	device_t sc_dev;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
#if NPCI > 0
	struct sgimips_pci_chipset sc_pc;
	struct xbridge_intrhand sc_intr[8];
#endif
};

static int	bridge_match(device_t, cfdata_t, void *);
static void	bridge_attach(device_t, device_t, void *);
#if NPCI > 0
static int	xbridge_bus_maxdevs(pci_chipset_tag_t, int);
static pcireg_t	xbridge_conf_read(pci_chipset_tag_t, pcitag_t, int);
static void	xbridge_conf_write(pci_chipset_tag_t, pcitag_t, int, pcireg_t);
static int	xbridge_intr_map(const struct pci_attach_args *,
		    pci_intr_handle_t *);
static const char *
		xbridge_intr_string(pci_chipset_tag_t, pci_intr_handle_t,
		    char *, size_t);
static void *	xbridge_intr_establish(int, int, int (*)(void *), void *);
static void	xbridge_intr_disestablish(void *);
static int	xbridge_intr_wrap(void *);
#ifdef XIRI_IP30_M3D1
static int	xbridge_setup_qlogic(struct bridge_softc *);
static void	xbridge_post_test(void *);
static int	xbridge_scsibus_children(device_t);
#ifdef XIRI_IP30_M3D3
static void	xbridge_disk_test(void *);
static int	xbridge_read_blocks(struct scsipi_periph *, uint32_t, uint8_t *, int);
#endif
#endif
static int	bridge_pin_vector[8] = { 18, 19, 17, 20, 16, 21, 22, 23 };
#endif

CFATTACH_DECL_NEW(xbridge, sizeof(struct bridge_softc),
    bridge_match, bridge_attach, NULL, NULL);

static struct bridge_softc *bridge_sc;

#ifdef XIRI_IP30_M3D1
static void bridge_pci_bus_mem_init(bus_space_tag_t, void *);
static struct mips_bus_space bridge_pci_mbst;
#endif
#ifdef XIRI_IP30_M3D3
static volatile int xbridge_post_done;
#endif

int
xbridge_intr_route(int pin, int vector)
{
	struct bridge_softc *sc = bridge_sc;
	uint32_t mode;

	if (sc == NULL)
		return ENXIO;
	if (pin < 0 || pin >= 8 || vector < 0 || vector >= 64)
		return EINVAL;

	bus_space_write_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_DEST_UPPER,
	    BRIDGE_HEART_WIDGET << 16);
	bus_space_write_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_DEST_LOWER,
	    BRIDGE_HEART_INTR_ADDR);
	bus_space_write_4(sc->sc_st, sc->sc_sh,
	    BRIDGE_INTR_HOST0 + pin * BRIDGE_INTR_HOST_STRIDE, vector);
	mode = bus_space_read_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_MODE);
	bus_space_write_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_MODE,
	    mode | (1U << pin));
	return 0;
}

int
xbridge_intr_enable(int pin, int enable)
{
	struct bridge_softc *sc = bridge_sc;
	uint32_t mask;

	if (sc == NULL)
		return ENXIO;
	if (pin < 0 || pin >= 8)
		return EINVAL;

	mask = bus_space_read_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_ENABLE);
	if (enable)
		mask |= 1U << pin;
	else
		mask &= ~(1U << pin);
	bus_space_write_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_ENABLE, mask);
	return 0;
}

uint32_t
xbridge_intr_status(void)
{
	struct bridge_softc *sc = bridge_sc;

	if (sc == NULL)
		return 0xffffffffU;
	return bus_space_read_4(sc->sc_st, sc->sc_sh, BRIDGE_INTR_STATUS);
}

static int
bridge_match(device_t parent, cfdata_t match, void *aux)
{

	return mach_type == MACH_SGI_IP30;
}

static uint32_t
bridge_conf_read(struct bridge_softc *sc, int slot, int function, int reg)
{
	bus_size_t off;
	uint32_t data;

	off = BRIDGE_CONFIG0_BASE + slot * BRIDGE_SLOT_STRIDE +
	    function * BRIDGE_FUNC_STRIDE + reg;
	data = bus_space_read_4(sc->sc_st, sc->sc_sh, off);
	return data;
}

#if NPCI > 0
static int
xbridge_bus_maxdevs(pci_chipset_tag_t pc, int busno)
{

	return busno == 0 ? 4 : 0;
}

static pcireg_t
xbridge_conf_read(pci_chipset_tag_t pc, pcitag_t tag, int reg)
{
	struct bridge_softc *sc = pc->cookie;
	int bus, device, function;

	if ((unsigned int)reg >= PCI_CONF_SIZE)
		return (pcireg_t)-1;
	pci_decompose_tag(pc, tag, &bus, &device, &function);
	if (bus != 0 || device < 0 || device >= 4 || function != 0)
		return (pcireg_t)-1;
	return bridge_conf_read(sc, device, function, reg);
}

static void
xbridge_conf_write(pci_chipset_tag_t pc, pcitag_t tag, int reg,
    pcireg_t data)
{
	struct bridge_softc *sc = pc->cookie;
	bus_size_t off;
	int bus, device, function;

	if ((unsigned int)reg >= PCI_CONF_SIZE)
		return;
	pci_decompose_tag(pc, tag, &bus, &device, &function);
	if (bus != 0 || device < 0 || device >= 4 || function != 0)
		return;
	off = BRIDGE_CONFIG0_BASE + device * BRIDGE_SLOT_STRIDE +
	    function * BRIDGE_FUNC_STRIDE + reg;
	bus_space_write_4(sc->sc_st, sc->sc_sh, off, data);
}

static int
xbridge_intr_map(const struct pci_attach_args *pa, pci_intr_handle_t *ihp)
{
	int pin;

	if (pa->pa_bus != 0 || pa->pa_device >= 4 || pa->pa_function != 0)
		return -1;
	if (pa->pa_intrpin < 1 || pa->pa_intrpin > 4)
		return -1;
	pin = pa->pa_device;
	if ((pa->pa_intrpin & 1) == 0)
		pin ^= 4;
	*ihp = pin;
	return 0;
}

static const char *
xbridge_intr_string(pci_chipset_tag_t pc, pci_intr_handle_t ih, char *buf,
    size_t len)
{

	snprintf(buf, len, "xbridge pin %d", ih);
	return buf;
}

static int
xbridge_intr_wrap(void *arg)
{
	struct xbridge_intrhand *ih = arg;

	ih->ih_count++;
	if (ih->ih_test) {
		heart_intr_clear(ih->ih_vector);
		return 1;
	}
	return ih->ih_fun(ih->ih_arg);
}

static void *
xbridge_intr_establish(int irq, int level, int (*func)(void *), void *arg)
{
	struct bridge_softc *sc = bridge_sc;
	struct xbridge_intrhand *ih;

	if (sc == NULL || irq < 0 || irq >= 8 || platform.intr_establish == NULL)
		return NULL;
	ih = &sc->sc_intr[irq];
	if (ih->ih_fun != NULL)
		return NULL;
	ih->ih_fun = func;
	ih->ih_arg = arg;
	ih->ih_vector = bridge_pin_vector[irq];
	ih->ih_test = 0;
	if (xbridge_intr_route(irq, ih->ih_vector) != 0) {
		ih->ih_fun = NULL;
		return NULL;
	}
	ih->ih_heart = platform.intr_establish(ih->ih_vector, IPL_VM,
	    xbridge_intr_wrap, ih);
	if (ih->ih_heart == NULL) {
		ih->ih_fun = NULL;
		return NULL;
	}
	if (xbridge_intr_enable(irq, 1) != 0) {
		ih->ih_fun = NULL;
		return NULL;
	}
	return ih;
}

static void
xbridge_intr_disestablish(void *cookie)
{
	struct bridge_softc *sc = bridge_sc;
	struct xbridge_intrhand *ih = cookie;

	if (sc == NULL || ih == NULL)
		return;
	(void)xbridge_intr_enable(ih - sc->sc_intr, 0);
	heart_intr_disestablish(ih->ih_heart);
	ih->ih_fun = NULL;
	ih->ih_arg = NULL;
	ih->ih_heart = NULL;
	ih->ih_test = 0;
	ih->ih_count = 0;
}

#ifdef XIRI_IP30_M3D3
static int
xbridge_read_blocks(struct scsipi_periph *periph, uint32_t lba, uint8_t *data,
    int blocks)
{
	struct scsipi_rw_10 cmd;
	int error;

	memset(&cmd, 0, sizeof(cmd));
	cmd.opcode = READ_10;
	_lto4b(lba, cmd.addr);
	_lto2b(blocks, cmd.length);
	error = scsipi_command(periph, (struct scsipi_generic *)&cmd,
	    sizeof(cmd), data, blocks * 512, SCSIPIRETRIES, 30000, NULL,
	    XS_CTL_DATA_IN | XS_CTL_SILENT);
	return error;
}

static int
xbridge_pattern_mismatch(uint32_t lba, const uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		uint32_t sector = lba + i / 512;
		uint32_t offset = i % 512;
		uint8_t expected;

		if (offset < 4) {
			expected = (sector >> (24 - offset * 8)) & 0xff;
		} else if (offset < 8) {
			expected = (~sector >> (24 - (offset - 4) * 8)) & 0xff;
		} else {
			expected = (uint8_t)((sector * 131 +
			    ((sector >> 8) & 0xff) * 197 +
			    ((sector >> 16) & 0xff) * 61 +
			    ((sector >> 24) & 0xff) * 241 +
			    (offset & 0xff) * 29 +
			    ((offset >> 8) & 0xff) * 233 + 0x5a) & 0xff);
		}
		if (buf[i] != expected)
			return (int)i;
	}
	return -1;
}

static int
xbridge_buffer_untouched(const uint8_t *buf, size_t len, uint8_t value)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (buf[i] != value)
			return (int)i;
	}
	return -1;
}

static void
xbridge_disk_test(void *arg)
{
	struct bridge_softc *sc = arg;
	struct sd_softc *sd;
	struct scsipi_periph *periph;
	struct scsipi_inquiry_data inq;
	device_t sddev;
	uint8_t *buf, *data;
	uint32_t blocks[] = { 2, 17, 113, 4097 };
	unsigned int base_irq, count0, count1;
	int i, error, bad;

	while (xbridge_post_done == 0)
		(void)tsleep(&xbridge_post_done, PWAIT, "ip30post", hz);

	sddev = NULL;
	sd = NULL;
	for (i = 0; i < 200; i++) {
		sddev = device_find_by_xname("sd0");
		if (sddev != NULL && device_is_active(sddev)) {
			sd = device_private(sddev);
			if (sd->sc_periph != NULL &&
			    sd->params.disksize != 0 &&
			    sd->params.blksize != 0)
				break;
		}
		(void)tsleep(sc, PWAIT, "ip30sd", hz / 10);
	}
	if (i == 200 || sddev == NULL || sd == NULL) {
		printf("XIRI_IP30_M3D3_FAILED sd\n");
		kthread_exit(0);
	}
	periph = sd->sc_periph;
	if (sd->params.blksize != 512 || sd->params.disksize != 32768) {
		printf("XIRI_IP30_M3D3_FAILED capacity blocks=%llu blksize=%lu\n",
		    (unsigned long long)sd->params.disksize,
		    sd->params.blksize);
		kthread_exit(0);
	}

	memset(&inq, 0, sizeof(inq));
	error = scsipi_inquire(periph, &inq, XS_CTL_SILENT);
	if (error != 0 ||
	    memcmp(inq.vendor, "SGI     ", 8) != 0 ||
	    memcmp(inq.product, "IRIS EMUL DISK  ", 16) != 0 ||
	    memcmp(inq.revision, "1.0 ", 4) != 0) {
		printf("XIRI_IP30_M3D3_FAILED identity error=%d\n", error);
		kthread_exit(0);
	}

	base_irq = sc->sc_intr[0].ih_count;
	buf = malloc(64 + 32768 + 64, M_DEVBUF, M_WAITOK);
	memset(buf, 0xcc, 64 + 32768 + 64);
	data = buf + 64;
	for (i = 0; i < (int)(sizeof(blocks) / sizeof(blocks[0])); i++) {
		memset(data, 0xdd, 32768);
		error = xbridge_read_blocks(periph, blocks[i], data, 1);
		if (error != 0) {
			printf("XIRI_IP30_M3D3_FAILED read %u error=%d\n",
			    blocks[i], error);
			goto out;
		}
		bad = xbridge_pattern_mismatch(blocks[i], data, 512);
		if (bad >= 0) {
			printf("XIRI_IP30_M3D3_FAILED data %u offset %d\n",
			    blocks[i], bad);
			goto out;
		}
		if (xbridge_pattern_mismatch(blocks[i] + 1, data, 512) < 0 ||
		    xbridge_pattern_mismatch(blocks[i] + 256, data, 512) < 0) {
			printf("XIRI_IP30_M3D3_FAILED neg_lba %u\n", blocks[i]);
			goto out;
		}
		if ((blocks[i] == 113 &&
		    xbridge_pattern_mismatch(4097, data, 512) < 0) ||
		    (blocks[i] == 4097 &&
		    xbridge_pattern_mismatch(113, data, 512) < 0)) {
			printf("XIRI_IP30_M3D3_FAILED neg_sector %u\n",
			    blocks[i]);
			goto out;
		}
		{
			uint8_t saved = data[123];

			data[123] ^= 0xff;
			bad = xbridge_pattern_mismatch(blocks[i], data, 512);
			data[123] = saved;
			if (bad < 0) {
				printf("XIRI_IP30_M3D3_FAILED neg_data %u\n",
				    blocks[i]);
				goto out;
			}
		}
		bad = xbridge_buffer_untouched(data + 512, 32768 - 512, 0xdd);
		if (bad >= 0) {
			printf("XIRI_IP30_M3D3_FAILED overrun %u offset %d\n",
			    blocks[i], bad);
			goto out;
		}
	}

	memset(data, 0xdd, 32768);
	error = xbridge_read_blocks(periph, 128, data, 64);
	if (error != 0) {
		printf("XIRI_IP30_M3D3_FAILED read 128/64 error=%d\n", error);
		goto out;
	}
	bad = xbridge_pattern_mismatch(128, data, 32768);
	if (bad >= 0) {
		printf("XIRI_IP30_M3D3_FAILED data 128/64 offset %d\n", bad);
		goto out;
	}
	bad = xbridge_buffer_untouched(buf, 64, 0xcc);
	if (bad >= 0 || xbridge_buffer_untouched(buf + 64 + 32768, 64,
	    0xcc) >= 0) {
		printf("XIRI_IP30_M3D3_FAILED guard\n");
		goto out;
	}

	if (sc->sc_intr[0].ih_count == base_irq) {
		printf("XIRI_IP30_M3D3_FAILED irq\n");
		goto out;
	}
	count0 = sc->sc_intr[0].ih_count;
	count1 = sc->sc_intr[1].ih_count;
	delay(10000);
	if (sc->sc_intr[0].ih_count != count0 ||
	    sc->sc_intr[1].ih_count != count1 ||
	    heart_intr_pending(18) || heart_intr_pending(19) ||
	    (xbridge_intr_status() & ((1U << 0) | (1U << 1))) != 0) {
		printf("XIRI_IP30_M3D3_FAILED interrupt\n");
		goto out;
	}
	if ((xbridge_intr_status() & BRIDGE_INTR_ERROR_MASK) != 0) {
		printf("XIRI_IP30_M3D3_FAILED bridge\n");
		goto out;
	}

	printf("XIRI_IP30_M3D3_DISK_OK vendor=SGI product=IRIS_EMUL_DISK "
	    "revision=1.0 blocks=32768 blksize=512\n");
	printf("XIRI_IP30_M3D3_READ_OK single=4 multi=64 guards=0 dma=1 irq=1\n");
	printf("XIRI_IP30_M3D3_READY interrupts=clear bridge_errors=0\n");
out:
	free(buf, M_DEVBUF);
	kthread_exit(0);
}
#endif

#ifdef XIRI_IP30_M3D2
static int
xbridge_isp_nop(struct bridge_softc *sc, device_t dev, int pin)
{
	struct ispsoftc *isp = device_private(dev);
	struct xbridge_intrhand *ih = &sc->sc_intr[pin];
	uint64_t intmboxc;
	unsigned int base;
	int i, s;

	if (isp->isp_state != ISP_RUNSTATE) {
		printf("XIRI_IP30_M3D2_FAILED state %d\n", pin);
		return 1;
	}
	if (MBOX_ACQUIRE(isp) != 0) {
		printf("XIRI_IP30_M3D2_FAILED busy %d\n", pin);
		return 1;
	}
	intmboxc = isp->isp_intmboxc;
	base = ih->ih_count;
	ih->ih_test = 0;
	s = splvm();
	ISP_LOCK(isp);
	isp->isp_mboxtmp[0] = 0;
	isp->isp_lastmbxcmd = MBOX_NO_OP;
	isp->isp_obits = 1;
	isp->isp_mboxbsy = 1;
	isp->isp_osinfo.mboxcmd_done = 0;
	ISP_WRITE(isp, MBOX_OFF(0), MBOX_NO_OP);
	ISP_WRITE(isp, HCCR, HCCR_CMD_SET_HOST_INT);
	ISP_UNLOCK(isp);

	for (i = 0; i < 100; i++) {
		if (heart_intr_pending(ih->ih_vector))
			break;
		delay(1000);
	}
	if (!heart_intr_pending(ih->ih_vector) ||
	    ih->ih_count != base) {
		heart_intr_clear(ih->ih_vector);
		splx(s);
		printf("XIRI_IP30_M3D2_FAILED pending %d\n", pin);
		goto fail;
	}
	splx(s);

	for (i = 0; i < 100; i++) {
		if (ih->ih_count == base + 1)
			break;
		delay(1000);
	}
	if (ih->ih_count != base + 1) {
		printf("XIRI_IP30_M3D2_FAILED delivered %d\n", pin);
		goto fail;
	}
	if (isp->isp_intmboxc != intmboxc + 1 ||
	    isp->isp_osinfo.mboxcmd_done == 0 ||
	    isp->isp_mboxtmp[0] != MBOX_COMMAND_COMPLETE) {
		printf("XIRI_IP30_M3D2_FAILED status %d\n", pin);
		goto fail;
	}
	if (heart_intr_pending(ih->ih_vector) ||
	    (xbridge_intr_status() & (1U << pin)) != 0) {
		printf("XIRI_IP30_M3D2_FAILED ack %d\n", pin);
		goto fail;
	}
	delay(10000);
	if (ih->ih_count != base + 1) {
		printf("XIRI_IP30_M3D2_FAILED storm %d\n", pin);
		goto fail;
	}
	ISP_LOCK(isp);
	isp->isp_mboxbsy = 0;
	MBOX_RELEASE(isp);
	ISP_UNLOCK(isp);
	return 0;

fail:
	heart_intr_clear(ih->ih_vector);
	ISP_LOCK(isp);
	isp->isp_mboxbsy = 0;
	MBOX_RELEASE(isp);
	ISP_UNLOCK(isp);
	return 1;
}
#endif

#ifdef XIRI_IP30_M3D1
static int
xbridge_scsibus_children(device_t parent)
{
	deviter_t di;
	device_t dv;
	int n = 0;

	for (dv = deviter_first(&di, 0); dv != NULL; dv = deviter_next(&di)) {
		if (device_parent(dv) != parent)
			continue;
		if (strncmp(device_xname(dv), "scsibus", 7) == 0 &&
		    device_is_active(dv))
			n++;
	}
	deviter_release(&di);
	return n;
}

static void
xbridge_post_test(void *arg)
{
	struct bridge_softc *sc = arg;
	struct xbridge_intrhand *ih;
	device_t isp0, isp1;
	struct pci_attach_args pa;
	pci_intr_handle_t ihp;
	int (*saved_fun)(void *);
	void *saved_arg;
	int dev, pin;
#ifndef XIRI_IP30_M3D2
	unsigned int base;
	int s;
#endif

	for (dev = 0; dev < 2; dev++) {
		for (pin = 1; pin <= 4; pin++) {
			memset(&pa, 0, sizeof(pa));
			pa.pa_device = dev;
			pa.pa_intrpin = pin;
			if (xbridge_intr_map(&pa, &ihp) != 0 ||
			    ihp != (dev | ((pin & 1) == 0 ? 4 : 0))) {
				printf("XIRI_IP30_M3D1_FAILED mapping %d/%d\n",
				    dev, pin);
				return;
			}
		}
	}

	isp0 = device_find_by_xname("isp0");
	isp1 = device_find_by_xname("isp1");
	if (isp0 == NULL || isp1 == NULL ||
	    !device_is_active(isp0) || !device_is_active(isp1)) {
		printf("XIRI_IP30_M3D1_FAILED controller\n");
		return;
	}
	if (xbridge_scsibus_children(isp0) != 1 ||
	    xbridge_scsibus_children(isp1) != 1) {
		printf("XIRI_IP30_M3D1_FAILED children\n");
		return;
	}

	for (pin = 0; pin < 2; pin++) {
		ih = &sc->sc_intr[pin];
		if (ih->ih_fun == NULL) {
			printf("XIRI_IP30_M3D1_FAILED attach %d\n", pin);
			return;
		}
		if (bus_space_read_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_INTR_HOST0 + pin * BRIDGE_INTR_HOST_STRIDE) !=
		    (uint32_t)ih->ih_vector ||
		    (bus_space_read_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_INTR_ENABLE) & (1U << pin)) == 0) {
			printf("XIRI_IP30_M3D1_FAILED route %d\n", pin);
			return;
		}
	}
#ifdef XIRI_IP30_M3D2
	if (xbridge_isp_nop(sc, isp0, 0) != 0 ||
	    xbridge_isp_nop(sc, isp1, 1) != 0)
		return;
#else
	for (pin = 0; pin < 2; pin++) {
		ih = &sc->sc_intr[pin];
		base = ih->ih_count;
		ih->ih_test = 1;
		s = splvm();
		heart_intr_set(ih->ih_vector);
		delay(1000);
		if (!heart_intr_pending(ih->ih_vector) ||
		    ih->ih_count != base) {
			heart_intr_clear(ih->ih_vector);
			splx(s);
			ih->ih_test = 0;
			printf("XIRI_IP30_M3D1_FAILED pending %d\n", pin);
			return;
		}
		splx(s);
		delay(10000);
		if (ih->ih_count != base + 1 ||
		    heart_intr_pending(ih->ih_vector)) {
			heart_intr_clear(ih->ih_vector);
			ih->ih_test = 0;
			printf("XIRI_IP30_M3D1_FAILED delivered %d\n", pin);
			return;
		}
		delay(10000);
		if (ih->ih_count != base + 1) {
			ih->ih_test = 0;
			printf("XIRI_IP30_M3D1_FAILED storm %d\n", pin);
			return;
		}
		ih->ih_test = 0;
	}
#endif

	ih = &sc->sc_intr[0];
	saved_fun = ih->ih_fun;
	saved_arg = ih->ih_arg;
	xbridge_intr_disestablish(ih);
	if (ih->ih_fun != NULL || ih->ih_heart != NULL ||
	    (bus_space_read_4(sc->sc_st, sc->sc_sh,
	    BRIDGE_INTR_ENABLE) & 1U) != 0 ||
	    heart_intr_pending(ih->ih_vector)) {
		printf("XIRI_IP30_M3D1_FAILED teardown\n");
		return;
	}
	if (xbridge_intr_establish(0, 0, saved_fun, saved_arg) == NULL ||
	    ih->ih_fun != saved_fun || ih->ih_heart == NULL ||
	    (bus_space_read_4(sc->sc_st, sc->sc_sh,
	    BRIDGE_INTR_ENABLE) & 1U) == 0) {
		printf("XIRI_IP30_M3D1_FAILED reestablish\n");
		return;
	}
	if ((xbridge_intr_status() & BRIDGE_INTR_ERROR_MASK) != 0) {
		printf("XIRI_IP30_M3D1_FAILED bridge\n");
		return;
	}
#ifdef XIRI_IP30_M3D2
	printf("XIRI_IP30_M3D2_IRQ_OK isp0=18 isp1=19 mailbox=2 ack=1 "
	    "storm=0 teardown=1 mapping=1\n");
	printf("XIRI_IP30_M3D2_READY isp=2 scsibus=2\n");
#else
	printf("XIRI_IP30_M3D1_IRQ_OK pin0=18 pin1=19 masked=deferred "
	    "delivered=2 storm=0 teardown=1 mapping=1\n");
	printf("XIRI_IP30_M3D1_READY isp=2 scsibus=2\n");
#endif
#ifdef XIRI_IP30_M3D3
	xbridge_post_done = 1;
	wakeup(&xbridge_post_done);
#endif
}

static int
xbridge_setup_qlogic(struct bridge_softc *sc)
{
	uint32_t base, window;
	int slot;

#ifdef XIRI_IP30_M3D3
	uint32_t direct = BRIDGE_HEART_WIDGET << BRIDGE_DIRECT_WIDGET_SHIFT;

	bus_space_write_4(sc->sc_st, sc->sc_sh, BRIDGE_DIRECT_MAP, direct);
	if (bus_space_read_4(sc->sc_st, sc->sc_sh,
	    BRIDGE_DIRECT_MAP) != direct) {
		printf("XIRI_IP30_M3D3_FAILED direct_map\n");
		return 1;
	}
#endif

	for (slot = 0; slot < 2; slot++) {
		base = 0x1f200000U + slot * 0x00200000U;
		window = (1U << 12) | ((base >> 20) & 0xfff);
#ifdef XIRI_IP30_M3D3
		window |= 1U << 19;
#endif
		bus_space_write_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_CONFIG0_BASE + slot * BRIDGE_SLOT_STRIDE +
		    PCI_BAR1, base);
		bus_space_write_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_CONFIG0_BASE + slot * BRIDGE_SLOT_STRIDE +
		    PCI_COMMAND_STATUS_REG, PCI_COMMAND_MEM_ENABLE |
		    PCI_COMMAND_MASTER_ENABLE);
		bus_space_write_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_DEVICE0 + slot * BRIDGE_DEVICE_STRIDE, window);
		if (bridge_conf_read(sc, slot, 0, PCI_BAR1) != base ||
		    bus_space_read_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_DEVICE0 + slot * BRIDGE_DEVICE_STRIDE) != window) {
			printf("XIRI_IP30_M3D1_FAILED bar/window %d\n", slot);
			return 1;
		}
	}
	printf("XIRI_IP30_M3D1_BAR_OK bar0=0x1f200000 bar1=0x1f400000 "
	    "window0=0x1f2 window1=0x1f4\n");
	return 0;
}

#endif

#endif /* NPCI > 0 */

static void
bridge_attach(device_t parent, device_t self, void *aux)
{
	struct mainbus_attach_args *ma = aux;
	struct bridge_softc *sc = device_private(self);
	uint32_t id, err_before, err_after, new_errors, expected[4];
	uint32_t found[4];
	int slot;

	sc->sc_dev = self;
	sc->sc_st = normal_memt;
	if (bus_space_map(sc->sc_st, ma->ma_addr, 0x100000,
	    BUS_SPACE_MAP_LINEAR, &sc->sc_sh) != 0)
		panic("%s: unable to map Bridge window", __func__);
	bridge_sc = sc;

	err_before = bus_space_read_4(sc->sc_st, sc->sc_sh,
	    BRIDGE_INTR_STATUS);
	id = bus_space_read_4(sc->sc_st, sc->sc_sh, BRIDGE_WIDGET_ID);
	aprint_normal(": id %#x", id);
	if (((id >> 12) & 0xffff) != BRIDGE_PART ||
	    ((id >> 1) & 0x7ff) != BRIDGE_MANUFACTURER) {
		aprint_normal(" (unexpected part)\n");
		printf("XIRI_IP30_M3_BRIDGE_FAILED id=%#x\n", id);
		return;
	}
	aprint_normal(" (rev %u)\n", id >> 28);

	/* The Bridge must be alive on Crossbow port 15. */
	{
		bus_space_tag_t xst = normal_memt;
		bus_space_handle_t xsh;
		uint32_t xid, stat;

		if (bus_space_map(xst, XIO_WIDGET0_BASE, 0x400,
		    BUS_SPACE_MAP_LINEAR, &xsh) != 0) {
			printf("XIRI_IP30_M3_BRIDGE_FAILED xbow map\n");
			return;
		}
		xid = bus_space_read_4(xst, xsh, XBOW_ID);
		stat = bus_space_read_4(xst, xsh, XBOW_LINK_BASE +
		    (15 - 8) * XBOW_LINK_STRIDE + XBOW_LINK_STATUS);
		bus_space_unmap(xst, xsh, 0x400);
		aprint_normal_dev(self, "Xbow id %#x link15 %#x\n", xid, stat);
		if (xid == 0xffffffffU || (xid & 1) == 0 ||
		    ((xid >> 12) & 0xffff) != 0 ||
		    ((xid >> 1) & 0x7ff) != 0) {
			printf("XIRI_IP30_M3_BRIDGE_FAILED xbow id=%#x\n",
			    xid);
			return;
		}
		if (stat == 0xffffffffU || (stat & XBOW_LINK_ALIVE) == 0) {
			printf("XIRI_IP30_M3_BRIDGE_FAILED link15=%#x\n",
			    stat);
			return;
		}
	}

	expected[0] = BRIDGE_EXPECT_SCSI0;
	expected[1] = BRIDGE_EXPECT_SCSI1;
	expected[2] = BRIDGE_EXPECT_IOC3;
	expected[3] = BRIDGE_EXPECT_RAD;
	for (slot = 0; slot < 4; slot++) {
		found[slot] = bridge_conf_read(sc, slot, 0, PCI_ID_REG);
		aprint_normal_dev(self, "pci %d: %04x:%04x", slot,
		    PCI_VENDOR(found[slot]), PCI_PRODUCT(found[slot]));
		if (found[slot] == expected[slot])
			aprint_normal(" (expected)\n");
		else
			aprint_normal(" (unexpected)\n");
	}

	err_after = bus_space_read_4(sc->sc_st, sc->sc_sh,
	    BRIDGE_INTR_STATUS);
	new_errors = (err_after & ~err_before) & BRIDGE_INTR_ERROR_MASK;
	aprint_normal_dev(self, "intr status before %#x after %#x new %#x\n",
	    err_before, err_after, new_errors);

	if (found[0] == expected[0] && found[1] == expected[1] &&
	    found[2] == expected[2] && found[3] == expected[3] &&
	    new_errors == 0) {
#if NPCI > 0
#ifdef XIRI_IP30_M3D1
		if (xbridge_setup_qlogic(sc) != 0)
			return;
#endif
		struct pcibus_attach_args pba;
		pci_chipset_tag_t pc = &sc->sc_pc;
		device_t child;
		uint32_t err_post, post_errors;

		printf("XIRI_IP30_M3_BRIDGE_OK\n");

		pc->cookie = sc;
		pc->iot = sc->sc_st;
		pc->ioh = sc->sc_sh;
		pc->pc_bus_maxdevs = xbridge_bus_maxdevs;
		pc->pc_conf_read = xbridge_conf_read;
		pc->pc_conf_write = xbridge_conf_write;
		pc->pc_intr_map = xbridge_intr_map;
		pc->pc_intr_string = xbridge_intr_string;
		pc->intr_establish = xbridge_intr_establish;
		pc->intr_disestablish = xbridge_intr_disestablish;

		memset(&pba, 0, sizeof(pba));
		pba.pba_iot = normal_memt;
#ifdef XIRI_IP30_M3D1
		bridge_pci_bus_mem_init(&bridge_pci_mbst, NULL);
		pba.pba_memt = &bridge_pci_mbst;
#else
		pba.pba_memt = normal_memt;
#endif
		pba.pba_dmat = &pci_bus_dma_tag;
		pba.pba_dmat64 = NULL;
		pba.pba_bus = 0;
		pba.pba_pc = pc;
		pba.pba_bridgetag = NULL;
		pba.pba_flags = PCI_FLAGS_IO_OKAY | PCI_FLAGS_MEM_OKAY;
		child = config_found(self, &pba, pcibusprint, CFARGS_NONE);

#ifdef XIRI_IP30_M3D1
		heart_set_post_test(xbridge_post_test, sc);
#endif
#ifdef XIRI_IP30_M3D3
		if (kthread_create(PRI_BIO, 0, NULL, xbridge_disk_test, sc,
		    NULL, "ip30disk") != 0)
			printf("XIRI_IP30_M3D3_FAILED thread\n");
#endif
		err_post = bus_space_read_4(sc->sc_st, sc->sc_sh,
		    BRIDGE_INTR_STATUS);
		post_errors = (err_post & ~err_after) &
		    BRIDGE_INTR_ERROR_MASK;
		aprint_normal_dev(self,
		    "post-pci intr status %#x new %#x child %s\n",
		    err_post, post_errors, child != NULL ? "attached" :
		    "missing");
		if (child != NULL && post_errors == 0)
			printf("XIRI_IP30_M3B_PCI_OK\n");
		else
			printf("XIRI_IP30_M3B_PCI_FAILED child=%p errors=%#x\n",
			    child, post_errors);
#else
		printf("XIRI_IP30_M3_BRIDGE_OK\n");
#endif
	} else {
		printf("XIRI_IP30_M3_BRIDGE_FAILED pci ids or errors\n");
	}
}


#ifdef XIRI_IP30_M3D1
#define	CHIP			bridge_pci
#define	CHIP_MEM
#define	CHIP_WRONG_ENDIAN
#define	CHIP_W1_BUS_START(v)	0x1f200000UL
#define	CHIP_W1_BUS_END(v)	0x1f5fffffUL
#define	CHIP_W1_SYS_START(v)	0x1f200000UL
#define	CHIP_W1_SYS_END(v)	0x1f5fffffUL

#include <mips/mips/bus_space_alignstride_chipdep.c>
#endif
