/* XIO graphics inventory for the Xiri Octane lab kernel. */
#include "opt_ip30gfx.h"
#include "hq4fb.h"
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/device.h>
#include <sys/systm.h>

#include <machine/autoconf.h>
#include <machine/machtype.h>
#include <sgimips/dev/bridgereg.h>

#define XIO_SHORT_WINDOW_SIZE 0x01000000
#define HQ4_WIDGET_PART 0xc003
#define HQ4_WIDGET_MFG  0x2aa

#if NHQ4FB > 0
#include "hq4_pio.h"
#endif

#ifdef IP30_HQ4_FILL
bool ip30gfx_fill(bus_space_tag_t, bus_addr_t);
#endif

static int
ip30gfxprobe_match(device_t parent, cfdata_t cf, void *aux)
{
	const struct mainbus_attach_args *ma = aux;

	return mach_type == MACH_SGI_IP30 && ma->ma_addr == XIO_WIDGET0_BASE;
}

static void
ip30gfxprobe_attach(device_t parent, device_t self, void *aux)
{
	const struct mainbus_attach_args *ma = aux;
	bus_space_tag_t st = normal_memt;
	bus_space_handle_t xbow, widget;
	unsigned int port, boards = 0;
	uint32_t id;
	bool complete = true;

	aprint_normal(": XIO graphics inventory\n");
	if (bus_space_map(st, ma->ma_addr, 0x400, 0, &xbow) != 0) {
		aprint_error_dev(self, "cannot map Crossbow\n");
		return;
	}
	id = bus_space_read_4(st, xbow, XBOW_ID);
	if ((id & 0x0fffffff) != 1) {
		aprint_error_dev(self, "unexpected Crossbow id %#x\n", id);
		bus_space_unmap(st, xbow, 0x400);
		return;
	}

	/* Ports 8 and 15 serve HEART and the base I/O Bridge on IP30. */
	for (port = 9; port < 15; port++) {
		bus_size_t link = XBOW_LINK_BASE + (port - 8) * XBOW_LINK_STRIDE;
		uint32_t status = bus_space_read_4(st, xbow, link + XBOW_LINK_STATUS);
		bus_addr_t base;

		if (status == 0xffffffffU || (status & XBOW_LINK_ALIVE) == 0)
			continue;
		base = XIO_WIDGET0_BASE + port * XIO_SHORT_WINDOW_SIZE;
		if (bus_space_map(st, base, 8, 0, &widget) != 0) {
			aprint_error_dev(self, "cannot map widget %u\n", port);
			complete = false;
			continue;
		}
		id = bus_space_read_4(st, widget, XBOW_ID);
		bus_space_unmap(st, widget, 8);
		if ((id & 1) && ((id >> 12) & 0xffff) == HQ4_WIDGET_PART &&
		    ((id >> 1) & 0x7ff) == HQ4_WIDGET_MFG) {
			aprint_normal_dev(self, "IMPACT HQ4 at widget %u, id %#x\n",
			    port, id);
			boards++;
#if NHQ4FB > 0
			struct hq4_attach_args ha = { st, base };
			config_found(self, &ha, NULL, CFARGS(.iattr = "ip30hq4bus"));
#endif
#ifdef IP30_HQ4_FILL
			if (ip30gfx_fill(st, base))
				aprint_normal_dev(self, "HQ4 rectangle submitted\n");
			else
				aprint_error_dev(self, "HQ4 rectangle submission failed\n");
#endif
		}
	}
	bus_space_unmap(st, xbow, 0x400);
	if (complete)
		aprint_normal_dev(self, "%u IMPACT board(s)\n", boards);
}

CFATTACH_DECL_NEW(ip30gfxprobe, 0, ip30gfxprobe_match, ip30gfxprobe_attach,
    NULL, NULL);
