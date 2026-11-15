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

#include <sys/param.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <sys/bus.h>
#include <machine/autoconf.h>
#include <machine/machtype.h>

#include <dev/pci/pcireg.h>
#include <dev/pci/pcidevs.h>

#include <sgimips/dev/bridgereg.h>

struct bridge_softc {
	device_t sc_dev;
	bus_space_tag_t sc_st;
	bus_space_handle_t sc_sh;
};

static int	bridge_match(device_t, cfdata_t, void *);
static void	bridge_attach(device_t, device_t, void *);

CFATTACH_DECL_NEW(xbridge, sizeof(struct bridge_softc),
    bridge_match, bridge_attach, NULL, NULL);

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
		printf("XIRI_IP30_M3_BRIDGE_OK\n");
	} else {
		printf("XIRI_IP30_M3_BRIDGE_FAILED pci ids or errors\n");
	}
}
