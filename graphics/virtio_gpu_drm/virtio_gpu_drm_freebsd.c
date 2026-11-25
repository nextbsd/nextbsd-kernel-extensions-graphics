/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026, Joseph Maloney
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * newbus attach glue for the vendored virtio-gpu DRM driver.
 *
 * Clean-room: written against FreeBSD's own virtio child drivers (if_vtnet.c
 * is the model for the method table and the config-change hook) rather than
 * derived from base's virtio_gpu(4), so nothing here inherits that driver's
 * licence or its console-ownership behaviour.
 *
 * Two things this file exists to bridge:
 *
 * 1. Binding. Linux registers a virtio driver at run time from
 *    module_virtio_driver(); FreeBSD declares it at compile time with
 *    DRIVER_MODULE() against virtio_pci and virtio_mmio. The vendored
 *    virtgpu_drv.c keeps its Linux shape and exports four wrappers for us.
 *
 * 2. The device hierarchy. drm_dev_alloc() wants the PARENT of the virtio
 *    device -- on Linux, the PCI function the virtio device hangs off. FreeBSD
 *    has that device_t but no linuxkpi `struct device` for it, because
 *    virtio_pci is a native driver and linuxkpi only builds pci_dev's for
 *    devices it attached itself. lkpinew_pci_dev() is the supported way to
 *    manufacture one, and it is what makes dev_is_pci()/to_pci_dev() answer
 *    correctly inside the vendored code -- which in turn is what lets
 *    virtio_gpu_pci_quirk() take the framebuffer away from efifb.
 *
 * Deliberately NOT here: `device virtio_gpu` in a kernel config, ever. Base's
 * module registers at VD_PRIORITY_GENERIC+10 against efifb's +1, so merely
 * compiling it in displaces the console driver that works. This module is
 * loadable-only and binds the same device by a different name.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>
#include <sys/kernel.h>
#include <sys/malloc.h>
#include <sys/module.h>
#include <sys/taskqueue.h>

#include <machine/bus.h>

#include <dev/virtio/virtio.h>
#include <dev/virtio/virtqueue.h>

#include <linux/device.h>
#include <linux/pci.h>
#include <linux/virtio.h>
#include <linux/virtio_ids.h>

#include "virtio_if.h"

/*
 * Base virtio_gpu(4)'s handoff (nextbsd-kernel patch 0112): keep the device
 * running across its detach, for this driver to adopt.
 */
int		 vtgpu_handoff(device_t dev);

/* Exported by the vendored virtgpu_drv.c under __FreeBSD__. */
int		 virtio_gpu_bsd_probe(struct virtio_device *vdev);
void		 virtio_gpu_bsd_remove(struct virtio_device *vdev);
void		 virtio_gpu_bsd_config_changed(struct virtio_device *vdev);
const unsigned int *virtio_gpu_bsd_features(unsigned int *count);

struct virtio_gpu_drm_softc {
	device_t		 vgd_dev;
	struct virtio_device	*vgd_vdev;
	struct pci_dev		*vgd_pdev;	/* manufactured, PCI transport */
	struct device		 vgd_parent;	/* used when not PCI (mmio) */
};

/*
 * VIRTIO_SIMPLE_PNPINFO would also emit the PNP metadata that devmatch uses to
 * autoload a module by device type. That is exactly what we want here -- the
 * kext is loaded on match rather than at boot -- and it is why the module is
 * named virtio_gpu_drm: base already owns the name `virtio_gpu`, and two
 * modules claiming one PNP identity is a coin flip over which one binds.
 */
VIRTIO_SIMPLE_PNPINFO(virtio_gpu_drm, VIRTIO_ID_GPU,
    "VirtIO GPU (DRM/KMS)");

/*
 * Is this virtio device on the PCI transport rather than virtio-mmio?
 *
 * Open-coded rather than calling is_pci_device(): that helper exists only in
 * -CURRENT, and this builds against releng/15.0, where it is absent. The
 * logic is the same one it uses -- ask whether the bus this device hangs off
 * is the pci devclass -- and it costs nothing to carry until the base
 * snapshot moves. Including <dev/pci/pcivar.h> to reach for it was actively
 * harmful: it perturbed the include order linuxkpi's own <linux/pci.h> needs
 * and left struct pci_devinfo incomplete inside that header.
 */
static bool
virtio_gpu_transport_is_pci(device_t transport)
{
	devclass_t dc;

	dc = device_get_devclass(device_get_parent(transport));

	return (dc != NULL && dc == devclass_find("pci"));
}

