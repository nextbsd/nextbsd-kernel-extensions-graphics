/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * newbus master for the full vc4 KMS pipeline
 * (nextbsd-kernel-extensions-graphics#51).
 *
 * Attaches to the top-level vc4 device-tree node, builds the struct device the
 * vendored master expects, and hands off to vc4_platform_drm_probe(), which
 * registers vc4_drm_ops as a component master. When every component has
 * registered -- HVS, CRTC, HDMI, each via its own newbus shim -- the master's
 * bind runs and brings up the drm_device.
 *
 * This is the same arrangement vc4_fkms_master.c uses for firmware KMS, and
 * the reason neither needs LinuxKPI's platform bus.
 *
 * BINDING ORDER
 *
 * The vendored source annotates component_drivers[] with:
 *
 *	The HDMI driver needs to be bound after the HVS so that we can lookup
 *	the HVS maximum core clock rate and figure out if we support 4kp60 or
 *	not.
 *
 * That comment is STALE on rpi-6.12.y and should not be used to reason about
 * this: the 4kp60 decision moved into vc4_hvs_bind(), which asks the firmware
 * mailbox directly, and every remaining vc4->hvs use in vc4_hdmi.c is a
 * runtime callback that cannot run before drm_dev_register().
 *
 * The constraint that IS real is encoder-before-CRTC. vc4_crtc_bind() calls
 * vc4_set_crtc_possible_masks(), which walks the encoders ALREADY REGISTERED
 * on the drm_device and sets encoder->possible_crtcs. Nothing revisits it. If
 * a pixelvalve binds before the HDMI encoders exist, those encoders keep
 * possible_crtcs == 0, DRM has no CRTC able to drive the HDMI connectors,
 * fbdev finds no usable CRTC, and every modeset fails while the driver reports
 * a successful load.
 *
 * bcm2712's device tree is in exactly that order -- pixelvalves at 0x7c410000
 * and 0x7c411000 come before the HDMI controllers at 0x7ef00700 and
 * 0x7ef05700 -- so binding in registration order would hit this on every boot.
 *
 * It is handled in the KPI rather than here: component_bind_all() binds in
 * the master's match-list order, the way upstream's
 * drivers/base/component.c does (nextbsd-kernel#199). This master therefore
 * only has to build its match list in component_drivers[] order, which
 * vc4_match_add_drivers() already does.
  */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/rman.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/component.h>

#include "vc4_newbus.h"

/*
 * linuxkpi's moduleparam.h expands a driver's tunables under a sysctl node it
 * does not itself declare, so vc4_hdmi.c's module_param(force_hotplug, ...)
 * refers to sysctl___hw_vc4_kms with nothing defining it. Declared here, once
 * -- bochs recorded that having it in two translation units is a duplicate
 * symbol at link time rather than a warning.
 */
SYSCTL_NODE(_hw, OID_AUTO, vc4_kms, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "VideoCore VI KMS");

/* Non-static in the vendored vc4_drv.c. */
extern struct platform_driver vc4_platform_driver;

static int
vc4_master_newbus_probe(device_t dev)
{

	return (vc4_newbus_probe(dev, &vc4_platform_driver,
	    "Broadcom VideoCore VI (KMS)"));
}

static int
vc4_master_newbus_attach(device_t dev)
{
	int error;

	/*
	 * Same struct device construction as every component, then the
	 * vendored probe -- which calls component_master_add_with_match()
	 * rather than binding anything itself.
	 *
	 * A master whose components have not all registered yet is NOT an
	 * error: component_master_add_with_match() returns 0 and the bind
	 * happens later, when the last component calls component_add(). That
	 * is why attach succeeding here does not mean the display is up.
	 */
	error = vc4_newbus_attach(dev, &vc4_platform_driver, "vc4");
	if (error != 0)
		return (error);

	if (bootverbose)
		device_printf(dev, "vc4: master registered; waiting on "
		    "components\n");
	return (0);
}

static int
vc4_master_newbus_detach(device_t dev)
{

	return (vc4_newbus_detach(dev, &vc4_platform_driver));
}

static device_method_t vc4_master_newbus_methods[] = {
	DEVMETHOD(device_probe,		vc4_master_newbus_probe),
	DEVMETHOD(device_attach,	vc4_master_newbus_attach),
	DEVMETHOD(device_detach,	vc4_master_newbus_detach),
	DEVMETHOD_END
};

static driver_t vc4_master_newbus_driver = {
	"vc4",
	vc4_master_newbus_methods,
	sizeof(struct vc4_newbus_softc),
};

/*
 * The master attaches LAST, in the default pass, after every component has
 * attached in BUS_PASS_SUPPORTDEV.
 *
 * This ordering is required, and the comment that used to sit here had it
 * exactly backwards. It claimed "an incomplete match list simply waits" and
 * put the master in an EARLIER pass than its components. It does not wait: the
 * master builds the match list at probe from the platform-device registry, and
 * a list built before the components attached is empty. An empty match list is
 * COMPLETE, so the master binds immediately with zero components, registers a
 * drm_device with no CRTCs and reports success -- a dark screen with nothing
 * in the log.
 *
 * Newbus runs passes in order and completes each before starting the next, so
 * putting the components in SUPPORTDEV and the master in the default pass is
 * what actually guarantees the list is populated.
 */
DRIVER_MODULE(vc4, simplebus, vc4_master_newbus_driver, 0, 0);
DRIVER_MODULE(vc4_ofwbus, ofwbus, vc4_master_newbus_driver, 0, 0);
MODULE_VERSION(vc4_kms, 1);

/*
 * Module dependencies. Without these the module declares none at all, and
 * kextload fails before anything is linked:
 *
 *	kldload(.../VideoCore6KMS): No such file or directory
 *	kextload: load failed (OSReturn 0xdc008016)
 *
 * Measured on a Pi 500+ -- the ENOENT is the kernel loader refusing the
 * module, not a missing file, and it happens even with every dependency
 * already resident. The accompanying "no kmod_info symbol or bad Mach-O
 * layout" is a red herring: that validator looks for Mach-O structure in an
 * ELF module and says the same of firmware KMS, which loads.
 *
 * The set matches vc4_fkms, which is the proven one on this hardware:
 *
 *   drmn                the DRM core
 *   drm_dma_helpers     drm_gem_dma_*, which vc4 allocates every buffer through
 *   drm_extra_helpers   drm_gem_fb_create() and the rest drm-kmod does not ship
 *   linuxkpi            everything the vendored sources are written against
 *
 * MODULE_DEPEND is DEPTH-1: the kernel linker searches only a module's own
 * declared dependencies, so having a module loaded is not the same as being
 * able to link against it. vc4_fkms_master.c records measuring exactly that --
 * "link_elf: symbol drm_gem_fb_create undefined" with IOGraphicsExtras already
 * loaded and exporting it.
 */
MODULE_DEPEND(vc4_kms, drmn, 2, 2, 2);
MODULE_DEPEND(vc4_kms, drm_dma_helpers, 1, 1, 1);
MODULE_DEPEND(vc4_kms, drm_extra_helpers, 1, 1, 1);
MODULE_DEPEND(vc4_kms, linuxkpi, 1, 1, 1);
/*
 * dmabuf, which vc4_fkms does NOT declare and does not need.
 *
 * vc4_crtc.c's async page flip calls dma_resv_get_singleton() on the
 * non-GEN_4 path, to get one fence to wait on before flipping. DMABuf.kext
 * exports it -- verified, the installed binary is byte-identical to the built
 * one -- but having it loaded is not enough, because MODULE_DEPEND is depth-1
 * and the linker searches only declared edges:
 *
 *	link_elf: symbol dma_resv_get_singleton undefined
 *
 * measured on a Pi 500+ with DMABuf resident and exporting the symbol. Same
 * trap the drm_extra_helpers comment above describes, one module further out.
 */
MODULE_DEPEND(vc4_kms, dmabuf, 1, 1, 1);
