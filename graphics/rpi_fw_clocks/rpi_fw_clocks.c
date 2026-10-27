/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * Raspberry Pi firmware clocks (nextbsd-kernel-extensions-graphics#51).
 *
 * WHY THIS EXISTS
 *
 * The vc4 KMS driver binds, reads EDID over the firmware mailbox, reports 57
 * modes from the attached panel and accepts a modeset at its native
 * 2560x1440@60 -- drmModeSetCrtc returns 0 and the CRTC goes ACTIVE. And the
 * monitor sleeps, because no clock was ever programmed.
 *
 * The device tree wires it up correctly. hdmi@7ef00700 carries:
 *
 *	clock-names = "hdmi", "bvb", "audio", "cec";
 *	clocks = <&firmware_clocks 13>, <&firmware_clocks 14>, ... ;
 *
 * read off a live Pi 500+, where 13 and 14 are RPI_FIRMWARE_M2MC_CLK_ID and
 * RPI_FIRMWARE_PIXEL_BVB_CLK_ID. What is missing is a driver for the provider:
 * /soc/firmware/clocks is compatible "raspberrypi,firmware-clocks" and nothing
 * attaches to it, so clk_get_by_ofw_name() fails, LinuxKPI's
 * devm_clk_get_optional() turns that into NULL, and every clk_set_rate() and
 * clk_prepare_enable() in vc4_hdmi silently becomes a no-op:
 *
 *	clk_set_min_rate(vc4_hdmi->hsm_clock, hsm_rate);      -> NULL, 0
 *	clk_set_rate(vc4_hdmi->pixel_clock, tmds_char_rate);  -> NULL, 0
 *	clk_prepare_enable(vc4_hdmi->pixel_clock);            -> NULL, 0
 *
 * so the HDMI state machine never gets a rate, the PHY has no clock, and there
 * is no TMDS signal to send. Nothing reports an error anywhere along that path,
 * which is why it presented as "the modeset works but the screen is dark".
 *
 * The kernel already has the mailbox underneath -- bcm2835_firmware(4), which
 * registers its node with OF_device_register_xref() during attach. This only
 * adds the clock provider on top of it.
 *
 * WHAT IT DOES NOT DO
 *
 * Rates are whatever the firmware decides. SET_CLOCK_RATE returns the rate it
 * actually applied, which may not be the rate asked for, and that returned
 * value is what gets reported back -- no rounding policy of our own, because
 * the firmware owns these clocks and is the only thing that knows the
 * constraints.
 *
 * Enable/disable are not implemented: the firmware has no per-clock gate in
 * this interface. clknode_enable() therefore succeeds without doing anything,
 * which is honest for a clock that is on whenever it has a rate.
 */

#include "opt_platform.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/malloc.h>

#include <machine/bus.h>

#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>
#include <dev/clk/clk.h>

#include <arm/broadcom/bcm2835/bcm2835_firmware.h>

/*
 * Firmware clock ids, from the mailbox interface. Same numbering the device
 * tree uses in its clock specifiers, which is what makes the mapping trivial:
 * the DT cell IS the firmware id.
 */
#define	RPI_FW_CLK_EMMC		1
#define	RPI_FW_CLK_UART		2
#define	RPI_FW_CLK_ARM		3
#define	RPI_FW_CLK_CORE		4
#define	RPI_FW_CLK_V3D		5
#define	RPI_FW_CLK_H264		6
#define	RPI_FW_CLK_ISP		7
#define	RPI_FW_CLK_SDRAM	8
#define	RPI_FW_CLK_PIXEL	9
#define	RPI_FW_CLK_PWM		10
#define	RPI_FW_CLK_HEVC		11
#define	RPI_FW_CLK_EMMC2	12
#define	RPI_FW_CLK_M2MC		13
#define	RPI_FW_CLK_PIXEL_BVB	14
#define	RPI_FW_CLK_VEC		15
#define	RPI_FW_CLK_DISP		16
#define	RPI_FW_CLK_MAX		17

#define	RPI_FW_TAG_GET_CLOCK_STATE	0x00030001
#define	RPI_FW_TAG_SET_CLOCK_STATE	0x00038001
#define	RPI_FW_TAG_GET_CLOCK_RATE	0x00030002
#define	RPI_FW_TAG_SET_CLOCK_RATE	0x00038002
#define	RPI_FW_TAG_GET_MAX_CLOCK_RATE	0x00030004

/* SET/GET_CLOCK_STATE state bits. */
#define	RPI_FW_CLOCK_STATE_ON		(1u << 0)
#define	RPI_FW_CLOCK_STATE_NOT_FOUND	(1u << 1)

/*
 * Names are prefixed because clock names are GLOBAL in this framework, not
 * per-domain. Registering the obvious ones panicked the machine outright:
 *
 *	panic: Duplicated clock registration: core
 *
 * measured on a Pi 500+ -- something else already owns "core", and probably
 * "arm", "uart" and "emmc" too. Nothing resolves clocks by these names: the
 * device tree specifier carries the firmware id and clk_get_by_ofw_index()
 * matches on that, so the name is only for identification in sysctls and
 * boot output.
 */
static const char *rpi_fw_clk_names[RPI_FW_CLK_MAX] = {
	[RPI_FW_CLK_EMMC]	= "rpifw-emmc",
	[RPI_FW_CLK_UART]	= "rpifw-uart",
	[RPI_FW_CLK_ARM]	= "rpifw-arm",
	[RPI_FW_CLK_CORE]	= "rpifw-core",
	[RPI_FW_CLK_V3D]	= "rpifw-v3d",
	[RPI_FW_CLK_H264]	= "rpifw-h264",
	[RPI_FW_CLK_ISP]	= "rpifw-isp",
	[RPI_FW_CLK_SDRAM]	= "rpifw-sdram",
	[RPI_FW_CLK_PIXEL]	= "rpifw-pixel",
	[RPI_FW_CLK_PWM]	= "rpifw-pwm",
	[RPI_FW_CLK_HEVC]	= "rpifw-hevc",
	[RPI_FW_CLK_EMMC2]	= "rpifw-emmc2",
	[RPI_FW_CLK_M2MC]	= "rpifw-m2mc",
	[RPI_FW_CLK_PIXEL_BVB]	= "rpifw-pixel-bvb",
	[RPI_FW_CLK_VEC]	= "rpifw-vec",
	[RPI_FW_CLK_DISP]	= "rpifw-disp",
};

struct rpi_fw_clocks_softc {
	device_t	dev;
	device_t	fwdev;		/* bcm2835_firmware(4) */
	struct clkdom	*clkdom;
};

struct rpi_fw_clknode_sc {
	device_t	fwdev;
	uint32_t	id;
};

struct rpi_fw_clk_msg {
	uint32_t	id;
	uint32_t	rate;
	uint32_t	skip_turbo;	/* SET only; 0 = let the firmware decide */
};

static int
rpi_fw_clk_get(device_t fwdev, uint32_t tag, uint32_t id, uint32_t *rate)
{
	struct rpi_fw_clk_msg msg;
	int error;

	memset(&msg, 0, sizeof(msg));
	msg.id = id;
	error = bcm2835_firmware_property(fwdev, tag, &msg,
	    sizeof(uint32_t) * 2);
	if (error != 0)
		return (error);
	*rate = msg.rate;
	return (0);
}

static int
rpi_fw_clknode_recalc(struct clknode *clk, uint64_t *freq)
{
	struct rpi_fw_clknode_sc *sc = clknode_get_softc(clk);
	uint32_t rate;

	if (rpi_fw_clk_get(sc->fwdev, RPI_FW_TAG_GET_CLOCK_RATE, sc->id,
	    &rate) != 0) {
		*freq = 0;
		return (0);
	}
	*freq = rate;
	return (0);
}

static int
rpi_fw_clknode_set_freq(struct clknode *clk, uint64_t fin, uint64_t *fout,
    int flags, int *stop)
{
	struct rpi_fw_clknode_sc *sc = clknode_get_softc(clk);
	struct rpi_fw_clk_msg msg;
	int error;

	if (*fout > UINT32_MAX)
		return (EINVAL);

	/*
	 * A dry run must not touch the hardware. Report the request back
	 * unchanged: the firmware is the only thing that knows what it can
	 * actually produce, and asking it costs a mailbox round trip per
	 * candidate rate during rate negotiation.
	 */
	if ((flags & CLK_SET_DRYRUN) != 0) {
		*stop = 1;
		return (0);
	}

	memset(&msg, 0, sizeof(msg));
	msg.id = sc->id;
	msg.rate = (uint32_t)*fout;
	msg.skip_turbo = 0;
	error = bcm2835_firmware_property(sc->fwdev,
	    RPI_FW_TAG_SET_CLOCK_RATE, &msg, sizeof(msg));
	if (error != 0)
		return (error);

	/*
	 * SET_CLOCK_RATE reports the rate it actually applied, which is not
	 * necessarily the one asked for. Report that rather than the request,
	 * so a caller reading the rate back sees the truth.
	 */
	*fout = msg.rate;
	*stop = 1;
	return (0);
}

static int
rpi_fw_clknode_init(struct clknode *clk, device_t dev)
{

	clknode_init_parent_idx(clk, 0);
	return (0);
}

/*
 * Turn the clock on and off (#51).
 *
 * This driver previously had no gate method, on the stated grounds that "the
 * firmware exposes no per-clock gate through this interface". That was an
 * assumption written as a fact, and it is wrong: SET_CLOCK_STATE (0x00038001)
 * is exactly that gate.
 *
 * The cost of the omission was total. vc4_drm_bind() sends
 * NOTIFY_DISPLAY_DONE, after which the firmware stops the HDMI state machine
 * clock; the driver then set a rate on it and called clk_prepare_enable(),
 * which raised a refcount in the clock framework and reached no hardware,
 * because there was no gate method for it to call. So the clock had a rate of
 * 365872500 and was not running.
 *
 * That is what left the HDMI block half alive: registers the driver writes
 * read back, because those are latches on the always-on side, while every
 * register the state machine generates read 0xffffffff -- HDMI_HOTPLUG,
 * HDMI_SCHEDULER_CONTROL, HDMI_RAM_PACKET_STATUS -- measured on a Pi 500+
 * before the reset, after the reset, before and after phy_init, all identical.
 */
static int
rpi_fw_clknode_set_gate(struct clknode *clk, bool enable)
{
	struct rpi_fw_clknode_sc *sc = clknode_get_softc(clk);
	struct rpi_fw_clk_msg msg;
	int error;

	memset(&msg, 0, sizeof(msg));
	msg.id = sc->id;
	msg.rate = enable ? RPI_FW_CLOCK_STATE_ON : 0;	/* the state word */
	error = bcm2835_firmware_property(sc->fwdev,
	    RPI_FW_TAG_SET_CLOCK_STATE, &msg, sizeof(uint32_t) * 2);
	if (error != 0)
		return (error);

	/*
	 * The firmware replies in the same buffer and sets NOT_FOUND for an id
	 * it does not have. Reporting that as success would hand out a clock
	 * that silently does nothing, which is the failure this whole change
	 * exists to remove.
	 */

	if ((msg.rate & RPI_FW_CLOCK_STATE_NOT_FOUND) != 0)
		return (ENXIO);

	return (0);
}

static clknode_method_t rpi_fw_clknode_methods[] = {
	CLKNODEMETHOD(clknode_init,		rpi_fw_clknode_init),
	CLKNODEMETHOD(clknode_recalc_freq,	rpi_fw_clknode_recalc),
	CLKNODEMETHOD(clknode_set_freq,		rpi_fw_clknode_set_freq),
	CLKNODEMETHOD(clknode_set_gate,		rpi_fw_clknode_set_gate),
	CLKNODEMETHOD_END
};

DEFINE_CLASS_1(rpi_fw_clknode, rpi_fw_clknode_class, rpi_fw_clknode_methods,
    sizeof(struct rpi_fw_clknode_sc), clknode_class);

/*
 * Take the node back from FreeBSD's generic ofw_clkbus (#65).
 *
 * ofw_clkbus matches on the node NAME alone -- strcmp(name, "clocks") -- at
 * BUS_PROBE_GENERIC, and it is compiled into the kernel, so at boot it claims
 * /soc/firmware/clocks before any kext exists. It then enumerates the node's
 * children, of which there are none: this node is a provider with
 * #clock-cells = 1, not a bus. It ends up owning the node and supplying
 * nothing, which is why every clk_get_by_ofw_name() against it returns NULL.
 *
 * rpi_fw_clocks_probe() returns BUS_PROBE_SPECIFIC and would win that contest
 * outright -- but only if it were ever run. bus_generic_driver_added() probes
 * a bus's children only when they are DS_NOTPRESENT, and the incumbent has
 * left this one DS_ATTACHED, so a driver arriving later never gets a look.
 * That is what forced the manual step this replaces:
 *
 *	devctl detach ofw_clkbus1
 *	kextload .../RpiFirmwareClocks.kext
 *
 * DEVICE_IDENTIFY is the hook that makes it automatic. bus_generic_driver_added()
 * calls it BEFORE the DS_NOTPRESENT sweep, so detaching the incumbent here puts
 * the child back in DS_NOTPRESENT in time for the sweep that follows to offer
 * it to us. device_detach() clears the driver and sets that state; the topology
 * lock it asserts is already held, since the same function goes on to call
 * device_probe_and_attach(), which asserts it too.
 *
 * Deliberately narrow. It only touches a node this driver would itself claim,
 * identified by compatible rather than by the incumbent's name, and it never
 * touches one already ours. A node whose owner refuses to detach is left alone
 * and reported -- that is a driver holding a resource, not something to force.
 */
static void
rpi_fw_clocks_identify(driver_t *driver, device_t parent)
{
	device_t *kids;
	int i, nkids;

	if (device_get_children(parent, &kids, &nkids) != 0)
		return;

	for (i = 0; i < nkids; i++) {
		char who[32];

		if (device_get_driver(kids[i]) == NULL)
			continue;	/* unowned: the sweep will offer it */
		if (device_get_driver(kids[i]) == driver)
			continue;	/* already ours */
		if (!ofw_bus_is_compatible(kids[i],
		    "raspberrypi,firmware-clocks"))
			continue;

		/*
		 * Copied BEFORE the detach, not after: device_detach() calls
		 * devclass_delete_device(), which frees the nameunit string.
		 * Reading it afterwards to name the loser would be a
		 * use-after-free.
		 */
		strlcpy(who, device_get_nameunit(kids[i]), sizeof(who));

		if (device_detach(kids[i]) != 0) {
			device_printf(kids[i], "will not release the firmware "
			    "clocks node; clocks stay unavailable\n");
			continue;
		}
		if (bootverbose)
			printf("rpi_fw_clocks: reclaimed the firmware clocks "
			    "node from %s\n", who);
	}

	free(kids, M_TEMP);
}

static int
rpi_fw_clocks_probe(device_t dev)
{

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (!ofw_bus_is_compatible(dev, "raspberrypi,firmware-clocks"))
		return (ENXIO);

	device_set_desc(dev, "Raspberry Pi firmware clocks");

	/*
	 * SPECIFIC, to outrank FreeBSD's generic ofw_clkbus, which otherwise
	 * claims this node and provides nothing from it.
	 */
	return (BUS_PROBE_SPECIFIC);
}

static int
rpi_fw_clocks_attach(device_t dev)
{
	struct rpi_fw_clocks_softc *sc = device_get_softc(dev);
	struct rpi_fw_clknode_sc *nsc;
	struct clknode_init_def def;
	struct clknode *clk;
	phandle_t node, parent;
	uint32_t id, rate;
	int nregistered;

	sc->dev = dev;
	node = ofw_bus_get_node(dev);

	/*
	 * The mailbox is this node's PARENT -- /soc/firmware, compatible
	 * "raspberrypi,bcm2835-firmware", which registers itself with
	 * OF_device_register_xref() when it attaches. If it has not attached
	 * yet there is nothing to talk to, and deferring is correct rather
	 * than failing outright.
	 */
	parent = OF_parent(node);
	sc->fwdev = OF_device_from_xref(OF_xref_from_node(parent));
	if (sc->fwdev == NULL) {
		device_printf(dev, "firmware mailbox not ready\n");
		return (ENXIO);
	}

	sc->clkdom = clkdom_create(dev);
	if (sc->clkdom == NULL)
		return (ENOMEM);

	nregistered = 0;
	for (id = 1; id < RPI_FW_CLK_MAX; id++) {
		if (rpi_fw_clk_names[id] == NULL)
			continue;

		/*
		 * Only register clocks the firmware admits to having. A rate
		 * of zero from GET_CLOCK_RATE means the id is not present on
		 * this board, and registering it anyway would hand consumers a
		 * clock that silently does nothing -- which is the failure
		 * being fixed here, not one to reproduce.
		 */
		if (rpi_fw_clk_get(sc->fwdev, RPI_FW_TAG_GET_MAX_CLOCK_RATE,
		    id, &rate) != 0 || rate == 0)
			continue;

		memset(&def, 0, sizeof(def));
		def.id = id;
		def.name = rpi_fw_clk_names[id];
		def.parent_names = NULL;
		def.parent_cnt = 0;

		clk = clknode_create(sc->clkdom, &rpi_fw_clknode_class, &def);
		if (clk == NULL) {
			device_printf(dev, "could not create clock %s\n",
			    def.name);
			continue;
		}
		nsc = clknode_get_softc(clk);
		nsc->fwdev = sc->fwdev;
		nsc->id = id;
		clknode_register(sc->clkdom, clk);
		nregistered++;

		if (bootverbose)
			device_printf(dev, "%s (id %u) max %u Hz\n",
			    def.name, id, rate);
	}

	if (clkdom_finit(sc->clkdom) != 0) {
		device_printf(dev, "could not finalise the clock domain\n");
		return (ENXIO);
	}

	device_printf(dev, "%d firmware clocks (#51)\n", nregistered);
	return (0);
}

static int
rpi_fw_clocks_detach(device_t dev)
{

	/* Consumers hold clk_t references into this domain. */
	return (EBUSY);
}

static device_method_t rpi_fw_clocks_methods[] = {
	DEVMETHOD(device_identify,	rpi_fw_clocks_identify),
	DEVMETHOD(device_probe,		rpi_fw_clocks_probe),
	DEVMETHOD(device_attach,	rpi_fw_clocks_attach),
	DEVMETHOD(device_detach,	rpi_fw_clocks_detach),
	DEVMETHOD_END
};

static driver_t rpi_fw_clocks_driver = {
	"rpi_fw_clocks",
	rpi_fw_clocks_methods,
	sizeof(struct rpi_fw_clocks_softc),
};

/*
 * bcm2835_firmware, NOT simplebus.
 *
 * /soc/firmware/clocks is a child of the firmware node, and the firmware
 * driver enumerates its own children, so the clocks node appears on the
 * bcm2835_firmware bus:
 *
 *	bcm2835_firmware0: <BCM2835 Firmware> on simplebus0
 *	ofw_clkbus1: <OFW clocks bus> on bcm2835_firmware0
 *
 * Registering on simplebus meant this driver never saw the device at all --
 * it loaded cleanly and attached to nothing.
 *
 * That second line is the other half of the problem: FreeBSD's generic
 * ofw_clkbus claims the node at boot. It enumerates CHILD clock nodes, and
 * this node has none -- it is a provider with #clock-cells = 1 -- so it owns
 * the node and supplies nothing. BUS_PROBE_SPECIFIC below outranks it, but
 * newbus does not re-probe a device that is already attached, so this driver
 * used to have to be handed the node by hand:
 *
 *	devctl detach ofw_clkbus1
 *	kextload .../RpiFirmwareClocks.kext
 *
 * rpi_fw_clocks_identify() now does that detach itself (#65), which is what
 * lets this load unattended. Compiling it into the kernel would settle the
 * contest on priority alone and make even that unnecessary -- still the
 * argument for this eventually living in nextbsd-kernel rather than here, but
 * no longer something the display depends on.
 */
EARLY_DRIVER_MODULE(rpi_fw_clocks, bcm2835_firmware, rpi_fw_clocks_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
EARLY_DRIVER_MODULE(rpi_fw_clocks_sb, simplebus, rpi_fw_clocks_driver, 0, 0,
    BUS_PASS_BUS + BUS_PASS_ORDER_MIDDLE);
MODULE_VERSION(rpi_fw_clocks, 1);
