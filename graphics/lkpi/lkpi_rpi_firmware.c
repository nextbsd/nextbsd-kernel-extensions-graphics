/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The rpi_firmware entry points vc4_hvs and vc4_hdmi need that
 * vc4_fkms_freebsd.c does not provide (nextbsd-kernel-extensions-graphics#51).
 *
 * That file implements the property interface -- rpi_firmware_property(),
 * rpi_firmware_property_list(), devm_rpi_firmware_get() -- which is everything
 * firmware KMS uses. The full KMS pipeline needs three more, declared in
 * <soc/bcm2835/raspberrypi-firmware.h> and implemented nowhere:
 *
 *	rpi_firmware_find_node()          vc4_hvs.c:2110
 *	rpi_firmware_get()                vc4_hvs.c:2113
 *	rpi_firmware_clk_get_max_rate()   vc4_hvs, for the 4kp60 decision
 *
 * They were undefined rather than wrong, so vc4_kms.ko linked and then failed
 * the resolution gate with ENOEXEC -- the module would not have loaded.
 */

#include <sys/param.h>
#include <sys/systm.h>

#include <linux/device.h>
#include <linux/of.h>
#include <linux/err.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

/*
 * The firmware node, found by compatible rather than by phandle.
 *
 * firmware KMS gets its handle from its own node's "brcm,firmware" phandle
 * (vc4_firmware_kms.c:1953), but vc4_hvs has no such property and asks for the
 * node globally, which is what upstream's find_node() does.
 *
 * Returns a device_node the caller of_node_put()s. Ours are not reference
 * counted -- of_node_put() is a no-op here -- so the lifetime is the module's.
 */
struct device_node *
rpi_firmware_find_node(void)
{

	return (of_find_compatible_node(NULL, NULL,
	    "raspberrypi,bcm2835-firmware"));
}

/*
 * A firmware handle for a node.
 *
 * devm_rpi_firmware_get() in vc4_fkms_freebsd.c does the real work; it takes a
 * struct device only to hang a devres cleanup off, and the handle itself does
 * not depend on it. Passing NULL means no automatic release, which is correct
 * for a caller that got here through find_node() rather than through its own
 * probe.
 */
struct rpi_firmware *
rpi_firmware_get(struct device_node *firmware_node)
{

	return (devm_rpi_firmware_get(NULL, firmware_node));
}

/*
 * Maximum rate of a firmware-managed clock, in Hz.
 *
 * vc4_hvs uses it for the core clock to decide whether this board can drive
 * 4kp60 -- the decision the stale upstream comment about HDMI binding after
 * HVS refers to (see the note in vc4_master_newbus.c).
 *
 * GET_MAX_CLOCK_RATE takes the clock id and returns the rate in the same
 * buffer. On failure it returns 0, which vc4_hvs treats as "unknown" and
 * handles: it decides against 4kp60 rather than misconfiguring the display.
 */
unsigned int
rpi_firmware_clk_get_max_rate(struct rpi_firmware *fw, unsigned int id)
{
	struct {
		uint32_t id;
		uint32_t rate;
	} msg = { .id = id, .rate = 0 };
	int ret;

	if (fw == NULL)
		return (0);
	ret = rpi_firmware_property(fw, RPI_FIRMWARE_GET_MAX_CLOCK_RATE,
	    &msg, sizeof(msg));
	if (ret != 0)
		return (0);
	return (msg.rate);
}
