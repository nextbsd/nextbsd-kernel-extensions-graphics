/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * newbus shim for the v3d GPU driver (nextbsd-kernel-extensions-graphics#66).
 *
 * v3d is vendored unmodified from raspberrypi/linux rpi-6.12.y, where it is a
 * Linux platform driver. LinuxKPI has no platform bus for it to bind to, so
 * this attaches to the device-tree node and calls the vendored probe -- the
 * same arrangement vc4_newbus.c uses for the display blocks.
 *
 * SIMPLER THAN THE DISPLAY SIDE, in one way that matters: vc4 is a component
 * driver, so each block registers with component_add() and nothing comes up
 * until a master's match list is complete. v3d is a single device that owns
 * one drm_device. Its probe registers the render node directly, so when this
 * returns the GPU is up or it is not -- there is no deferred bind to reason
 * about, and no BUS_PASS ordering against a master.
 *
 * The glue is duplicated from vc4_newbus.c rather than shared. That is
 * deliberate for now: vc4_kms is working and shipping, and #67 had to untangle
 * a `.PATH` that reached into another module's directory. Hoisting this into
 * graphics/lkpi/ is the right refactor once v3d works, not while it is being
 * brought up.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>
#include <sys/sysctl.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/dma-mapping.h>

/*
 * Defined here, exactly once: linuxkpi's module_param() in v3d_drv.c refers to
 * sysctl___hw_v3d and declares it nowhere. v3d_compat.h carries the matching
 * SYSCTL_DECL for every other file. Two definitions would be a duplicate
 * symbol at link time.
 */
SYSCTL_NODE(_hw, OID_AUTO, v3d, CTLFLAG_RW | CTLFLAG_MPSAFE, 0,
    "Broadcom V3D GPU");

/* Non-static in the vendored v3d_drv.c -- see the note there. */
extern struct platform_driver v3d_platform_driver;

/*
 * Lives in v3d_state.c, which includes v3d_drv.h. This file cannot: it handles
 * newbus resources and so sees FreeBSD's struct resource, while v3d_drv.h
 * pulls in <linux/ioport.h> and a different struct of the same name.
 */
int v3d_sysctl_state(SYSCTL_HANDLER_ARGS);
int v3d_sysctl_kick(SYSCTL_HANDLER_ARGS);
void v3d_watchdog_start(struct device *dev);
void v3d_watchdog_stop(void);

struct v3d_newbus_softc {
	device_t		bsddev;
	struct platform_device	pdev;
	struct device_node	node;
};

static int
v3d_newbus_probe(device_t dev)
{
	const struct of_device_id *id;
	struct platform_driver *drv = &v3d_platform_driver;

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (drv->driver.of_match_table == NULL)
		return (ENXIO);

	for (id = drv->driver.of_match_table; id->compatible[0] != '\0'; id++) {
		if (!ofw_bus_is_compatible(dev, id->compatible))
			continue;
		device_set_desc(dev, "Broadcom V3D GPU");
		return (BUS_PROBE_DEFAULT);
	}
	return (ENXIO);
}

static int
v3d_newbus_attach(device_t dev)
{
	struct v3d_newbus_softc *sc = device_get_softc(dev);
	struct platform_driver *drv = &v3d_platform_driver;
	struct resource *irq_res;
	int irq_rid, error;

	sc->bsddev = dev;
	sc->node.node = (intptr_t)ofw_bus_get_node(dev);

	sc->pdev.name = "v3d";
	sc->pdev.dev.bsddev = dev;
	lkpi_set_of_node(&sc->pdev.dev, &sc->node, drv->driver.of_match_table);
	lkpi_platform_device_register(&sc->pdev, &drv->driver);

	sc->pdev.dev.parent = NULL;
	INIT_LIST_HEAD(&sc->pdev.dev.devres_head);
	spin_lock_init(&sc->pdev.dev.devres_lock);
	INIT_LIST_HEAD(&sc->pdev.dev.irqents);
	dev_set_name(&sc->pdev.dev, "v3d.%d", device_get_unit(dev));
	dev_set_drvdata(&sc->pdev.dev, NULL);

	/*
	 * A 36-bit mask to start with. v3d_platform_drm_probe() narrows it
	 * itself from V3D_MMU_DEBUG_INFO -- the MMU reports its own physical
	 * address width and the driver calls dma_set_mask_and_coherent() with
	 * whatever it read. This only has to be wide enough not to fail before
	 * that happens, and non-NULL so linux_dma_priv is initialised at all:
	 * with dma_priv NULL every allocator in linux_pci.c fails its NULL
	 * check and the MMU scratch page allocation returns ENOMEM
	 * (nextbsd-kernel#176, measured on the display path).
	 */
	error = linux_dma_priv_init(&sc->pdev.dev, DMA_BIT_MASK(36),
	    DMA_BIT_MASK(36));
	if (error != 0) {
		device_printf(dev, "dma_priv init failed: %d\n", error);
		return (error);
	}

	/*
	 * v3d has TWO interrupts -- hub and core -- where every vc4 block has
	 * at most one. Only rid 0 is read here because that is all
	 * struct device carries; v3d_irq_init() asks for both by index through
	 * platform_get_irq(), which reads the node directly and does not go
	 * through this field.
	 */
	irq_rid = 0;
	irq_res = bus_alloc_resource_any(dev, SYS_RES_IRQ, &irq_rid,
	    RF_ACTIVE | RF_SHAREABLE);
	if (irq_res != NULL) {
		sc->pdev.dev.irq = rman_get_start(irq_res);
		bus_release_resource(dev, SYS_RES_IRQ, irq_rid, irq_res);
	} else {
		sc->pdev.dev.irq = LINUX_IRQ_INVALID;
	}

	if (bootverbose)
		device_printf(dev, "v3d: node %#lx irq %u\n",
		    (long)sc->node.node, sc->pdev.dev.irq);

	if (drv->probe == NULL)
		return (ENXIO);

	error = drv->probe(&sc->pdev);
	if (error != 0) {
		device_printf(dev, "v3d probe failed: %d\n", error);
		return (ENXIO);
	}

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "state",
	    CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_MPSAFE, &sc->pdev.dev, 0,
	    v3d_sysctl_state, "A",
	    "V3D scheduler queues and GPU registers");

	SYSCTL_ADD_PROC(device_get_sysctl_ctx(dev),
	    SYSCTL_CHILDREN(device_get_sysctl_tree(dev)), OID_AUTO, "kick",
	    CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_MPSAFE, &sc->pdev.dev, 0,
	    v3d_sysctl_kick, "I",
	    "write 1 to re-kick the scheduler submit taskqueues");

	/*
	 * Recovery for the lost taskqueue wakeup (nextbsd#450): drm_sched's
	 * submit work can be left enqueued with ta_pending>0 while its
	 * taskqueue thread sleeps, which starves the GPU permanently. See the
	 * long comment in v3d_state.c.
	 */
	v3d_watchdog_start(&sc->pdev.dev);

	return (0);
}

