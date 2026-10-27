/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * BCM2712 power-management block: the V3D reset line
 * (nextbsd-kernel-extensions-graphics#66).
 *
 * WHY THIS EXISTS
 *
 * v3d's device-tree node takes a reset from this block:
 *
 *	resets        = <&pm 0>;        // BCM2835_RESET_V3D
 *	power-domains = <&pm 1>;
 *
 * and nothing claimed the node, so the reset never resolved:
 *
 *	simplebus0: <watchdog@7d200000> mem 0x7d200000-0x7d200307
 *	    compat brcm,bcm2712-pm (no driver attached)
 *
 * That is not a cosmetic gap. v3d_platform_drm_probe() does:
 *
 *	v3d->reset = devm_reset_control_get_exclusive(dev, NULL);
 *	if (IS_ERR(v3d->reset)) {
 *		...
 *		ret = map_regs(v3d, &v3d->bridge_regs, "bridge");
 *		if (ret) { "Failed to get reset control or bridge regs"; }
 *	}
 *
 * -- it falls back to a "bridge" register bank, and a 2712 device tree has no
 * such bank: reg-names is "hub", "core0", "sms". So a missing reset does not
 * degrade the driver, it fails the probe outright. v3d_reset_v3d() then needs
 * the same reset at runtime to recover from a GPU hang.
 *
 * ONLY THE RESET, DELIBERATELY
 *
 * The node also advertises #power-domain-cells, and this driver does not
 * implement power domains. That is not an omission: v3d makes no pm_runtime or
 * genpd calls anywhere, and sequences its own power through v3d_idle_sms() and
 * v3d_reset_sms() against the SMS register bank it maps itself. Implementing a
 * power-domain provider nothing asks for would be code with no caller.
 *
 * WHAT THE HARDWARE WANTS
 *
 * Derived from Linux drivers/pmdomain/bcm/bcm2835-power.c. On 2712 the V3D
 * domain reduces to one bit in one register, because the ASB (async AXI bridge)
 * handshake that earlier parts need is skipped entirely -- bcm2835_asb_control()
 * begins "case 0: return 0" and the 2712 call sites pass 0 for both ASB
 * registers:
 *
 *	bcm2835_asb_power_on(pd, PM_GRAFX_2712, 0, 0, PM_V3DRSTN)
 *
 * so all that remains of power_on/power_off is the reset bit, and reset is
 * power_off followed by power_on.
 *
 * NOTE THE POLARITY. PM_V3DRSTN is reset-NOT: the bit SET means the block is
 * running, and clearing it holds the block in reset. That is the opposite of
 * bcm_dvp's SW_INIT, where a set bit means held. Both polarities are now
 * represented in this tree, so neither can be assumed from the other.
 *
 * Every write carries PM_PASSWORD in the top byte; without it the write is
 * silently discarded by the hardware.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/rman.h>

#include <machine/bus.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <dev/hwreset/hwreset.h>

#include "hwreset_if.h"

/*
 * Register offsets, from bcm2835-power.c. PM_GRAFX_2712 is the 2712 spelling;
 * earlier parts use PM_GRAFX at 0x10c, which is a different register and is
 * deliberately not handled here.
 */
#define	PM_GRAFX_2712		0x304
#define	PM_V3DRSTN		(1u << 6)
#define	PM_PASSWORD		0x5a000000u

/* Reset ids, from dt-bindings/reset/raspberrypi,bcm2835-pm.h. */
#define	BCM2835_RESET_V3D	0
#define	PM_NRESETS		1

struct bcm2712_pm_softc {
	device_t		dev;
	struct resource		*mem;
	struct mtx		mtx;
};

#define	PM_RD4(sc, off)		bus_read_4((sc)->mem, (off))
/* Every write needs the password, so it lives in the accessor. */
#define	PM_WR4(sc, off, val)	\
	bus_write_4((sc)->mem, (off), PM_PASSWORD | (val))

static int
bcm2712_pm_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "brcm,bcm2712-pm"))
		return (ENXIO);

	device_set_desc(dev, "BCM2712 power management (V3D reset)");
	return (BUS_PROBE_DEFAULT);
}

