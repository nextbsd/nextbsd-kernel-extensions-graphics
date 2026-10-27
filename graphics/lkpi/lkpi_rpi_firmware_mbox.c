/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The Raspberry Pi firmware mailbox, FreeBSD side (nextbsd-kernel-extensions-graphics#51).
 *
 * rpi_firmware_property(), rpi_firmware_property_list() and
 * devm_rpi_firmware_get(), implemented over bcm2835_firmware(4). Everything
 * <soc/bcm2835/raspberrypi-firmware.h> declares and Linux implements in a
 * driver we do not have.
 *
 * This was vc4_fkms/vc4_fkms_freebsd.c and moved here when firmware KMS was
 * retired (#64). It never belonged to that driver: it is LinuxKPI glue, and
 * vc4_kms depends on it for EDID -- the panel on a Pi 500+ has no DDC adapter,
 * so GET_EDID_BLOCK_DISPLAY over this mailbox is the only source of an EDID.
 * It is also how the firmware clocks are read and set.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/malloc.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>

#include <arm/broadcom/bcm2835/bcm2835_firmware.h>
#include <arm/broadcom/bcm2835/bcm2835_mbox_prop.h>

#include <linux/device.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/slab.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

/*
 * Linux keeps this opaque and hands the pointer back to every call. All we
 * need behind it is the FreeBSD device that owns the mailbox.
 */
struct rpi_firmware {
	device_t	dev;
};

/*
 * Resolve the "brcm,firmware" phandle to the bcm2835_firmware(4) instance.
 *
 * That driver calls OF_device_register_xref() during attach
 * (bcm2835_firmware.c:98), so the node-to-device lookup is available as soon
 * as it has probed. If it has not, returning NULL gives the caller the
 * -EPROBE_DEFER path it already has for exactly this case.
 *
 * devm_ in the Linux name means the allocation is freed when the device is;
 * this uses plain malloc because the lifetime here is the module's, and the
 * driver never frees it.
 */
struct rpi_firmware *
devm_rpi_firmware_get(struct device *dev, struct device_node *firmware_node)
{
	struct rpi_firmware *fw;
	device_t fwdev;

	if (firmware_node == NULL)
		return (NULL);

	fwdev = OF_device_from_xref(OF_xref_from_node(
	    (phandle_t)firmware_node->node));
	if (fwdev == NULL)
		return (NULL);

	fw = malloc(sizeof(*fw), M_DEVBUF, M_NOWAIT | M_ZERO);
	if (fw == NULL)
		return (NULL);
	fw->dev = fwdev;
	return (fw);
}

/*
 * One tag, request and reply in the same buffer. bcm2835_firmware_property()
 * builds the header, sends it and copies the reply back over *data, which is
 * precisely rpi_firmware_property()'s contract.
 *
 * Returns a negative errno, as Linux callers expect, rather than FreeBSD's
 * positive convention.
 */
int
rpi_firmware_property(struct rpi_firmware *fw, u32 tag, void *data, size_t len)
{
	int error;

	if (fw == NULL || fw->dev == NULL)
		return (-ENODEV);

	error = bcm2835_firmware_property(fw->dev, tag, data, len);
	return (error == 0 ? 0 : -error);
}

/*
 * A pre-built sequence of tags, which the caller has already laid out. Only
 * the enclosing message header is ours to add.
 *
 * The buffer sent is header + tags + the terminating zero word, and the reply
 * is copied back over the caller's tags so it can read the values it asked
 * for -- the same in-place convention as the single-tag call.
 */
int
rpi_firmware_property_list(struct rpi_firmware *fw, void *data, size_t tag_size)
{
	struct bcm2835_mbox_hdr *hdr;
	size_t msg_size;
	int error;

	if (fw == NULL || fw->dev == NULL)
		return (-ENODEV);
	/* The mailbox moves 32-bit words; a ragged tail is a caller bug. */
	if ((tag_size & (sizeof(uint32_t) - 1)) != 0)
		return (-EINVAL);

	msg_size = sizeof(*hdr) + tag_size + sizeof(uint32_t);
	hdr = malloc(msg_size, M_DEVBUF, M_NOWAIT | M_ZERO);
	if (hdr == NULL)
		return (-ENOMEM);

	hdr->buf_size = msg_size;
	hdr->code = BCM2835_MBOX_CODE_REQ;
	memcpy(hdr + 1, data, tag_size);
	/* End tag: the zeroed word after the copy already is one. */

	error = bcm2835_mbox_property(hdr, msg_size);
	if (error == 0)
		memcpy(data, hdr + 1, tag_size);

	free(hdr, M_DEVBUF);
	return (error == 0 ? 0 : -error);
}

/*
 * The one vc4 symbol vc4_firmware_kms.c needs from outside itself -- measured,
 * it is the only one of 55 vc4_* calls not defined in that file.
 *
 * It maps the SMI interrupt register, and only on pre-BCM2712 parts:
 *
 *	if (fkms->revision >= BCM2712) {
 *		devm_request_irq(..., vc4_crtc2712_irq_handler, ...);
 *	} else {
 *		crtc_list[0]->regs = vc4_ioremap_regs(pdev, 0);   <- here
 *
 * A Pi 5 always takes the first branch, so this exists to satisfy the linker
 * rather than to run. It returns an error pointer, which is what the call site
 * tests for -- though note the caller only logs and carries on, so an older Pi
 * reaching this would fault on the next line. That is honest: this port
 * targets BCM2712 and the earlier path has never been exercised.
 */
/*
 * Declared here rather than by including vc4_drv.h: that header pulls the
 * whole DRM stack in for one prototype, and this file is deliberately the
 * FreeBSD side of the module.
 */
/*
 * EXCLUDED from vc4_kms (#51). The full KMS module gets this from upstream's
 * own vc4_drv.c, and compiling this file into it as well gave
 *
 *	ld.lld: error: duplicate symbol: vc4_ioremap_regs
 *
 * The rpi_firmware property interface above is what vc4_kms needs from this
 * file, so the guard is around this one definition rather than the whole file.
 * Firmware KMS does not define LKPI_NO_VC4_IOREMAP and is unaffected.
 */
#ifndef LKPI_NO_VC4_IOREMAP
void __iomem *vc4_ioremap_regs(struct platform_device *pdev, int index);

void __iomem *
vc4_ioremap_regs(struct platform_device *pdev, int index)
{

	return (ERR_PTR(-ENODEV));
}
#endif /* LKPI_NO_VC4_IOREMAP */

void
rpi_firmware_put(struct rpi_firmware *fw)
{

	free(fw, M_DEVBUF);
}
