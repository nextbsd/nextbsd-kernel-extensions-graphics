/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Shared newbus glue for the vc4 component drivers -- see vc4_newbus.h for
 * why this exists (nextbsd-kernel-extensions-graphics#51).
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/rman.h>

#include <dev/ofw/openfirm.h>
#include <dev/ofw/ofw_bus.h>
#include <dev/ofw/ofw_bus_subr.h>

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>
#include <linux/component.h>
#include <linux/dma-mapping.h>

#include "vc4_newbus.h"

int
vc4_newbus_probe(device_t dev, struct platform_driver *drv, const char *desc)
{
	const struct of_device_id *id;

	if (!ofw_bus_status_okay(dev))
		return (ENXIO);
	if (drv == NULL || drv->driver.of_match_table == NULL)
		return (ENXIO);

	for (id = drv->driver.of_match_table; id->compatible[0] != '\0'; id++) {
		if (!ofw_bus_is_compatible(dev, id->compatible))
			continue;
		device_set_desc(dev, desc);
		return (BUS_PROBE_DEFAULT);
	}
	return (ENXIO);
}

int
vc4_newbus_attach(device_t dev, struct platform_driver *drv, const char *name)
{
	struct vc4_newbus_softc *sc;
	struct resource *irq_res;
	int irq_rid, error;

	sc = device_get_softc(dev);
	sc->bsddev = dev;
	sc->node.node = (intptr_t)ofw_bus_get_node(dev);

	/*
	 * dev.driver is NOT set: struct platform_driver::driver is a
	 * module-private type now (it carries of_match_table, which the
	 * kernel's struct device_driver no longer does), so it is not a
	 * struct device_driver *. Nothing reads dev->driver --
	 * of_device_get_match_data() takes the table from the registry below.
	 */
	sc->pdev.name = name;
	sc->pdev.dev.bsddev = dev;
	/*
	 * See vc4_fkms_master.c: struct device no longer carries of_node, so
	 * the device -> node mapping is module-private and dev_of_node() reads
	 * it back. The driver's match table is registered with it, standing in
	 * for the of_match_table struct device_driver no longer has.
	 */
	lkpi_set_of_node(&sc->pdev.dev, &sc->node,
	    drv->driver.of_match_table);

	/*
	 * Register with the platform-device registry, which is what
	 * platform_find_device_by_driver() enumerates when the master builds
	 * its component match list. Without this the list comes back empty --
	 * and an empty match list is complete, so the master would bind with
	 * zero components and report success (#51).
	 */
	lkpi_platform_device_register(&sc->pdev, &drv->driver);
	sc->pdev.dev.parent = NULL;
	INIT_LIST_HEAD(&sc->pdev.dev.devres_head);
	spin_lock_init(&sc->pdev.dev.devres_lock);
	INIT_LIST_HEAD(&sc->pdev.dev.irqents);
	/*
	 * Unique per instance. A 2712 has two pixelvalves and two HDMI
	 * controllers, and Linux gives each its own name; naming both instances
	 * "vc4_hdmi" makes IRQ labels and every drm message that prints
	 * dev_name() ambiguous between them.
	 */
	dev_set_name(&sc->pdev.dev, "%s.%d", name, device_get_unit(dev));
	dev_set_drvdata(&sc->pdev.dev, NULL);

	/*
	 * Without dma_priv every allocator in linux_pci.c fails its
	 * dma_priv == NULL check, and DRM_IOCTL_MODE_CREATE_DUMB returns ENOMEM
	 * for a 1.2MB buffer on a machine with 16GB free. That was measured on
	 * firmware KMS (nextbsd-kernel#176) and is why X could not get past
	 * ScreenInit.
	 *
	 * It is worse here than it was there, because vc4_drm_bind() calls
	 * dma_set_mask_and_coherent() and IGNORES the return -- and
	 * dma_set_mask() returns -EIO when dma_priv is NULL. The bind would
	 * report success and every GEM allocation afterwards would fail.
	 *
	 * 32-bit masks to start, matching vc4_drm_bind()'s initial
	 * coherent_dma_mask; that function then widens both to 36 bits on
	 * gen6 (2712), which re-inits the tag through the same path.
	 */
	error = linux_dma_priv_init(&sc->pdev.dev, DMA_BIT_MASK(32),
	    DMA_BIT_MASK(32));
	if (error != 0) {
		device_printf(dev, "%s: dma_priv init failed: %d\n", name, error);
		return (error);
	}

	/*
	 * Read the IRQ number, then release the resource: the vendored driver
	 * allocates it itself through devm_request_irq(), and holding it here
	 * makes that allocation fail. Blocks with no interrupt (some
	 * pixelvalves) simply get LINUX_IRQ_INVALID, which is not an error.
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
		device_printf(dev, "%s: node %#lx irq %u\n", name,
		    (long)sc->node.node, sc->pdev.dev.irq);

	if (drv->probe == NULL)
		return (ENXIO);

	/*
	 * The vendored probe calls component_add(). The block does NOT come up
	 * here -- it binds when a master's match list is complete, which is the
	 * whole point of the component framework and why probe order between
	 * these drivers does not matter.
	 */
	error = drv->probe(&sc->pdev);
	if (error != 0) {
		device_printf(dev, "%s probe failed: %d\n", name, error);
		return (ENXIO);
	}
	return (0);
}

int
vc4_newbus_detach(device_t dev, struct platform_driver *drv)
{
	struct vc4_newbus_softc *sc = device_get_softc(dev);

	/*
	 * 6.12-era drivers use .remove_new; .remove is checked too so this glue
	 * works for a driver written against either spelling.
	 */
	if (drv->remove_new != NULL)
		drv->remove_new(&sc->pdev);
	else if (drv->remove != NULL)
		drv->remove(&sc->pdev);

	/*
	 * Out of the registry only after the driver's remove has run, so a
	 * master unbinding concurrently still finds a live device rather than
	 * a half-torn-down one.
	 */
	lkpi_platform_device_unregister(&sc->pdev);
	lkpi_clear_of_node(&sc->pdev.dev);
	return (0);
}
