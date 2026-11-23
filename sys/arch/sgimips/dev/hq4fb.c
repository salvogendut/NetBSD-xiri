/*
 * Copyright (c) 2010, 2012 Miodrag Vallat.
 * Copyright (c) 2009, 2010 Joel Sing <jsing@openbsd.org>
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

/* NetBSD genfb console on a PROM-initialized Octane IMPACT widget. */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/kmem.h>
#include <sys/kthread.h>
#include <sys/systm.h>
#include <uvm/uvm_extern.h>
#include <dev/wsfb/genfbvar.h>

#include "hq4_pio.h"
#include <sgimips/gio/impactreg.h>

#define FB_BYTES (HQ4_WIDTH * HQ4_HEIGHT * sizeof(uint32_t))
#define SPAN 32

struct hq4fb_softc {
	struct genfb_softc gen;
	bus_space_tag_t st;
	bus_space_handle_t h;
	uint32_t *pixels;
	uint32_t *previous;
	bool initialized;
};

static int
hq4fb_match(device_t parent, cfdata_t cf, void *aux)
{
	/* The inventory parent offers only identified HQ4 widgets. */
	return 1;
}

static uint32_t
hq4_colour(uint32_t pixel)
{
	return ((pixel & 0xff) << 16) | (pixel & 0xff00) | ((pixel >> 16) & 0xff);
}

/* A block of 32 pixels plus its setup fits below the 0x47 high-water mark. */
static void
hq4fb_span(struct hq4fb_softc *sc, unsigned x, unsigned y, const uint32_t *p)
{
	bus_space_tag_t st = sc->st;
	bus_space_handle_t h = sc->h;

	if (memcmp(p, p + 1, (SPAN - 1) * sizeof(*p)) == 0) {
		hq4_cmd(st, h, IMPACTSR_CMD_FILLMODE, 0, false);
		hq4_rectangle(st, h, x, y, SPAN, 1, p[0] & 0xffffff);
		return;
	}
	hq4_cmd(st, h, IMPACTSR_CMD_FILLMODE, 3U << 22, false);
	hq4_cmd(st, h, IMPACTSR_CMD_XFRMODE, 0x80, false);
	hq4_cmd(st, h, IMPACTSR_CMD_XFRSIZE, IMPACTSR_YXCOORDS(SPAN, 1), false);
	hq4_cmd(st, h, IMPACTSR_CMD_BLOCKXYSTARTI, IMPACTSR_XYCOORDS(x, y), false);
	hq4_cmd(st, h, IMPACTSR_CMD_BLOCKXYENDI, IMPACTSR_XYCOORDS(x + SPAN - 1, y), false);
	hq4_cmd(st, h, IMPACTSR_CMD_IR_ALIAS, 0x18, true);
	for (unsigned i = 0; i < SPAN; i += 2) {
		hq4_cmd(st, h, IMPACTSR_CMD_CHAR_H, hq4_colour(p[i]), false);
		hq4_cmd(st, h, IMPACTSR_CMD_CHAR_L, hq4_colour(p[i + 1]), true);
	}
}

static void
hq4fb_update(void *arg)
{
	struct hq4fb_softc *sc = arg;
	uint32_t pixels[SPAN];

	CTASSERT(HQ4_WIDTH % SPAN == 0);
	for (;;) {
		for (unsigned y = 0; y < HQ4_HEIGHT; y++) {
			unsigned row = y * HQ4_WIDTH;
			if (memcmp(sc->pixels + row, sc->previous + row,
			    HQ4_WIDTH * sizeof(*pixels)) == 0)
				continue;
			for (unsigned x = 0; x < HQ4_WIDTH; x += SPAN) {
				unsigned off = row + x;
				memcpy(pixels, sc->pixels + off, sizeof(pixels));
				if (memcmp(pixels, sc->previous + off, sizeof(pixels)) == 0)
					continue;
				if (!hq4_fifo_empty(sc->st, sc->h)) {
					aprint_error_dev(sc->gen.sc_dev, "HQ4 FIFO timeout\n");
					kthread_exit(ETIMEDOUT);
				}
				hq4fb_span(sc, x, y, pixels);
				/* A concurrent console write remains dirty for next pass. */
				memcpy(sc->previous + off, pixels, sizeof(pixels));
			}
		}
		kpause("hq4fb", false, MAX(1, hz / 10), NULL);
	}
}