static int
virtio_gpu_drm_probe(device_t dev)
{
	int rv;

	rv = VIRTIO_SIMPLE_PROBE(dev, virtio_gpu_drm);

	/*
	 * Outrank base's virtio_gpu(4), which claims the same virtio device
	 * type (16) from vtgpu_probe() with the identical VIRTIO_SIMPLE_PROBE
	 * helper -- so at equal priority the tie goes to whichever driver
	 * newbus reaches first, and that is always the compiled-in one. On
	 * arm64 that is not academic: GENERIC includes virtio_gpu, so vtgpu
	 * takes 1af4:1050 during boot, this driver never gets a probe, and no
	 * DRM node is ever published. (amd64 GENERIC has no virtio_gpu, which
	 * is why the amd64 boot test binds and the breakage is arm64-only.)
	 *
	 * BUS_PROBE_VENDOR (-10) vs BUS_PROBE_DEFAULT (-20) is the mechanism
	 * newbus provides for exactly this: a more capable driver superseding
	 * the base one. Deliberately NOT done by removing virtio_gpu from the
	 * kernel config -- leaving it in place keeps a working console driver
	 * on any machine where this kext is absent, and it simply loses the
	 * probe wherever the kext is present.
	 *
	 * Requires this kext to be resident before the virtio bus probes its
	 * children, i.e. preloaded by the loader rather than kextd-loaded
	 * after root mount; a later load still gets no probe, because newbus
	 * does not re-probe an already-attached device.
	 */
	if (rv == BUS_PROBE_DEFAULT)
		rv = BUS_PROBE_VENDOR;

	return (rv);
}

static int
virtio_gpu_drm_attach(device_t dev)
{
	struct virtio_gpu_drm_softc *sc;
	const unsigned int *features;
	device_t parent;
	unsigned int nfeatures;
	int error;

	sc = device_get_softc(dev);
	sc->vgd_dev = dev;

	features = virtio_gpu_bsd_features(&nfeatures);

	/*
	 * Feature negotiation must precede virtqueue allocation on FreeBSD,
	 * and Linux drivers assume it already happened by the time probe()
	 * runs, so the shim does it as part of building the device.
	 */
	sc->vgd_vdev = lkpi_virtio_device_alloc(dev, features, nfeatures);
	if (sc->vgd_vdev == NULL)
		return (ENOMEM);

	/*
	 * The Linux-side view of this device and of its parent. The parent is
	 * what drm_dev_alloc() is handed, so it has to be the transport, not
	 * the virtio device itself.
	 */
	/*
	 * Note what is NOT called here: device_initialize(). linuxkpi's is not
	 * a plain field initialiser -- it dereferences dev->class->bsdclass and
	 * dev->class->kobj.name, and will call device_add_child() to
	 * manufacture a NEW newbus device when dev->parent is set. On a bare
	 * struct device with no class that is a null dereference, and the
	 * child it wants to create already exists: it is the device we are
	 * attaching to. Filling the two fields that get read is both necessary
	 * and sufficient.
	 */
	parent = device_get_parent(dev);
	if (virtio_gpu_transport_is_pci(parent)) {
		/*
		 * lkpinew_pci_dev() builds a complete struct pci_dev around an
		 * existing newbus PCI device -- which is exactly our situation,
		 * because virtio_pci is a native FreeBSD driver and linuxkpi
		 * only builds pci_dev's for devices it attached itself. This is
		 * what makes dev_is_pci() and to_pci_dev() answer correctly
		 * inside the vendored probe, and so what lets
		 * virtio_gpu_pci_quirk() take the aperture from efifb.
		 */
		sc->vgd_pdev = lkpinew_pci_dev(parent);
		if (sc->vgd_pdev == NULL) {
			lkpi_virtio_device_free(sc->vgd_vdev);
			sc->vgd_vdev = NULL;
			return (ENOMEM);
		}
		sc->vgd_vdev->dev.parent = &sc->vgd_pdev->dev;
	} else {
		/*
		 * virtio-mmio: no PCI function to point at. dev_is_pci() will
		 * report false and the vendored probe skips the framebuffer
		 * handover, which is correct -- there is no PCI aperture to
		 * take over from. The devres list is initialised because this
		 * struct device is handed to drm_dev_alloc() as the DRM
		 * device's parent, and anything reaching for devm_* on it would
		 * otherwise walk an empty list head.
		 */
		sc->vgd_parent.bsddev = parent;
		INIT_LIST_HEAD(&sc->vgd_parent.devres_head);
		spin_lock_init(&sc->vgd_parent.devres_lock);
		sc->vgd_vdev->dev.parent = &sc->vgd_parent;
	}

	sc->vgd_vdev->dev.bsddev = dev;

	error = virtio_gpu_bsd_probe(sc->vgd_vdev);
	if (error != 0) {
		device_printf(dev, "virtio-gpu probe failed: %d\n", error);
		if (sc->vgd_pdev != NULL)
			pci_dev_put(sc->vgd_pdev);
		lkpi_virtio_device_free(sc->vgd_vdev);
		sc->vgd_vdev = NULL;
		/* Linux errno on the way in, FreeBSD errno on the way out. */
		return (-error);
	}

	return (0);
}