static int
v3d_newbus_detach(device_t dev)
{
	struct v3d_newbus_softc *sc = device_get_softc(dev);
	struct platform_driver *drv = &v3d_platform_driver;

	v3d_watchdog_stop();

	if (drv->remove_new != NULL)
		drv->remove_new(&sc->pdev);
	else if (drv->remove != NULL)
		drv->remove(&sc->pdev);

	lkpi_platform_device_unregister(&sc->pdev);
	lkpi_clear_of_node(&sc->pdev.dev);
	return (0);
}

static device_method_t v3d_newbus_methods[] = {
	DEVMETHOD(device_probe,		v3d_newbus_probe),
	DEVMETHOD(device_attach,	v3d_newbus_attach),
	DEVMETHOD(device_detach,	v3d_newbus_detach),
	DEVMETHOD_END
};

static driver_t v3d_newbus_driver = {
	"v3d",
	v3d_newbus_methods,
	sizeof(struct v3d_newbus_softc),
};

/*
 * BUS_PASS_SUPPORTDEV, like the vc4 blocks: the clock provider and the reset
 * provider both have to be attached before this probe runs, since v3d resolves
 * both during probe and a missing reset makes it fall back to a "bridge"
 * register bank a 2712 device tree does not have.
 */
EARLY_DRIVER_MODULE(v3d, simplebus, v3d_newbus_driver, 0, 0,
    BUS_PASS_SUPPORTDEV);
EARLY_DRIVER_MODULE(v3d_ofwbus, ofwbus, v3d_newbus_driver, 0, 0,
    BUS_PASS_SUPPORTDEV);

/*
 * MODULE_DEPEND is DEPTH-1 (#66). The kernel linker searches only a module's
 * OWN declared dependencies, so a module being loaded and exporting a symbol is
 * not the same as being able to link against it. This module shipped without a
 * single declaration and failed exactly as vc4 already recorded twice:
 *
 *	link_elf: symbol drm_read undefined
 *
 * with IOGraphics resident and exporting drm_read as a GLOBAL. Same trap as
 * "symbol drm_gem_fb_create undefined" and "symbol dma_resv_get_singleton
 * undefined" in vc4_master_newbus.c; third time on this hardware.
 *
 * The set below is not copied from another driver, it is measured: every
 * undefined symbol in v3d.ko resolved against the shipped kexts.
 *
 *	drmn                45 symbols, incl. the whole drm_sched_* scheduler
 *	                    that this port was scoped around as missing
 *	drm_shmem_helpers   12, drm_gem_shmem_create and friends -- v3d's BOs
 *	                    are shmem, not TTM and not the DMA helper
 *	dmabuf              11, the dma_fence_* the scheduler signals through
 *
 * drm_extra_helpers is deliberately ABSENT: it satisfies zero of v3d's
 * symbols. virtio_gpu declares it and v3d does not need it -- it is a KMS
 * framebuffer helper, and v3d is render-only, with no framebuffer of its own.
 */
MODULE_DEPEND(v3d, drmn, 2, 2, 2);
MODULE_DEPEND(v3d, drm_shmem_helpers, 1, 1, 1);
MODULE_DEPEND(v3d, dmabuf, 1, 1, 1);
MODULE_DEPEND(v3d, linuxkpi, 1, 1, 1);
MODULE_VERSION(v3d, 1);
