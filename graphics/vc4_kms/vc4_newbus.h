/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared newbus glue for the vc4 component drivers
 * (nextbsd-kernel-extensions-graphics#51).
 *
 * Each vc4 hardware block -- HVS, CRTC (pixelvalve), HDMI -- is vendored
 * unmodified from raspberrypi/linux and registers itself as a Linux platform
 * driver. LinuxKPI has no platform bus for those to bind to
 * (platform_driver_register() is a stub returning -ENXIO), so each block gets
 * a newbus driver that attaches to its device-tree node and calls the vendored
 * probe itself. That is the arrangement vc4_fkms_master.c already uses on
 * hardware.
 *
 * The three shims differ only in which platform_driver they wrap and what to
 * call the device, so the work lives here and each shim is its DEVMETHOD table
 * plus one line of each callback.
 */
#ifndef _VC4_NEWBUS_H_
#define _VC4_NEWBUS_H_

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>

/*
 * One per attached block. The platform_device is what the vendored probe
 * sees; the device_node backs dev.of_node so of_* helpers can reach the FDT.
 */
struct vc4_newbus_softc {
	device_t		bsddev;
	struct platform_device	pdev;
	struct device_node	node;
};

/*
 * Probe against the vendored driver's OWN of_match_table rather than a copy,
 * so the shim and the driver cannot disagree about which parts are supported.
 */
int	vc4_newbus_probe(device_t dev, struct platform_driver *drv,
	    const char *desc);

/*
 * Build the struct device the vendored probe expects, then call it.
 *
 * The field list is the one established the hard way in nextbsd-kernel#176:
 * devres_head and devres_lock because devm_* walks them, irqents because
 * request_irq() links into it, dev_set_name() because drm_dev_init() hands
 * dev_name(parent) to drmm_kstrdup() and a NULL faults there. Each missing
 * field was a panic at a different point in attach.
 */
int	vc4_newbus_attach(device_t dev, struct platform_driver *drv,
	    const char *name);

int	vc4_newbus_detach(device_t dev, struct platform_driver *drv);

#endif /* _VC4_NEWBUS_H_ */