static int
virtio_gpu_drm_detach(device_t dev)
{
	struct virtio_gpu_drm_softc *sc;

	sc = device_get_softc(dev);
	if (sc->vgd_vdev != NULL) {
		virtio_gpu_bsd_remove(sc->vgd_vdev);
		if (sc->vgd_pdev != NULL)
			pci_dev_put(sc->vgd_pdev);
		lkpi_virtio_device_free(sc->vgd_vdev);
		sc->vgd_vdev = NULL;
	}

	return (0);
}

/*
 * The host changed something in the config space -- for virtio-gpu that means
 * the display topology moved, which is how a window resize on the host becomes
 * a mode change in the guest. The vendored handler defers to a workqueue, so
 * this is safe to call from the bus.
 */
static int
virtio_gpu_drm_config_change(device_t dev)
{
	struct virtio_gpu_drm_softc *sc;

	sc = device_get_softc(dev);
	if (sc->vgd_vdev != NULL)
		virtio_gpu_bsd_config_changed(sc->vgd_vdev);

	return (0);
}

static device_method_t virtio_gpu_drm_methods[] = {
	DEVMETHOD(device_probe,		virtio_gpu_drm_probe),
	DEVMETHOD(device_attach,	virtio_gpu_drm_attach),
	DEVMETHOD(device_detach,	virtio_gpu_drm_detach),

	DEVMETHOD(virtio_config_change,	virtio_gpu_drm_config_change),

	DEVMETHOD_END
};

static driver_t virtio_gpu_drm_driver = {
	"virtio_gpu_drm",
	virtio_gpu_drm_methods,
	sizeof(struct virtio_gpu_drm_softc),
};

/*
 * Take the virtio-gpu device over from base virtio_gpu(4).
 *
 * On a stock boot base wins the device outright: it is compiled into GENERIC,
 * it probes long before any kext exists, and it becomes the vt(4) console
 * backend. On arm64 that is the ONLY console -- a qemu `virt` guest and Apple
 * Virtualization.framework both lack an EFI GOP, so there is no efifb to fall
 * back on. That is exactly why base stays compiled in rather than being
 * nodevice'd out: removing it buys KMS at the price of a blind boot, from the
 * first kernel message until userland gets around to loading this kext.
 *
 * But loading this module later does not disturb base either, and that is the
 * problem this solves. bus_generic_driver_added() re-probes only children in
 * DS_NOTPRESENT, so an already-attached vtgpu0 is never re-bid no matter how
 * much better a driver arrives afterwards, and /dev/dri/card0 never appears.
 * (releng/15.1 has no DF_REBID; that mechanism is gone.)
 *
 * So take the device explicitly. This is DEV_SET_DRIVER's sequence -- see the
 * devctl2 ioctl in kern/subr_bus.c -- expressed in exported functions: detach
 * base, then re-probe. device_detach() drops the devclass of a non-fixed
 * device, so the re-probe is a clean bid, and virtio_gpu_drm_probe()'s
 * BUS_PROBE_VENDOR outranks base's BUS_PROBE_DEFAULT. This driver wins.
 *
 * The detach and the re-attach MUST stay inside one bus_topo_lock() section.
 * Measured on arm64: leave vt(4) with no backend between them -- a bare
 * `devctl detach vtgpu0` does exactly that -- and the machine panics seconds
 * later, when the next console write lands on a driver that is gone.
 * vtgpu_detach() calls vt_deallocate(), and nothing repaints until the
 * replacement registers drmfb. The whole handover is one atomic step or it is
 * a crash; there is no middle state that survives.
 *
 * Deferred to a task, and driven from a SYSINIT rather than a chained module
 * event handler, for two independent reasons:
 *   - driver_module_handler() calls the chained MOD_LOAD handler BEFORE
 *     devclass_add_driver() (kern/subr_bus.c), so inline this driver would not
 *     yet be registered and the re-probe would hand the device back to base;
 *   - VIRTIO_DRIVER_MODULE registers on both virtio transports, so a chained
 *     handler runs twice.
 */