static int
hq4fb_ioctl(void *v, void *vs, u_long cmd, void *data, int flag, struct lwp *l)
{
	if (cmd == WSDISPLAYIO_GTYPE) {
		*(u_int *)data = WSDISPLAY_TYPE_GENFB;
		return 0;
	}
	return EPASSTHROUGH;
}

static paddr_t
hq4fb_mmap(void *v, void *vs, off_t off, int prot)
{
	struct hq4fb_softc *sc = v;
	paddr_t pa;

	if (off < 0 || off >= FB_BYTES || (off & PAGE_MASK) != 0 ||
	    (prot & VM_PROT_EXECUTE) != 0)
		return -1;
	if (!pmap_extract(pmap_kernel(), (vaddr_t)sc->pixels + off, &pa))
		return -1;
	return mips_btop(pa);
}

static int
hq4fb_finalize(device_t self)
{
	struct hq4fb_softc *sc = device_private(self);
	prop_dictionary_t props = device_properties(self);
	struct genfb_ops ops = {
		.genfb_ioctl = hq4fb_ioctl,
		.genfb_mmap = hq4fb_mmap,
	};
	int error;

	if (sc->initialized)
		return 0;
	sc->initialized = true;
	hq4_setup(sc->st, sc->h);
	hq4_rectangle(sc->st, sc->h, 0, 0, HQ4_WIDTH, HQ4_HEIGHT, 0);
	sc->pixels = (void *)uvm_km_alloc(kernel_map, FB_BYTES, 0,
	    UVM_KMF_WIRED | UVM_KMF_ZERO);
	if (sc->pixels == NULL) {
		aprint_error_dev(self, "cannot allocate framebuffer\n");
		goto unmap;
	}
	sc->previous = kmem_zalloc(FB_BYTES, KM_SLEEP);
	prop_dictionary_set_uint32(props, "width", HQ4_WIDTH);
	prop_dictionary_set_uint32(props, "height", HQ4_HEIGHT);
	prop_dictionary_set_uint32(props, "depth", 32);
	prop_dictionary_set_uint32(props, "linebytes", HQ4_WIDTH * 4);
	prop_dictionary_set_uint64(props, "address", 0);
	prop_dictionary_set_uint64(props, "virtual_address", (uintptr_t)sc->pixels);
	prop_dictionary_set_bool(props, "enable_shadowfb", false);
	prop_dictionary_set_bool(props, "is_console", device_unit(self) == 0);
	genfb_init(&sc->gen);
	if (sc->gen.sc_private == NULL || genfb_attach(&sc->gen, &ops) != 0) {
		aprint_error_dev(self, "cannot initialize genfb\n");
		kmem_free(sc->previous, FB_BYTES);
		uvm_km_free(kernel_map, (vaddr_t)sc->pixels, FB_BYTES, UVM_KMF_WIRED);
		goto unmap;
	}
	error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL, hq4fb_update,
	    sc, NULL, "hq4fb");
	if (error)
		aprint_error_dev(self, "cannot start display updates: %d\n", error);
	return 0;
unmap:
	bus_space_unmap(sc->st, sc->h, HQ4_REG_SIZE);
	return 0;
}

static void
hq4fb_attach(device_t parent, device_t self, void *aux)
{
	struct hq4fb_softc *sc = device_private(self);
	const struct hq4_attach_args *ha = aux;

	sc->gen.sc_dev = self;
	sc->st = ha->st;
	aprint_normal(": IMPACT HQ4, %ux%u RGB shadow framebuffer\n",
	    HQ4_WIDTH, HQ4_HEIGHT);
	if (bus_space_map(sc->st, ha->base, HQ4_REG_SIZE, 0, &sc->h) != 0) {
		aprint_error_dev(self, "cannot map HQ4\n");
		return;
	}
	if (!hq4_fifo_empty(sc->st, sc->h)) {
		aprint_error_dev(self, "FIFO not ready\n");
		bus_space_unmap(sc->st, sc->h, HQ4_REG_SIZE);
		return;
	}
	/* Hand over after deferred PCI/SCSI attachment. genfb replays msgbuf. */
	if (config_finalize_register(self, hq4fb_finalize) != 0) {
		aprint_error_dev(self, "cannot register framebuffer handoff\n");
		bus_space_unmap(sc->st, sc->h, HQ4_REG_SIZE);
	}
}

CFATTACH_DECL_NEW(hq4fb, sizeof(struct hq4fb_softc),
    hq4fb_match, hq4fb_attach, NULL, NULL);
