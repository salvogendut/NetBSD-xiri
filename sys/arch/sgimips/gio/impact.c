/* $NetBSD$ */
/*
 * Copyright (c) 2010, 2012 Miodrag Vallat.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * GIO IMPACT (HQ3), using firmware display timing. Register setup derives
 * from OpenBSD impact.c/impact_gio.c; the genfb attachment and RAM shadow
 * transport are native NetBSD code. There is no linear video aperture.
 *
 * genfb and wsfb draw into ordinary RAM. One kernel thread owns the command
 * FIFO and uploads changed spans. Serial remains the early/debug console:
 * a scheduler-driven shadow cannot provide a reliable DDB console.
 * Initial qualification is High IMPACT at 1280x1024, without 3D or DMA.
 */
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/kthread.h>
#include <sys/kmem.h>
#include <sys/bus.h>
#include <uvm/uvm_extern.h>
#include <dev/arcbios/arcbios.h>
#include <dev/arcbios/arcbiosvar.h>
#include <dev/wsfb/genfbvar.h>
#include <sgimips/gio/giovar.h>
#include <sgimips/gio/gioreg.h>
#include <sgimips/gio/impactreg.h>

#define IMPACT_PRODUCT 0x10
#define IMPACT_REG_SIZE 0x80000
#define IMPACT_WIDTH 1280
#define IMPACT_HEIGHT 1024
#define IMPACT_SPAN 64
#define IMPACT_FBSIZE (IMPACT_WIDTH * IMPACT_HEIGHT * sizeof(uint32_t))

struct impact_softc {
	struct genfb_softc sc_gen;
	bus_space_tag_t sc_iot;
	bus_space_handle_t sc_ioh;
	uint32_t *sc_pixels;
	uint32_t *sc_previous;
};

static int impact_match(device_t, cfdata_t, void *);
static void impact_attach(device_t, device_t, void *);
static int impact_ioctl(void *, void *, u_long, void *, int, struct lwp *);
static paddr_t impact_mmap(void *, void *, off_t, int);
static void impact_update(void *);

CFATTACH_DECL_NEW(impact, sizeof(struct impact_softc),
    impact_match, impact_attach, NULL, NULL);

static int
impact_match(device_t parent, cfdata_t cf, void *aux)
{
	struct gio_attach_args *ga = aux;
	return ga->ga_product != -1 && GIO_PRODUCT_32BIT_ID(ga->ga_product) &&
	    GIO_PRODUCT_PRODUCTID(ga->ga_product) == IMPACT_PRODUCT;
}

static void
impact_cmd(struct impact_softc *sc, unsigned reg, uint32_t val, bool exec)
{
	uint64_t cmd = IMPACTSR_CFIFO_WRITE |
	    ((uint64_t)reg << IMPACTSR_CFIFO_REG_SHIFT) | val;
	if (exec)
		cmd |= IMPACTSR_CFIFO_EXEC;
	bus_space_write_8(sc->sc_iot, sc->sc_ioh, IMPACT_CFIFO, cmd);
}

static bool
impact_wait(struct impact_softc *sc)
{
	/* Bound failures: an unresponsive board must not hang the kernel. */
	for (unsigned n = 0; n < 1000000; n++) {
		if ((bus_space_read_4(sc->sc_iot, sc->sc_ioh,
		    IMPACT_FIFOSTATUS) & IMPACTSR_FIFO_MASK) == 0)
			return true;
		delay(1);
	}
	return false;
}

