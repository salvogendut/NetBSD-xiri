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
 * FIFO and uploads changed spans during normal operation. Early and polled
 * console text is uploaded synchronously, without scheduler dependencies.
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
#include <sgimips/gio/impactvar.h>

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
	bool sc_polling;
	bool sc_failed;
};

static int impact_match(device_t, cfdata_t, void *);
static void impact_attach(device_t, device_t, void *);
static int impact_ioctl(void *, void *, u_long, void *, int, struct lwp *);
static paddr_t impact_mmap(void *, void *, off_t, int);
static void impact_update(void *);
static void impact_init_screen(struct genfb_softc *, struct vcons_screen *);
static int impact_poll_on(void *);
static int impact_poll_off(void *);

/* Early console storage cannot depend on UVM or kernel threads. */
static uint32_t impact_console_pixels[IMPACT_WIDTH * IMPACT_HEIGHT]
    __aligned(PAGE_SIZE);
static struct impact_softc impact_console;
static struct vcons_screen impact_console_screen;
static struct wsscreen_descr impact_console_descr = {
    "default", 0, 0, NULL, 8, 16, WSSCREEN_WSCOLORS, NULL
};
static bus_addr_t impact_console_addr;
static bool impact_is_console;

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
impact_span(struct impact_softc *sc, unsigned x, unsigned y, const uint32_t *p,
    unsigned count)
{
	bool solid = true;
	for (unsigned i = 1; i < count; i++)
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
		    IMPACTSR_YXCOORDS(count, 1), false);
	}
	impact_cmd(sc, IMPACTSR_CMD_BLOCKXYSTARTI, IMPACTSR_XYCOORDS(x, y), false);
	impact_cmd(sc, IMPACTSR_CMD_BLOCKXYENDI,
	    IMPACTSR_XYCOORDS(x + count - 1, y), false);
	impact_cmd(sc, IMPACTSR_CMD_IR_ALIAS, 0x18, true);
	if (!solid) {
		for (unsigned i = 0; i < count; i += 2) {
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

/*
 * IP28 is uniprocessor. Mask interrupts for each complete FIFO transaction so
 * a keyboard debugger entry cannot interrupt and corrupt an upload. Never
 * sleep or acquire an adaptive lock here: this path also runs in DDB.
 */
static void
impact_flush(struct impact_softc *sc, unsigned x, unsigned y,
    unsigned w, unsigned h)
{
	unsigned endx = MIN(roundup(x + w, 2), IMPACT_WIDTH);
	unsigned endy = MIN(y + h, IMPACT_HEIGHT);
	x &= ~1U;
	for (; y < endy; y++) {
		for (unsigned col = x; col < endx; col += IMPACT_SPAN) {
			uint32_t pixels[IMPACT_SPAN];
			unsigned off = y * IMPACT_WIDTH + col;
			unsigned count = MIN(IMPACT_SPAN, endx - col);
			size_t bytes = count * sizeof(pixels[0]);
			int s = splhigh();
			if (sc->sc_failed) {
				splx(s);
				return;
			}
			if (sc->sc_previous != NULL &&
			    memcmp(sc->sc_previous + off, sc->sc_pixels + off,
			    bytes) == 0) {
				splx(s);
				continue;
			}
			/* Snapshot first; a later userspace write remains dirty. */
			memcpy(pixels, sc->sc_pixels + off, bytes);
			if (impact_span(sc, col, y, pixels, count)) {
				if (sc->sc_previous != NULL)
					memcpy(sc->sc_previous + off, pixels, bytes);
			} else {
				/* Latch failure; do not recurse into a broken console. */
				sc->sc_failed = true;
			}
			splx(s);
		}
	}
}

static void
impact_update(void *cookie)
{
	struct impact_softc *sc = cookie;
	for (;;) {
		if (!sc->sc_polling)
			impact_flush(sc, 0, 0, IMPACT_WIDTH, IMPACT_HEIGHT);
		if (sc->sc_failed) {
			aprint_error_dev(sc->sc_gen.sc_dev,
			    "command FIFO timeout; display updates stopped\n");
			kthread_exit(ETIMEDOUT);
		}
		kpause("impactfb", false, MAX(1, hz / 20), NULL);
	}
}

static void
impact_text_flush(struct rasops_info *ri, int x, int y, int w, int h)
{
	struct vcons_screen *scr = ri->ri_hw;
	struct impact_softc *sc = scr->scr_cookie;
	impact_flush(sc, x, y, w, h);
}

static void
impact_putchar(void *v, int row, int col, u_int c, long attr)
{
	struct rasops_info *ri = v;
	ri->ri_real_ops.putchar(v, row, col, c, attr);
	impact_text_flush(ri, ri->ri_xorigin + col * ri->ri_font->fontwidth,
	    ri->ri_yorigin + row * ri->ri_font->fontheight,
	    ri->ri_font->fontwidth, ri->ri_font->fontheight);
}

static void
impact_copyrows(void *v, int src, int dst, int num)
{
	struct rasops_info *ri = v;
	ri->ri_real_ops.copyrows(v, src, dst, num);
	impact_text_flush(ri, ri->ri_xorigin,
	    ri->ri_yorigin + dst * ri->ri_font->fontheight,
	    ri->ri_emuwidth, num * ri->ri_font->fontheight);
}

static void
impact_copycols(void *v, int row, int src, int dst, int num)
{
	struct rasops_info *ri = v;
	ri->ri_real_ops.copycols(v, row, src, dst, num);
	impact_text_flush(ri, ri->ri_xorigin + dst * ri->ri_font->fontwidth,
	    ri->ri_yorigin + row * ri->ri_font->fontheight,
	    num * ri->ri_font->fontwidth, ri->ri_font->fontheight);
}

static void
impact_eraserows(void *v, int row, int num, long attr)
{
	struct rasops_info *ri = v;
	ri->ri_real_ops.eraserows(v, row, num, attr);
	if (row == 0 && num == ri->ri_rows && (ri->ri_flg & RI_FULLCLEAR))
		impact_text_flush(ri, 0, 0, IMPACT_WIDTH, IMPACT_HEIGHT);
	else
		impact_text_flush(ri, ri->ri_xorigin,
		    ri->ri_yorigin + row * ri->ri_font->fontheight,
		    ri->ri_emuwidth, num * ri->ri_font->fontheight);
}

static void
impact_erasecols(void *v, int row, int col, int num, long attr)
{
	struct rasops_info *ri = v;
	ri->ri_real_ops.erasecols(v, row, col, num, attr);
	impact_text_flush(ri, ri->ri_xorigin + col * ri->ri_font->fontwidth,
	    ri->ri_yorigin + row * ri->ri_font->fontheight,
	    num * ri->ri_font->fontwidth, ri->ri_font->fontheight);
}

static void
impact_cursor(void *v, int on, int row, int col)
{
	struct rasops_info *ri = v;
	int oldrow = ri->ri_crow, oldcol = ri->ri_ccol;
	ri->ri_real_ops.cursor(v, on, row, col);
	impact_text_flush(ri, ri->ri_xorigin + oldcol * ri->ri_font->fontwidth,
	    ri->ri_yorigin + oldrow * ri->ri_font->fontheight,
	    ri->ri_font->fontwidth, ri->ri_font->fontheight);
	impact_text_flush(ri, ri->ri_xorigin + col * ri->ri_font->fontwidth,
	    ri->ri_yorigin + row * ri->ri_font->fontheight,
	    ri->ri_font->fontwidth, ri->ri_font->fontheight);
}

static void
impact_init_screen(struct genfb_softc *gen, struct vcons_screen *scr)
{
	struct rasops_info *ri = &scr->scr_ri;
	/* RAM reads are cheap, including scrolling and cursor inversion. */
	scr->scr_flags &= ~VCONS_DONT_READ;
	scr->scr_cookie = gen;
	/* Unrotated display: preserve the software ops behind our transport. */
	ri->ri_real_ops = ri->ri_ops;
	ri->ri_ops.putchar = impact_putchar;
	ri->ri_ops.copyrows = impact_copyrows;
	ri->ri_ops.copycols = impact_copycols;
	ri->ri_ops.eraserows = impact_eraserows;
	ri->ri_ops.erasecols = impact_erasecols;
	ri->ri_ops.cursor = impact_cursor;
}

static int
impact_poll_on(void *v)
{
	struct impact_softc *sc = v;
	sc->sc_polling = true;
	/* genfb switched to its console screen before calling this hook. */
	vcons_redraw_screen(sc->sc_gen.vd.active);
	return 0;
}

static int
impact_poll_off(void *v)
{
	struct impact_softc *sc = v;
	sc->sc_polling = false;
	return 0;
}

int
impact_cnattach(struct gio_attach_args *ga)
{
	struct impact_softc *sc = &impact_console;
	struct rasops_info *ri = &impact_console_screen.scr_ri;
	uint32_t id = bus_space_read_4(ga->ga_iot, ga->ga_ioh, 0);
	long attr;

	if (!GIO_PRODUCT_32BIT_ID(id) ||
	    GIO_PRODUCT_PRODUCTID(id) != IMPACT_PRODUCT)
		return ENXIO;
	sc->sc_iot = ga->ga_iot;
	sc->sc_ioh = ga->ga_ioh;
	if (!impact_wait(sc))
		return ETIMEDOUT;
	sc->sc_pixels = impact_console_pixels;
	impact_setup(sc);
	ri->ri_bits = (uint8_t *)sc->sc_pixels;
	ri->ri_depth = 32;
	ri->ri_width = IMPACT_WIDTH;
	ri->ri_height = IMPACT_HEIGHT;
	ri->ri_stride = IMPACT_WIDTH * 4;
	ri->ri_rnum = ri->ri_gnum = ri->ri_bnum = 8;
	ri->ri_rpos = 16;
	ri->ri_gpos = 8;
	ri->ri_bpos = 0;
	ri->ri_flg = RI_CENTER | RI_FULLCLEAR | RI_NO_AUTO;
	if (rasops_init(ri, 0, 0) != 0)
		return ENXIO;
	rasops_reconfig(ri, IMPACT_HEIGHT / ri->ri_font->fontheight,
	    IMPACT_WIDTH / ri->ri_font->fontwidth);
	ri->ri_hw = &impact_console_screen;
	impact_init_screen(&sc->sc_gen, &impact_console_screen);
	ri->ri_ops.allocattr(ri, WS_DEFAULT_FG, WS_DEFAULT_BG, 0, &attr);
	ri->ri_ops.eraserows(ri, 0, ri->ri_rows, attr);
	impact_console_descr.ncols = ri->ri_cols;
	impact_console_descr.nrows = ri->ri_rows;
	impact_console_descr.textops = &ri->ri_ops;
	wsdisplay_preattach(&impact_console_descr, ri, 0, 0, attr);
	impact_console_addr = ga->ga_addr;
	impact_is_console = true;
	return 0;
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
		.genfb_mmap = impact_mmap,
		.genfb_init_screen = impact_init_screen,
		.genfb_enable_polling = impact_poll_on,
		.genfb_disable_polling = impact_poll_off
	};
	prop_dictionary_t props = device_properties(self);
	const char *status = arcbios_GetEnvironmentVariable("gfx");
	int error;
	bool console = impact_is_console && ga->ga_addr == impact_console_addr;

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
	sc->sc_pixels = console ? impact_console_pixels :
	    (void *)uvm_km_alloc(kernel_map, IMPACT_FBSIZE, 0,
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
	prop_dictionary_set_bool(props, "is_console", console);
	prop_dictionary_set_bool(props, "enable_shadowfb", false);
	genfb_init(&sc->sc_gen);
	error = genfb_attach(&sc->sc_gen, &ops);
	if (error == 0)
		error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL,
		    impact_update, sc, NULL, "impactfb");
	if (error != 0)
		aprint_error_dev(self, "framebuffer attachment failed: %d\n", error);
}
