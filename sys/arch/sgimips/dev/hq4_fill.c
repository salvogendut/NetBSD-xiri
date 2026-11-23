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

/* Opt-in rectangle qualification. */
#include <sys/param.h>
#include <sys/bus.h>
#include "hq4_pio.h"

bool ip30gfx_fill(bus_space_tag_t, bus_addr_t);

bool
ip30gfx_fill(bus_space_tag_t st, bus_addr_t base)
{
	bus_space_handle_t h;
	bool ready;

	if (bus_space_map(st, base, HQ4_REG_SIZE, 0, &h) != 0)
		return false;
	ready = hq4_fifo_empty(st, h);
	if (ready) {
		hq4_setup(st, h);
		hq4_rectangle(st, h, 0, 0, HQ4_WIDTH, HQ4_HEIGHT, 0x21354b);
		hq4_rectangle(st, h, 117, 203, 413, 239, 0xf4a139);
		ready = hq4_fifo_empty(st, h);
	}
	bus_space_unmap(st, h, HQ4_REG_SIZE);
	return ready;
}