static void
impact_setup(struct impact_softc *sc)
{
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IMPACT_CFIFO_HW, 0x20);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IMPACT_CFIFO_LW, 0x14);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IMPACT_CFIFO_DELAY, 0x64);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IMPACT_DFIFO_HW, 0x28);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IMPACT_DFIFO_LW, 0x14);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh, IMPACT_DFIFO_DELAY, 0xfff);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh,
	    IMPACT_VC3_BASE + IMPACTSR_VC3_INDEXDATA, 0x1d000100);
	impact_cmd(sc, IMPACTSR_CMD_COLORMASKLSBSA, 0xffffff, false);
	impact_cmd(sc, IMPACTSR_CMD_COLORMASKLSBSB, 0xffffff, false);
	impact_cmd(sc, IMPACTSR_CMD_COLORMASKMSBS, 0, false);
	impact_cmd(sc, IMPACTSR_CMD_XFRMASKLO, 0xffffff, false);
	impact_cmd(sc, IMPACTSR_CMD_XFRMASKHI, 0xffffff, false);
	impact_cmd(sc, IMPACTSR_CMD_DRBPOINTERS, 0xc8240, false);
	impact_cmd(sc, IMPACTSR_CMD_CONFIG, 0xcac, false);
	impact_cmd(sc, IMPACTSR_CMD_XYWIN, IMPACTSR_YXCOORDS(0, 0x3ff), false);
	bus_space_write_1(sc->sc_iot, sc->sc_ioh,
	    IMPACT_XMAP_BASE + IMPACTSR_XMAP_PP1SELECT, 0x01);
	bus_space_write_1(sc->sc_iot, sc->sc_ioh,
	    IMPACT_XMAP_BASE + IMPACTSR_XMAP_INDEX, 0x00);
	bus_space_write_4(sc->sc_iot, sc->sc_ioh,
	    IMPACT_XMAP_BASE + IMPACTSR_XMAP_MAIN_MODE, 0x7a4);
}

static bool
impact_span(struct impact_softc *sc, unsigned x, unsigned y, const uint32_t *p)
{
	bool solid = true;
	for (unsigned i = 1; i < IMPACT_SPAN; i++)
		solid &= p[i] == p[0];
	if (!impact_wait(sc))
		return false;
	impact_cmd(sc, IMPACTSR_CMD_PP1FILLMODE,
	    IMPACTSR_PP1FILLMODE(0x6300, 3), false);
	impact_cmd(sc, IMPACTSR_CMD_FILLMODE, solid ? 0 : (3U << 22), false);
	if (solid) {
		uint32_t rgb = ((p[0] & 0xff) << 16) | (p[0] & 0xff00) |
		    ((p[0] >> 16) & 0xff);
		impact_cmd(sc, IMPACTSR_CMD_PACKEDCOLOR, rgb, false);
	} else {
		/* Four-byte packed RGB, hardware red in bits 7:0. */
		impact_cmd(sc, IMPACTSR_CMD_XFRMODE, 0x80, false);
		impact_cmd(sc, IMPACTSR_CMD_XFRSIZE,
		    IMPACTSR_YXCOORDS(IMPACT_SPAN, 1), false);
	}
	impact_cmd(sc, IMPACTSR_CMD_BLOCKXYSTARTI, IMPACTSR_XYCOORDS(x, y), false);
	impact_cmd(sc, IMPACTSR_CMD_BLOCKXYENDI,
	    IMPACTSR_XYCOORDS(x + IMPACT_SPAN - 1, y), false);
	impact_cmd(sc, IMPACTSR_CMD_IR_ALIAS, 0x18, true);
	if (!solid) {
		for (unsigned i = 0; i < IMPACT_SPAN; i += 2) {
			if (!impact_wait(sc))
				return false;
			uint32_t a = ((p[i] & 0xff) << 16) | (p[i] & 0xff00) |
			    ((p[i] >> 16) & 0xff);
			uint32_t b = ((p[i + 1] & 0xff) << 16) | (p[i + 1] & 0xff00) |
			    ((p[i + 1] >> 16) & 0xff);
			impact_cmd(sc, IMPACTSR_CMD_CHAR_H, a, false);
			impact_cmd(sc, IMPACTSR_CMD_CHAR_L, b, true);
		}
	}
	return true;
}

