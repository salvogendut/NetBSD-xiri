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

/* HQ4 programmed I/O, retaining PROM display timing. */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/systm.h>
#include <sgimips/gio/impactreg.h>
#include "hq4_pio.h"

void
hq4_cmd(bus_space_tag_t st, bus_space_handle_t h, unsigned reg, uint32_t value,
    bool execute)
{
	uint64_t word = IMPACTSR_CFIFO_WRITE |
	    ((uint64_t)reg << IMPACTSR_CFIFO_REG_SHIFT) | value;
	bus_space_write_8(st, h, IMPACTSR_CFIFO,
	    execute ? word | IMPACTSR_CFIFO_EXEC : word);
}

bool
hq4_fifo_empty(bus_space_tag_t st, bus_space_handle_t h)
{
	for (unsigned i = 0; i < 10000; i++) {
		if ((bus_space_read_4(st, h, IMPACTSR_FIFOSTATUS) &
		    IMPACTSR_FIFO_MASK) == 0)
			return true;
		delay(10);
	}
	return false;
}

void
hq4_rectangle(bus_space_tag_t st, bus_space_handle_t h, unsigned x, unsigned y,
    unsigned w, unsigned height, uint32_t rgb)
{
	/* RSS takes red in bits 7:0 of PACKEDCOLOR. */
	uint32_t packed = ((rgb & 0xff) << 16) | (rgb & 0xff00) | (rgb >> 16);
	hq4_cmd(st, h, IMPACTSR_CMD_PACKEDCOLOR, packed, false);
	hq4_cmd(st, h, IMPACTSR_CMD_BLOCKXYSTARTI, IMPACTSR_XYCOORDS(x, y), false);
	hq4_cmd(st, h, IMPACTSR_CMD_BLOCKXYENDI,
	    IMPACTSR_XYCOORDS(x + w - 1, y + height - 1), false);
	hq4_cmd(st, h, IMPACTSR_CMD_IR_ALIAS, 0x18, true);
}

void
hq4_setup(bus_space_tag_t st, bus_space_handle_t h)
{
	bus_space_write_4(st, h, IMPACTSR_CFIFO_HW, 0x47);
	bus_space_write_4(st, h, IMPACTSR_CFIFO_LW, 0x14);
	bus_space_write_4(st, h, IMPACTSR_VC3_BASE + IMPACTSR_VC3_INDEXDATA,
	    0x1d000100);	/* disable the hardware cursor */
	bus_space_write_1(st, h, IMPACTSR_XMAP_BASE + IMPACTSR_XMAP_PP1SELECT, 1);
	bus_space_write_1(st, h, IMPACTSR_XMAP_BASE + IMPACTSR_XMAP_INDEX, 0);
	bus_space_write_4(st, h, IMPACTSR_XMAP_BASE + IMPACTSR_XMAP_MAIN_MODE,
	    0x7a4);	/* 24-bit RGB */
	hq4_cmd(st, h, IMPACTSR_CMD_COLORMASKLSBSA, 0xffffff, false);
	hq4_cmd(st, h, IMPACTSR_CMD_COLORMASKLSBSB, 0xffffff, false);
	hq4_cmd(st, h, IMPACTSR_CMD_COLORMASKMSBS, 0, false);
	hq4_cmd(st, h, IMPACTSR_CMD_XFRMASKLO, 0xffffff, false);
	hq4_cmd(st, h, IMPACTSR_CMD_XFRMASKHI, 0xffffff, false);
	hq4_cmd(st, h, IMPACTSR_CMD_DRBPOINTERS, 0xc8240, false);
	hq4_cmd(st, h, IMPACTSR_CMD_CONFIG, 0xcac, false);
	hq4_cmd(st, h, IMPACTSR_CMD_XYWIN, IMPACTSR_YXCOORDS(0, 0x3ff), false);
	hq4_cmd(st, h, IMPACTSR_CMD_PP1FILLMODE, IMPACTSR_PP1FILLMODE(0x6300, 3), false);
	hq4_cmd(st, h, IMPACTSR_CMD_FILLMODE, 0, false);
}
