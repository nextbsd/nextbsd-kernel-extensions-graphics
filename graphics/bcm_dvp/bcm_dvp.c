/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * BCM2711/2712 DVP -- display video path clocks and resets
 * (nextbsd-kernel-extensions-graphics#51).
 *
 * WHY THIS EXISTS
 *
 * With firmware clocks provided, vc4 programs the HDMI state machine for real
 * -- hw.clock.rpifw-m2mc.frequency goes from 0 to 365872500 for a 2560x1440@60
 * mode -- the CRTC goes ACTIVE with a real mode blob, and the panel still gets
 * no signal at all. Its backlight goes out and stays out. The one error left is
 *
 *	vc40: [drm] *ERROR* Failed to wait for infoframe to go idle: -60
 *
 * ETIMEDOUT, measured on a Pi 500+: the block accepts register writes and never
 * reaches a ready state, which is what a device still held in reset looks like.
 *
 * hdmi@7ef00700 says who holds it:
 *
 *	resets = <&dvp 1>;
 *	clocks = <&firmware_clocks 13>, <&firmware_clocks 14>, <&dvp 0>, ...;
 *
 * and dvp is clock@7c700000, compatible "brcm,brcm2711-dvp", which had no
 * driver:
 *
 *	simplebus0: <clock@7c700000> mem 0x7c700000-0x7c70000f
 *	    compat brcm,brcm2711-dvp (no driver attached)
 *
 * devm_reset_control_get() was made optional earlier in this port precisely
 * because nothing could satisfy it. That let bind finish; it did not bring the
 * block out of reset, and nothing else was going to.
 *
 * REGISTER LAYOUT
 *
 * From Linux's drivers/clk/bcm/clk-bcm2711-dvp.c, which is the only
 * description of this block:
 *
 *	0x04  SW_INIT      one reset per bit, 6 of them, 1 = held in reset
 *	0x0c  MISC_CONFIG  gate bits 3 and 4 for the two HDMI 108MHz clocks,
 *	                   SET_TO_DISABLE -- a 1 turns the clock OFF
 *
 * The whole node is 16 bytes, which matches the newbus line above.
 *
 * The inverted gate sense is worth stating because getting it backwards would
 * silently do the opposite of what is intended and look like this same bug.
 */

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/mutex.h>
#include <sys/lock.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/clk/clk.h>
#include <dev/clk/clk_gate.h>
#include <dev/hwreset/hwreset.h>

#include "clkdev_if.h"
#include "hwreset_if.h"

#define	DVP_SW_INIT		0x04
#define	DVP_MISC_CONFIG		0x0c

#define	DVP_NRESETS		6
#define	DVP_NCLOCKS		2

/* MISC_CONFIG gate bits, SET_TO_DISABLE. */
#define	DVP_GATE_HDMI0		3
#define	DVP_GATE_HDMI1		4

struct bcm_dvp_softc {
	device_t		 dev;
	struct resource		*mem_res;
	struct mtx		 mtx;
	struct clkdom		*clkdom;
};

#define	DVP_RD4(sc, o)		bus_read_4((sc)->mem_res, (o))
#define	DVP_WR4(sc, o, v)	bus_write_4((sc)->mem_res, (o), (v))

/*
 * clkdev back end. The gate clknodes reach the registers through these, and
 * the lock is shared with the reset methods because SW_INIT and MISC_CONFIG
 * are read-modify-written from both.
 */
static int
bcm_dvp_clk_write(device_t dev, bus_addr_t addr, uint32_t val)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);

	DVP_WR4(sc, addr, val);
	return (0);
}

static int
bcm_dvp_clk_read(device_t dev, bus_addr_t addr, uint32_t *val)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);

	*val = DVP_RD4(sc, addr);
	return (0);
}

static int
bcm_dvp_clk_modify(device_t dev, bus_addr_t addr, uint32_t clr, uint32_t set)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);
	uint32_t val;

	val = DVP_RD4(sc, addr);
	val &= ~clr;
	val |= set;
	DVP_WR4(sc, addr, val);
	return (0);
}

static void
bcm_dvp_clk_lock(device_t dev)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);

	mtx_lock(&sc->mtx);
}

static void
bcm_dvp_clk_unlock(device_t dev)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);

	mtx_unlock(&sc->mtx);
}

/*
 * Resets. SW_INIT holds one bit per reset line and a 1 means held, so assert
 * sets and deassert clears -- the opposite of the gate bits next door, which
 * is exactly the kind of thing worth being explicit about.
 */
static int
bcm_dvp_reset_assert(device_t dev, intptr_t id, bool reset)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (id < 0 || id >= DVP_NRESETS)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	val = DVP_RD4(sc, DVP_SW_INIT);
	if (reset)
		val |= (1u << id);
	else
		val &= ~(1u << id);
	DVP_WR4(sc, DVP_SW_INIT, val);

	mtx_unlock(&sc->mtx);
	return (0);
}