static void
impact_update(void *cookie)
{
	struct impact_softc *sc = cookie;
	for (;;) {
		for (unsigned y = 0; y < IMPACT_HEIGHT; y++) {
			for (unsigned x = 0; x < IMPACT_WIDTH; x += IMPACT_SPAN) {
				unsigned off = y * IMPACT_WIDTH + x;
				uint32_t *p = sc->sc_previous + off;
				if (memcmp(p, sc->sc_pixels + off,
				    IMPACT_SPAN * sizeof(*p)) == 0)
					continue;
				/* Snapshot before uploading; later writes remain dirty. */
				memcpy(p, sc->sc_pixels + off, IMPACT_SPAN * sizeof(*p));
				if (!impact_span(sc, x, y, p)) {
					aprint_error_dev(sc->sc_gen.sc_dev,
					    "command FIFO timeout; display updates stopped\n");
					kthread_exit(ETIMEDOUT);
				}
			}
		}
		kpause("impactfb", false, MAX(1, hz / 20), NULL);
	}
}

static paddr_t
impact_mmap(void *v, void *vs, off_t off, int prot)
{
	struct impact_softc *sc = v;
	paddr_t pa;
	if (off < 0 || off >= IMPACT_FBSIZE || (off & PAGE_MASK) != 0)
		return -1;
	if (!pmap_extract(pmap_kernel(), (vaddr_t)sc->sc_pixels + off, &pa))
		return -1;
	return mips_btop(pa);
}

static int
impact_ioctl(void *v, void *vs, u_long cmd, void *data, int flag, struct lwp *l)
{
	if (cmd == WSDISPLAYIO_GTYPE) {
		*(u_int *)data = WSDISPLAY_TYPE_GENFB;
		return 0;
	}
	return EPASSTHROUGH;
}

static void
impact_attach(device_t parent, device_t self, void *aux)
{
	struct impact_softc *sc = device_private(self);
	struct gio_attach_args *ga = aux;
	struct genfb_ops ops = {
		.genfb_ioctl = impact_ioctl,
		.genfb_mmap = impact_mmap
	};
	prop_dictionary_t props = device_properties(self);
	const char *status = arcbios_GetEnvironmentVariable("gfx");
	int error;

	sc->sc_gen.sc_dev = self;
	sc->sc_iot = ga->ga_iot;
	aprint_normal(": IMPACT HQ3, 1280x1024 RGB shadow framebuffer\n");
	if (status != NULL && strncmp(status, "alive", 5) != 0) {
		aprint_error_dev(self, "graphics not initialized by firmware\n");
		return;
	}
	if (bus_space_map(sc->sc_iot, ga->ga_addr, IMPACT_REG_SIZE, 0,
	    &sc->sc_ioh) != 0)
		return;
	if (!impact_wait(sc)) {
		aprint_error_dev(self, "command FIFO not ready\n");
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, IMPACT_REG_SIZE);
		return;
	}
	sc->sc_pixels = (void *)uvm_km_alloc(kernel_map, IMPACT_FBSIZE, 0,
	    UVM_KMF_WIRED | UVM_KMF_ZERO);
	if (sc->sc_pixels == NULL) {
		bus_space_unmap(sc->sc_iot, sc->sc_ioh, IMPACT_REG_SIZE);
		return;
	}
	sc->sc_previous = kmem_alloc(IMPACT_FBSIZE, KM_SLEEP);
	memset(sc->sc_previous, 0xff, IMPACT_FBSIZE);
	impact_setup(sc);
	prop_dictionary_set_uint32(props, "width", IMPACT_WIDTH);
	prop_dictionary_set_uint32(props, "height", IMPACT_HEIGHT);
	prop_dictionary_set_uint32(props, "depth", 32);
	prop_dictionary_set_uint32(props, "linebytes", IMPACT_WIDTH * 4);
	prop_dictionary_set_uint64(props, "address", 0);
	prop_dictionary_set_uint64(props, "virtual_address", (uintptr_t)sc->sc_pixels);
	prop_dictionary_set_bool(props, "is_console", false);
	prop_dictionary_set_bool(props, "enable_shadowfb", false);
	genfb_init(&sc->sc_gen);
	error = genfb_attach(&sc->sc_gen, &ops);
	if (error == 0)
		error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL,
		    impact_update, sc, NULL, "impactfb");
	if (error != 0)
		aprint_error_dev(self, "framebuffer attachment failed: %d\n", error);
}