static struct task virtio_gpu_drm_takeover_task;

static void
virtio_gpu_drm_takeover(void *ctx __unused, int pending __unused)
{
	char nameunit[64];
	devclass_t dc;
	device_t *devs;
	int error, i, ndevs;

	dc = devclass_find("vtgpu");		/* base virtio_gpu(4) */
	if (dc == NULL)
		return;				/* base not present -- nothing to take */

	bus_topo_lock();
	if (devclass_get_devices(dc, &devs, &ndevs) != 0) {
		bus_topo_unlock();
		return;
	}
	for (i = 0; i < ndevs; i++) {
		if (!device_is_attached(devs[i]))
			continue;
		/*
		 * Snapshot the name: device_detach() releases the unit back to
		 * the devclass, so device_get_nameunit() is only meaningful
		 * while base still owns the device.
		 */
		strlcpy(nameunit, device_get_nameunit(devs[i]),
		    sizeof(nameunit));

		/*
		 * Ask base to hand the device over running: then the detach
		 * doesn't reset it, this driver adopts its features and
		 * virtqueues, and the host keeps showing base's framebuffer
		 * (the boot splash) until our first mode replaces it, instead
		 * of a dark screen in between.  Where base or the transport
		 * can't, the detach resets the device as it always did.
		 */
		error = vtgpu_handoff(devs[i]);
		printf("virtio_gpu_drm: %s %s\n", nameunit, error == 0 ?
		    "hands the running device over" :
		    "can't hand the device over running; resetting it");

		error = device_detach(devs[i]);
		if (error != 0) {
			/*
			 * Base kept the device (it is the console, so this is
			 * the safe outcome). No KMS this boot, but the screen
			 * still works.
			 */
			printf("virtio_gpu_drm: %s would not detach (%d); "
			    "leaving base virtio_gpu(4) in place\n",
			    nameunit, error);
			continue;
		}
		error = device_probe_and_attach(devs[i]);
		if (error != 0)
			printf("virtio_gpu_drm: re-probe of %s failed (%d)\n",
			    nameunit, error);
	}
	free(devs, M_TEMP);
	bus_topo_unlock();
}

static void
virtio_gpu_drm_takeover_init(void *arg __unused)
{

	TASK_INIT(&virtio_gpu_drm_takeover_task, 0, virtio_gpu_drm_takeover,
	    NULL);
	taskqueue_enqueue(taskqueue_thread, &virtio_gpu_drm_takeover_task);
}
/*
 * SI_SUB_CONFIGURE is after SI_SUB_KLD, where a kld's DRIVER_MODULEs register,
 * so by the time this runs virtio_gpu_drm is a candidate driver.
 */
SYSINIT(virtio_gpu_drm_takeover, SI_SUB_CONFIGURE, SI_ORDER_ANY,
    virtio_gpu_drm_takeover_init, NULL);

VIRTIO_DRIVER_MODULE(virtio_gpu_drm, virtio_gpu_drm_driver, 0, 0);
MODULE_VERSION(virtio_gpu_drm, 1);

/*
 * MODULE_DEPEND is depth-1: kern_linker.c resolves symbols against file->deps[]
 * without recursing, so every module whose symbols this one references
 * directly has to be named here even when a dependency already names it. That
 * is why drmn and linuxkpi appear alongside linux_virtio.
 */
MODULE_DEPEND(virtio_gpu_drm, virtio, 1, 1, 1);
MODULE_DEPEND(virtio_gpu_drm, linux_virtio, 1, 1, 1);
MODULE_DEPEND(virtio_gpu_drm, linuxkpi, 1, 1, 1);
MODULE_DEPEND(virtio_gpu_drm, drmn, 2, 2, 2);
MODULE_DEPEND(virtio_gpu_drm, dmabuf, 1, 1, 1);
/*
 * Two helper modules, not one. drm_extra_helpers carries drm_simple_encoder_init
 * and drm_gem_fb_create_handle; drm_shmem_helpers carries the shmem GEM layer
 * and drm_fbdev_shmem. They are separate .ko's so that a failure in the newer,
 * unproven one cannot take down the module BochsGraphics resolves through.
 */
MODULE_DEPEND(virtio_gpu_drm, drm_extra_helpers, 1, 1, 1);
MODULE_DEPEND(virtio_gpu_drm, drm_shmem_helpers, 1, 1, 1);