static int
bcm_dvp_reset_is_asserted(device_t dev, intptr_t id, bool *reset)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);

	if (id < 0 || id >= DVP_NRESETS)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	*reset = (DVP_RD4(sc, DVP_SW_INIT) & (1u << id)) != 0;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
bcm_dvp_reset_map(device_t dev, phandle_t xref, int ncells,
    pcell_t *cells, intptr_t *id)
{

	if (ncells != 1 || cells == NULL)
		return (EINVAL);
	if (cells[0] >= DVP_NRESETS)
		return (EINVAL);
	*id = cells[0];
	return (0);
}

static int
bcm_dvp_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "brcm,brcm2711-dvp"))
		return (ENXIO);

	device_set_desc(dev, "BCM2711 DVP clocks and resets");
	return (BUS_PROBE_DEFAULT);
}

static int
bcm_dvp_attach(device_t dev)
{
	struct bcm_dvp_softc *sc = device_get_softc(dev);
	struct clk_gate_def def;
	int rid, i, error;
	static const struct {
		const char	*name;
		int		 shift;
	} gates[DVP_NCLOCKS] = {
		{ "hdmi0-108MHz", DVP_GATE_HDMI0 },
		{ "hdmi1-108MHz", DVP_GATE_HDMI1 },
	};

	sc->dev = dev;
	mtx_init(&sc->mtx, "bcm_dvp", NULL, MTX_DEF);

	rid = 0;
	sc->mem_res = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (sc->mem_res == NULL) {
		device_printf(dev, "could not map registers\n");
		mtx_destroy(&sc->mtx);
		return (ENXIO);
	}

	sc->clkdom = clkdom_create(dev);
	if (sc->clkdom == NULL) {
		error = ENOMEM;
		goto fail;
	}

	/*
	 * No parent is named. Gating does not need to know the rate, and vc4
	 * asks these clocks to be enabled, never retuned.
	 */
	for (i = 0; i < DVP_NCLOCKS; i++) {
		memset(&def, 0, sizeof(def));
		def.clkdef.id = i;
		def.clkdef.name = gates[i].name;
		def.clkdef.parent_names = NULL;
		def.clkdef.parent_cnt = 0;
		def.offset = DVP_MISC_CONFIG;
		def.shift = gates[i].shift;
		def.mask = 1;
		def.on_value = 0;	/* SET_TO_DISABLE: 0 enables */
		def.off_value = 1;

		error = clknode_gate_register(sc->clkdom, &def);
		if (error != 0) {
			device_printf(dev, "could not register %s: %d\n",
			    gates[i].name, error);
			goto fail;
		}
	}

	if (clkdom_finit(sc->clkdom) != 0) {
		device_printf(dev, "could not finalise the clock domain\n");
		error = ENXIO;
		goto fail;
	}

	/* Returns void -- it has no failure path to report. */
	hwreset_register_ofw_provider(dev);

	device_printf(dev, "%d clocks, %d resets (#51)\n", DVP_NCLOCKS,
	    DVP_NRESETS);
	return (0);

fail:
	bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem_res);
	mtx_destroy(&sc->mtx);
	return (error);
}

static int
bcm_dvp_detach(device_t dev)
{

	/* Consumers hold clocks and resets from this node. */
	return (EBUSY);
}

static device_method_t bcm_dvp_methods[] = {
	DEVMETHOD(device_probe,		bcm_dvp_probe),
	DEVMETHOD(device_attach,	bcm_dvp_attach),
	DEVMETHOD(device_detach,	bcm_dvp_detach),

	/* clkdev, for the gate clknodes. */
	DEVMETHOD(clkdev_write_4,	bcm_dvp_clk_write),
	DEVMETHOD(clkdev_read_4,	bcm_dvp_clk_read),
	DEVMETHOD(clkdev_modify_4,	bcm_dvp_clk_modify),
	DEVMETHOD(clkdev_device_lock,	bcm_dvp_clk_lock),
	DEVMETHOD(clkdev_device_unlock,	bcm_dvp_clk_unlock),

	/* hwreset. */
	DEVMETHOD(hwreset_assert,	bcm_dvp_reset_assert),
	DEVMETHOD(hwreset_is_asserted,	bcm_dvp_reset_is_asserted),
	DEVMETHOD(hwreset_map,		bcm_dvp_reset_map),

	DEVMETHOD_END
};

static driver_t bcm_dvp_driver = {
	"bcm_dvp",
	bcm_dvp_methods,
	sizeof(struct bcm_dvp_softc),
};

/*
 * BUS_PASS_BUS: compiled in, this must precede the display blocks that take
 * its resets. Loaded as a module the pass no longer gates anything, so load
 * this kext before VideoCore6KMS.
 */
EARLY_DRIVER_MODULE(bcm_dvp, simplebus, bcm_dvp_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(bcm_dvp, 1);
