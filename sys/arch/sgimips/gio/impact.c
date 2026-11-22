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
 * IMPACT_DMA enables HQ3 host-to-board uploads for dense shadow changes.
 */
#include <sys/cdefs.h>
__KERNEL_RCSID(0, "$NetBSD$");

#include "opt_impact.h"

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

#ifdef IMPACT_FRAME
#include <sgimips/gio/newport_frame.h>
#endif

#define IMPACT_PRODUCT 0x10
#define IMPACT_REG_SIZE 0x80000
#define IMPACT_WIDTH 1280
#define IMPACT_HEIGHT 1024
#define IMPACT_SPAN 64
#define IMPACT_TILE_HEIGHT 8
#define IMPACT_FBSIZE (IMPACT_WIDTH * IMPACT_HEIGHT * sizeof(uint32_t))

struct impact_softc {
	struct genfb_softc sc_gen;
	bus_space_tag_t sc_iot;
	bus_space_handle_t sc_ioh;
	uint32_t *sc_pixels;
	uint32_t *sc_previous;
#ifdef IMPACT_DMA
	bus_dma_tag_t sc_dmat;
	bus_dmamap_t sc_upload_map;
	void *sc_upload;
#endif
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
impact_wait(struct impact_softc *sc, unsigned maxlevel)
{
	/* Bound failures: an unresponsive board must not hang the kernel. */
	for (unsigned n = 0; n < 1000000; n++) {
		if ((bus_space_read_4(sc->sc_iot, sc->sc_ioh,
		    IMPACT_FIFOSTATUS) & IMPACTSR_FIFO_MASK) <= maxlevel)
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

static uint32_t
impact_rgb(uint32_t pixel)
{
	return ((pixel & 0xff) << 16) | (pixel & 0xff00) |
	    ((pixel >> 16) & 0xff);
}

static void
impact_block(struct impact_softc *sc, unsigned x, unsigned y,
    unsigned w, unsigned h)
{
	impact_cmd(sc, IMPACTSR_CMD_BLOCKXYSTARTI, IMPACTSR_XYCOORDS(x, y), false);
	impact_cmd(sc, IMPACTSR_CMD_BLOCKXYENDI,
	    IMPACTSR_XYCOORDS(x + w - 1, y + h - 1), false);
	impact_cmd(sc, IMPACTSR_CMD_IR_ALIAS, 0x18, true);
}

static bool
impact_fill(struct impact_softc *sc, unsigned x, unsigned y, unsigned w,
    unsigned h, uint32_t pixel)
{
	if (!impact_wait(sc, 0))
		return false;
	impact_cmd(sc, IMPACTSR_CMD_PP1FILLMODE,
	    IMPACTSR_PP1FILLMODE(0x6300, 3), false);
	impact_cmd(sc, IMPACTSR_CMD_FILLMODE, 0, false);
	impact_cmd(sc, IMPACTSR_CMD_PACKEDCOLOR, impact_rgb(pixel), false);
	impact_block(sc, x, y, w, h);
	return true;
}

static bool
impact_span(struct impact_softc *sc, unsigned x, unsigned y, const uint32_t *p,
    unsigned count)
{
	if (memcmp(p, p + 1, (count - 1) * sizeof(*p)) == 0)
		return impact_fill(sc, x, y, count, 1, p[0]);
	if (!impact_wait(sc, 0))
		return false;
	impact_cmd(sc, IMPACTSR_CMD_PP1FILLMODE,
	    IMPACTSR_PP1FILLMODE(0x6300, 3), false);
	impact_cmd(sc, IMPACTSR_CMD_FILLMODE, 3U << 22, false);
	impact_cmd(sc, IMPACTSR_CMD_XFRMODE, 0x80, false);
	impact_cmd(sc, IMPACTSR_CMD_XFRSIZE, IMPACTSR_YXCOORDS(count, 1), false);
	impact_block(sc, x, y, count, 1);
	for (unsigned i = 0; i < count; i += 2) {
		/*
		 * Reserve room below the programmed HQ3 high water mark (32)
		 * for eight 64-bit writes. The commands remain in FIFO order.
		 * Drain at each new block, not between pairs of pixels.
		 */
		if (i % 8 == 0 && !impact_wait(sc, 16))
			return false;
		impact_cmd(sc, IMPACTSR_CMD_CHAR_H, impact_rgb(p[i]), false);
		impact_cmd(sc, IMPACTSR_CMD_CHAR_L, impact_rgb(p[i + 1]), true);
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

/*
 * Coalesce uniform shadow pixels into a hardware rectangle. Keep the
 * comparison, FIFO transaction and previous-image update atomic with
 * respect to console output. One tile covers at most 2 KiB on this UP.
 */
static void
impact_update_tile(struct impact_softc *sc, unsigned x, unsigned y)
{
	uint32_t *p = sc->sc_pixels + y * IMPACT_WIDTH + x;
	uint32_t *prev = sc->sc_previous + y * IMPACT_WIDTH + x;
	const size_t rowbytes = IMPACT_SPAN * sizeof(*p);
	bool dirty = false;
	int s = splhigh();

	if (sc->sc_failed)
		goto done;
	if (memcmp(p, p + 1, rowbytes - sizeof(*p)) != 0)
		goto spans;
	for (unsigned row = 0; row < IMPACT_TILE_HEIGHT; row++) {
		unsigned off = row * IMPACT_WIDTH;
		if (row != 0 && memcmp(p, p + off, rowbytes) != 0)
			goto spans;
		dirty |= memcmp(prev + off, p + off, rowbytes) != 0;
	}
	if (dirty) {
		if (!impact_fill(sc, x, y, IMPACT_SPAN, IMPACT_TILE_HEIGHT, p[0])) {
			sc->sc_failed = true;
			goto done;
		}
		for (unsigned row = 0; row < IMPACT_TILE_HEIGHT; row++) {
			unsigned off = row * IMPACT_WIDTH;
			memcpy(prev + off, p, rowbytes);
		}
	}
done:
	splx(s);
	return;
spans:
	splx(s);
	impact_flush(sc, x, y, IMPACT_SPAN, IMPACT_TILE_HEIGHT);
}

#ifdef IMPACT_DMA
/* The HQ3 translation granule is independent of the kernel page size. */
#define IMPACT_HQ_PAGE	4096
#define IMPACT_BAND_BYTES (IMPACT_WIDTH * IMPACT_TILE_HEIGHT * sizeof(uint32_t))
#define IMPACT_UPLOAD_BYTES (IMPACT_HQ_PAGE + IMPACT_BAND_BYTES)

static bool
impact_dma_alloc(struct impact_softc *sc)
{
	bus_dma_segment_t seg;
	bus_dmamap_t map;
	void *va;
	int n;
	const size_t size = IMPACT_UPLOAD_BYTES;

	if (bus_dmamem_alloc(sc->sc_dmat, size, PAGE_SIZE, 0, &seg, 1, &n,
	    BUS_DMA_NOWAIT))
		return false;
	if (bus_dmamem_map(sc->sc_dmat, &seg, n, size, &va, BUS_DMA_NOWAIT))
		goto free;
	if (bus_dmamap_create(sc->sc_dmat, size, 1, size, 0, BUS_DMA_NOWAIT,
	    &map))
		goto unmap;
	if (bus_dmamap_load(sc->sc_dmat, map, va, size, NULL, BUS_DMA_NOWAIT))
		goto destroy;
	bus_addr_t addr = map->dm_segs[0].ds_addr;
	if (map->dm_nsegs != 1 || addr > UINT32_MAX - size || (addr & (IMPACT_HQ_PAGE - 1)) ||
	    gio_arb_config(GIO_SLOT_GFX, GIO_ARB_MST) != 0)
		goto unload;
	uint32_t *pt = va;
	for (unsigned i = 0; i < IMPACT_BAND_BYTES / IMPACT_HQ_PAGE; i++)
		pt[i] = (addr / IMPACT_HQ_PAGE) + 1 + i;
	bus_dmamap_sync(sc->sc_dmat, map, 0, IMPACT_HQ_PAGE, BUS_DMASYNC_PREWRITE);
	sc->sc_upload_map = map;
	sc->sc_upload = va;
	return true;

unload:
	bus_dmamap_unload(sc->sc_dmat, map);
destroy:
	bus_dmamap_destroy(sc->sc_dmat, map);
unmap:
	bus_dmamem_unmap(sc->sc_dmat, va, size);
free:
	bus_dmamem_free(sc->sc_dmat, &seg, n);
	return false;
}

/* HQ3 FIFO header with one 32-bit host-interface command argument. */
static void
impact_hq(struct impact_softc *sc, unsigned cmd, uint32_t v)
{
	uint64_t packet = ((uint64_t)((cmd << 8) | 4) << 32) | v;
	bus_space_write_8(sc->sc_iot, sc->sc_ioh, IMPACT_CFIFO, packet);
}

static bool
impact_upload(struct impact_softc *sc, unsigned x, unsigned y, unsigned w)
{
	const size_t stride = w * sizeof(uint32_t);
	const size_t size = stride * IMPACT_TILE_HEIGHT;
	uint32_t *buf = (uint32_t *)((char *)sc->sc_upload + IMPACT_HQ_PAGE);
	unsigned off = y * IMPACT_WIDTH + x;

	/* Only the worker uses this buffer.  While splhigh holds on IP28,
	 * neither console drawing nor X can change the shadow underneath us. */
	for (unsigned r = 0; r < IMPACT_TILE_HEIGHT; r++)
		memcpy(buf + r * w, sc->sc_pixels + off + r * IMPACT_WIDTH, stride);
	if (memcmp(buf, buf + 1, size - sizeof(*buf)) == 0) {
		if (!impact_fill(sc, x, y, w, IMPACT_TILE_HEIGHT, buf[0]))
			return false;
		goto submitted;
	}
	/* HQ3 expects ABGR bytes.  genfb's big-endian pixels have the red
	 * and blue lanes reversed relative to this transfer format. */
	for (size_t i = 0; i < size / sizeof(*buf); i++)
		buf[i] = impact_rgb(buf[i]);
	bus_dmamap_sync(sc->sc_dmat, sc->sc_upload_map, IMPACT_HQ_PAGE, size,
	    BUS_DMASYNC_PREWRITE);
	if (!impact_wait(sc, 0))
		return false;
	impact_cmd(sc, IMPACTSR_CMD_PP1FILLMODE,
	    IMPACTSR_PP1FILLMODE(0x6300, 3), false);
	impact_cmd(sc, IMPACTSR_CMD_FILLMODE, 5U << 22, false);
	impact_hq(sc, IMPACT_HQ_FORMATTER, 0x600);	/* Host formatter, 32-bit pixels. */
	impact_hq(sc, IMPACT_HQ_REIF_SCANWIDTH, stride);
	impact_hq(sc, IMPACT_HQ_REIF_DMACTRL, 0xc);
	impact_cmd(sc, IMPACTSR_CMD_XFRMODE, 0x80, false);
	unsigned extent = IMPACTSR_YXCOORDS(w, IMPACT_TILE_HEIGHT);
	impact_cmd(sc, IMPACTSR_CMD_XFRSIZE, extent, false);
	impact_cmd(sc, IMPACTSR_CMD_XFRCOUNTERS, extent, false);
	impact_block(sc, x, y, w, IMPACT_TILE_HEIGHT);
	impact_cmd(sc, IMPACTSR_CMD_XFRCONTROL, 1, false);
	/* Pool 0 maps consecutive host pages. The table uses a bus address,
	 * not a kernel virtual address. The transfer is host to board only. */
	impact_hq(sc, IMPACT_HQ_DMA_POOL0_TABLE, sc->sc_upload_map->dm_segs[0].ds_addr);
	impact_hq(sc, IMPACT_HQ_DMA_PAGELIST, 0x80000000);
	impact_hq(sc, IMPACT_HQ_DMA_STRIDE, stride);
	impact_hq(sc, IMPACT_HQ_DMA_ROWOFF, 0);
	impact_hq(sc, IMPACT_HQ_DMA_ROWSTART, 0);
	impact_hq(sc, IMPACT_HQ_DMA_LINES, IMPACT_TILE_HEIGHT);
	impact_hq(sc, IMPACT_HQ_DMA_LINEBYTES, stride);
	impact_hq(sc, IMPACT_HQ_DMA_START, 0xb1);
	bus_space_barrier(sc->sc_iot, sc->sc_ioh, IMPACT_CFIFO, 8,
	    BUS_SPACE_BARRIER_WRITE);
	for (unsigned i = 0; i < 10000; i++) {
		/* Wait for the FIFO, host interface and raster interface. */
		if (!(bus_space_read_4(sc->sc_iot, sc->sc_ioh, IMPACT_DMABUSY) &
		    IMPACT_HQ_BUSY_UPLOAD)) {
			impact_cmd(sc, IMPACTSR_CMD_XFRCONTROL, 0, false);
			bus_dmamap_sync(sc->sc_dmat, sc->sc_upload_map,
			    IMPACT_HQ_PAGE, size, BUS_DMASYNC_POSTWRITE);
			goto submitted;
		}
		delay(1);
	}
	return false;

submitted:
	for (unsigned r = 0; r < IMPACT_TILE_HEIGHT; r++)
		memcpy(sc->sc_previous + off + r * IMPACT_WIDTH,
		    sc->sc_pixels + off + r * IMPACT_WIDTH, stride);
	return true;
}

/* Group adjacent dirty tiles across eight scanlines. A single changed tile
 * keeps the small-fill/PIO path. No Xorg or userspace ABI is needed. */
static bool
impact_dma_band(struct impact_softc *sc, unsigned y)
{
	uint32_t *p = sc->sc_pixels + y * IMPACT_WIDTH;
	uint32_t *prev = sc->sc_previous + y * IMPACT_WIDTH;
	unsigned left = IMPACT_WIDTH, right = 0, dirty = 0;
	for (unsigned x = 0; x < IMPACT_WIDTH; x += IMPACT_SPAN) {
		for (unsigned r = 0; r < IMPACT_TILE_HEIGHT; r++) {
			unsigned pos = r * IMPACT_WIDTH + x;
			if (memcmp(p + pos, prev + pos, IMPACT_SPAN * sizeof(*p)) != 0) {
				left = MIN(left, x);
				right = x + IMPACT_SPAN;
				dirty += IMPACT_SPAN;
				break;
			}
		}
	}
	if (dirty < 2 * IMPACT_SPAN || dirty * 2 < right - left)
		return false;
	int s = splhigh();
	if (!impact_upload(sc, left, y, right - left))
		sc->sc_failed = true;
	splx(s);
	return true;
}

#endif /* IMPACT_DMA */

CTASSERT(IMPACT_WIDTH % IMPACT_SPAN == 0);
CTASSERT(IMPACT_HEIGHT % IMPACT_TILE_HEIGHT == 0);

static void
impact_update(void *cookie)
{
	struct impact_softc *sc = cookie;
	for (;;) {
		for (unsigned y = 0; !sc->sc_polling && !sc->sc_failed &&
		    y < IMPACT_HEIGHT; y += IMPACT_TILE_HEIGHT) {
			unsigned off = y * IMPACT_WIDTH;
			/* A concurrent write after this check is seen next pass. */
			if (memcmp(sc->sc_pixels + off, sc->sc_previous + off,
			    IMPACT_TILE_HEIGHT * IMPACT_WIDTH * sizeof(uint32_t)) == 0)
				continue;
#ifdef IMPACT_DMA
			if (sc->sc_upload != NULL && impact_dma_band(sc, y))
					continue;
#endif
			for (unsigned x = 0; x < IMPACT_WIDTH; x += IMPACT_SPAN)
				impact_update_tile(sc, x, y);
		}
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

#ifdef IMPACT_FRAME
CTASSERT(NEWPORT_FRAME_WIDTH >= IMPACT_WIDTH);
CTASSERT(NEWPORT_FRAME_HEIGHT == IMPACT_HEIGHT);
CTASSERT(2 * NEWPORT_FRAME_TEXT_X + NEWPORT_FRAME_TEXT_WIDTH == IMPACT_WIDTH);
CTASSERT(2 * NEWPORT_FRAME_TEXT_Y + NEWPORT_FRAME_TEXT_HEIGHT == IMPACT_HEIGHT);

static bool
impact_framed(struct rasops_info *ri)
{
	/* rasops requires a minimum of 20 columns and 10 rows. */
	return !FONT_IS_ALPHA(ri->ri_font) &&
	    20 * ri->ri_font->fontwidth <= NEWPORT_FRAME_TEXT_WIDTH &&
	    10 * ri->ri_font->fontheight <= NEWPORT_FRAME_TEXT_HEIGHT;
}

static uint32_t
impact_frame_colour(unsigned index)
{
	const uint8_t *rgb = newport_frame_cmap[index];

	return ((uint32_t)rgb[0] << 16) | ((uint32_t)rgb[1] << 8) | rgb[2];
}

/* Paint scanout and both RAM images together, even before UVM or in DDB. */
static void
impact_frame_rect(struct impact_softc *sc, unsigned x, unsigned y,
    unsigned w, unsigned h, uint32_t colour)
{
	while (h != 0) {
		unsigned rows = MIN(h, 4);
		int s = splhigh();
		if (sc->sc_failed || !impact_fill(sc, x, y, w, rows, colour)) {
			sc->sc_failed = true;
			splx(s);
			return;
		}
		for (unsigned row = 0; row < rows; row++) {
			unsigned off = (y + row) * IMPACT_WIDTH + x;
			for (unsigned col = 0; col < w; col++)
				sc->sc_pixels[off + col] = colour;
			if (sc->sc_previous != NULL)
				memcpy(sc->sc_previous + off, sc->sc_pixels + off,
				    w * sizeof(uint32_t));
		}
		splx(s);
		y += rows;
		h -= rows;
	}
}

static void
impact_frame_clear(struct rasops_info *ri, long attr)
{
	struct vcons_screen *scr = ri->ri_hw;
	struct impact_softc *sc = scr->scr_cookie;

	uint32_t bg = ri->ri_devcmap[((uint32_t)attr >> 16) & 0xff];

	for (size_t i = 0; i < __arraycount(newport_frame_rects); i++) {
		const struct newport_frame_rect *r = &newport_frame_rects[i];
		if (r->x >= IMPACT_WIDTH)
			continue;
		impact_frame_rect(sc, r->x, r->y,
		    MIN(r->width, IMPACT_WIDTH - r->x), r->height,
		    impact_frame_colour(r->colour));
	}
	if (bg != impact_frame_colour(NEWPORT_FRAME_PANEL))
		impact_frame_rect(sc, ri->ri_xorigin, ri->ri_yorigin,
		    ri->ri_emuwidth, ri->ri_emuheight, bg);
}
#endif

static void
impact_putchar(void *v, int row, int col, u_int c, long attr)
{
	struct rasops_info *ri = v;
#ifdef IMPACT_FRAME
	/* Index zero supplies the panel background. ANSI index 16 is black. */
	if (impact_framed(ri) && (((uint32_t)attr >> 24) & 0xff) == 0)
		attr |= 16UL << 24;
#endif
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
#ifdef IMPACT_FRAME
	if (row == 0 && num == ri->ri_rows && impact_framed(ri)) {
		impact_frame_clear(ri, attr);
		return;
	}
#endif
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
#ifdef IMPACT_FRAME
	if (impact_framed(ri)) {
		/* Keep physical geometry and stride for wsfb. Center the cell
		 * grid in the artwork's panel, including after a font change. */
		ri->ri_flg &= ~RI_CLEAR;
		ri->ri_flg |= RI_FULLCLEAR;
		rasops_reconfig(ri, NEWPORT_FRAME_TEXT_HEIGHT / ri->ri_font->fontheight,
		    NEWPORT_FRAME_TEXT_WIDTH / ri->ri_font->fontwidth);
		ri->ri_devcmap[0] = impact_frame_colour(NEWPORT_FRAME_PANEL);
		ri->ri_devcmap[16] = 0;
	}
#endif
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
	if (!impact_wait(sc, 0))
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
	if (!impact_wait(sc, 0)) {
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
#ifdef IMPACT_DMA
	sc->sc_dmat = ga->ga_dmat;
	if (error == 0 && impact_dma_alloc(sc))
		aprint_normal_dev(self, "HQ3 DMA shadow uploads enabled\n");
#endif
	if (error == 0)
		error = kthread_create(PRI_NONE, KTHREAD_MPSAFE, NULL,
		    impact_update, sc, NULL, "impactfb");
	if (error != 0)
		aprint_error_dev(self, "framebuffer attachment failed: %d\n", error);
}
