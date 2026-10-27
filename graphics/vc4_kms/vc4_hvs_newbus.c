/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * newbus shim for vc4_hvs_driver (nextbsd-kernel-extensions-graphics#51).
 *
 * The compositor: reads framebuffers from memory, composites and scales planes,
 * and feeds pixels to the display. This is the block firmware KMS asks the
 * firmware to program, and the one whose SET_PLANE the 2712 firmware refuses
 * (#48) -- programming it directly is what puts correct pixels on screen.
 *
 * All the work is in vc4_newbus.c; this is the DEVMETHOD table and the name.
 * vc4_hvs_driver is non-static in the vendored source, so its .probe and
 * .remove_new are reachable without editing that file.
 */

#include <sys/param.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>

#include <linux/platform_device.h>

#include "vc4_newbus.h"

extern struct platform_driver vc4_hvs_driver;

static int
vc4_hvs_newbus_probe(device_t dev)
{

	return (vc4_newbus_probe(dev, &vc4_hvs_driver, "BCM2712 Hardware Video Scaler"));
}

static int
vc4_hvs_newbus_attach(device_t dev)
{

	return (vc4_newbus_attach(dev, &vc4_hvs_driver, "vc4_hvs"));
}

static int
vc4_hvs_newbus_detach(device_t dev)
{

	return (vc4_newbus_detach(dev, &vc4_hvs_driver));
}

static device_method_t vc4_hvs_newbus_methods[] = {
	DEVMETHOD(device_probe,		vc4_hvs_newbus_probe),
	DEVMETHOD(device_attach,	vc4_hvs_newbus_attach),
	DEVMETHOD(device_detach,	vc4_hvs_newbus_detach),
	DEVMETHOD_END
};

static driver_t vc4_hvs_newbus_driver = {
	"vc4_hvs",
	vc4_hvs_newbus_methods,
	sizeof(struct vc4_newbus_softc),
};

/*
 * BUS_PASS_SUPPORTDEV, EARLIER than the master (#51).
 *
 * The master builds its component match list at probe by asking the
 * platform-device registry which devices are bound to each driver in
 * component_drivers[]. A component that has not attached yet is not in that
 * registry and does not make the list.
 *
 * That is not a "binds later" situation. An empty or short match list is
 * COMPLETE by definition, so the master binds immediately with whatever it
 * found -- possibly nothing -- registers a drm_device with no CRTCs, and
 * reports success. The screen stays dark and nothing logs an error.
 *
 * So every component attaches in an earlier newbus pass than the master.
 */
EARLY_DRIVER_MODULE(vc4_hvs, simplebus, vc4_hvs_newbus_driver, 0, 0,
    BUS_PASS_SUPPORTDEV);
EARLY_DRIVER_MODULE(vc4_hvs_ofwbus, ofwbus, vc4_hvs_newbus_driver, 0, 0,
    BUS_PASS_SUPPORTDEV);