static int
bcm2712_pm_attach(device_t dev)
{
	struct bcm2712_pm_softc *sc = device_get_softc(dev);
	int rid;

	sc->dev = dev;
	mtx_init(&sc->mtx, "bcm2712_pm", NULL, MTX_DEF);

	rid = 0;
	sc->mem = bus_alloc_resource_any(dev, SYS_RES_MEMORY, &rid, RF_ACTIVE);
	if (sc->mem == NULL) {
		device_printf(dev, "cannot allocate the pm register bank\n");
		mtx_destroy(&sc->mtx);
		return (ENXIO);
	}

	/*
	 * hwreset_register_ofw_provider() returns void -- it cannot fail and
	 * must not be tested. (It was tested against 0 once here, which does
	 * not compile.)
	 */
	hwreset_register_ofw_provider(dev);

	if (bootverbose)
		device_printf(dev, "V3D reset %s\n",
		    (PM_RD4(sc, PM_GRAFX_2712) & PM_V3DRSTN) != 0 ?
		    "deasserted (block running)" : "asserted (block held)");

	return (0);
}

static int
bcm2712_pm_detach(device_t dev)
{
	struct bcm2712_pm_softc *sc = device_get_softc(dev);

	/*
	 * The reset line is deliberately left as it is. Asserting it on detach
	 * would hold the GPU in reset under a v3d that is still attached, and
	 * this driver has no way to know whether anything is using it.
	 */
	if (sc->mem != NULL)
		bus_release_resource(dev, SYS_RES_MEMORY, 0, sc->mem);
	mtx_destroy(&sc->mtx);
	return (0);
}

static int
bcm2712_pm_reset_assert(device_t dev, intptr_t id, bool reset)
{
	struct bcm2712_pm_softc *sc = device_get_softc(dev);
	uint32_t val;

	if (id != BCM2835_RESET_V3D)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	val = PM_RD4(sc, PM_GRAFX_2712);
	/*
	 * Inverted, see the polarity note at the top: asserting reset CLEARS
	 * PM_V3DRSTN.
	 */
	if (reset)
		val &= ~PM_V3DRSTN;
	else
		val |= PM_V3DRSTN;
	PM_WR4(sc, PM_GRAFX_2712, val);
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
bcm2712_pm_reset_is_asserted(device_t dev, intptr_t id, bool *reset)
{
	struct bcm2712_pm_softc *sc = device_get_softc(dev);

	if (id != BCM2835_RESET_V3D)
		return (EINVAL);

	mtx_lock(&sc->mtx);
	*reset = (PM_RD4(sc, PM_GRAFX_2712) & PM_V3DRSTN) == 0;
	mtx_unlock(&sc->mtx);
	return (0);
}

static int
bcm2712_pm_reset_map(device_t dev, phandle_t xref, int ncells,
    pcell_t *cells, intptr_t *id)
{

	if (ncells != 1 || cells == NULL)
		return (EINVAL);
	if (cells[0] >= PM_NRESETS)
		return (EINVAL);
	*id = cells[0];
	return (0);
}

static device_method_t bcm2712_pm_methods[] = {
	DEVMETHOD(device_probe,		bcm2712_pm_probe),
	DEVMETHOD(device_attach,	bcm2712_pm_attach),
	DEVMETHOD(device_detach,	bcm2712_pm_detach),

	DEVMETHOD(hwreset_assert,	bcm2712_pm_reset_assert),
	DEVMETHOD(hwreset_is_asserted,	bcm2712_pm_reset_is_asserted),
	DEVMETHOD(hwreset_map,		bcm2712_pm_reset_map),

	DEVMETHOD_END
};

static driver_t bcm2712_pm_driver = {
	"bcm2712_pm",
	bcm2712_pm_methods,
	sizeof(struct bcm2712_pm_softc),
};

/*
 * BUS_PASS_BUS, like bcm_dvp: compiled in this would have to precede the
 * consumer that takes its reset. Loaded as a kext the pass no longer orders
 * anything, so V3D.kext names this bundle in OSBundleLibraries and kextd's
 * loader attaches it first.
 */
EARLY_DRIVER_MODULE(bcm2712_pm, simplebus, bcm2712_pm_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(bcm2712_pm, 1);
