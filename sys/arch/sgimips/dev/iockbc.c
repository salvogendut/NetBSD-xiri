/* Octane IOC3 PS/2 ports in the onboard Bridge device window. */
#include <sys/param.h>
#include <sys/bus.h>
#include <sys/callout.h>
#include <sys/device.h>
#include <sys/systm.h>
#include <machine/autoconf.h>
#include <machine/intr.h>
#include <machine/machtype.h>
#include <dev/pckbport/pckbportvar.h>

#define KM_CSR		0
#define KM_RX(slot)	(4 + (slot) * 4)
#define KM_TX(slot)	(12 + (slot) * 4)
#define KM_SIZE		20
#define CLAMP_ONE	(3U << 20)
#define RX_VALID	(1U << 31)
#define RX_ERROR	(1U << 26)

struct iockbc_softc {
	bus_space_handle_t regs;
	pckbport_tag_t tag;
	callout_t callout;
	bool polling[PCKBPORT_NSLOTS];
	bool enabled[PCKBPORT_NSLOTS];
	bool attached;
};

static int
iockbc_match(device_t parent, cfdata_t cf, void *aux)
{
	struct mainbus_attach_args *ma = aux;
	return mach_type == MACH_SGI_IP30 && ma->ma_addr == 0x1f60009c;
}

static int
iockbc_translation(void *v, pckbport_slot_t slot, int on)
{
	/* No 8042 translation. pckbd requests set 1 directly. */
	return !on;
}

static int
iockbc_send(void *v, pckbport_slot_t slot, u_char byte)
{
	struct iockbc_softc *sc = v;
	for (unsigned n = 0; n < 250; n++) {
		if (!(bus_space_read_4(normal_memt, sc->regs, KM_CSR) & (1U << slot))) {
			bus_space_write_4(normal_memt, sc->regs, KM_TX(slot), byte);
			return 1;
		}
		delay(50);
	}
	return 0;
}

static int
iockbc_read(struct iockbc_softc *sc, pckbport_slot_t slot)
{
	uint32_t word = bus_space_read_4(normal_memt, sc->regs, KM_RX(slot));
	if ((word & (RX_VALID | RX_ERROR)) != RX_VALID)
		return -1;
	return (word >> 16) & 0xff;
}

static int
iockbc_poll(void *v, pckbport_slot_t slot)
{
	for (unsigned n = 0; n < 100; n++) {
		int byte = iockbc_read(v, slot);
		if (byte != -1)
			return byte;
		delay(10);
	}
	return -1;
}

static void
iockbc_slot_enable(void *v, pckbport_slot_t slot, int on)
{
	struct iockbc_softc *sc = v;
	sc->enabled[slot] = on;
}

static void
iockbc_set_poll(void *v, pckbport_slot_t slot, int on)
{
	struct iockbc_softc *sc = v;
	sc->polling[slot] = on;
}

static void
iockbc_input_establish(void *v, pckbport_slot_t slot)
{
	struct iockbc_softc *sc = v;
	sc->enabled[slot] = true;
}

static const struct pckbport_accessops iockbc_ops = {
	.t_xt_translation = iockbc_translation,
	.t_send_devcmd = iockbc_send,
	.t_poll_data1 = iockbc_poll,
	.t_slot_enable = iockbc_slot_enable,
	.t_intr_establish = iockbc_input_establish,
	.t_set_poll = iockbc_set_poll,
};

static void
iockbc_service(void *arg)
{
	struct iockbc_softc *sc = arg;
	int s = spltty();

	/* IOC3 shares its SuperIO interrupt with the serial console. Until
	 * both are children of an interrupt dispatcher, leave that line to
	 * com(4). CLAMP_ONE holds the PS/2 clock until each byte is consumed. */
	for (int slot = 0; slot < PCKBPORT_NSLOTS; slot++) {
		for (unsigned n = 0; n < 64 && sc->enabled[slot] &&
		    !sc->polling[slot]; n++) {
			int byte = iockbc_read(sc, slot);
			if (byte == -1)
				break;
			pckbportintr(sc->tag, slot, byte);
		}
	}
	splx(s);
	callout_schedule(&sc->callout, 1);
}

static int
iockbc_finalize(device_t self)
{
	struct iockbc_softc *sc = device_private(self);
	if (sc->attached)
		return 0;
	sc->attached = true;
	/* Take over both ports from PROM, release the lines and discard
	 * pre-existing receive words before the children reset their devices. */
	bus_space_write_4(normal_memt, sc->regs, KM_CSR, CLAMP_ONE);
	for (int slot = 0; slot < PCKBPORT_NSLOTS; slot++)
		(void)bus_space_read_4(normal_memt, sc->regs, KM_RX(slot));
	pckbport_cnattach(sc, &iockbc_ops, PCKBPORT_KBD_SLOT);
	sc->tag = pckbport_attach(sc, &iockbc_ops);
	for (int slot = 0; slot < PCKBPORT_NSLOTS; slot++)
		pckbport_attach_slot(self, sc->tag, slot);
	callout_init(&sc->callout, 0);
	callout_setfunc(&sc->callout, iockbc_service, sc);
	callout_schedule(&sc->callout, 1);
	return 0;
}

static void
iockbc_attach(device_t parent, device_t self, void *aux)
{
	struct iockbc_softc *sc = device_private(self);
	struct mainbus_attach_args *ma = aux;
	aprint_normal(": IOC3 PS/2 controller\n");
	if (bus_space_map(normal_memt, ma->ma_addr, KM_SIZE, 0, &sc->regs) != 0) {
		aprint_error_dev(self, "cannot map keyboard/mouse registers\n");
		return;
	}
	if (config_finalize_register(self, iockbc_finalize) != 0) {
		aprint_error_dev(self, "cannot register input attachment\n");
		bus_space_unmap(normal_memt, sc->regs, KM_SIZE);
	}
}

CFATTACH_DECL_NEW(iockbc, sizeof(struct iockbc_softc), iockbc_match,
    iockbc_attach, NULL, NULL);
