/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * The platform-device registry (nextbsd-kernel-extensions-graphics#51).
 *
 * LinuxKPI has no platform bus. vc4's master builds its component match list
 * by asking, for each entry in component_drivers[], which devices are bound to
 * that driver -- platform_find_device_by_driver(). With no bus there was
 * nothing to answer with, and the function was deliberately left undefined so
 * the build would fail rather than quietly produce an empty match list.
 *
 * That caution was right. An empty match list is COMPLETE by definition, so
 * the master binds immediately with zero components, reports success, and
 * leaves the display dark -- a failure that points nowhere near its cause.
 *
 * The newbus shims already build one struct platform_device per attached
 * block, so the registry is just those: recorded at attach, removed at detach.
 * A vc4 pipeline is five devices, so a list is the right shape.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/queue.h>
#include <sys/bus.h>
#include <sys/rman.h>

#include <machine/bus.h>
#include <machine/resource.h>	/* SYS_RES_MEMORY */

#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of.h>

struct lkpi_pdev_ent {
	TAILQ_ENTRY(lkpi_pdev_ent)	 link;
	struct platform_device		*pdev;
	const struct lkpi_driver	*drv;
};

static TAILQ_HEAD(, lkpi_pdev_ent) lkpi_pdevs =
    TAILQ_HEAD_INITIALIZER(lkpi_pdevs);
static struct mtx lkpi_pdev_mtx;
MTX_SYSINIT(lkpi_pdev_mtx, &lkpi_pdev_mtx, "lkpi-pdevs", MTX_DEF);

void
lkpi_platform_device_register(struct platform_device *pdev,
    const struct lkpi_driver *drv)
{
	struct lkpi_pdev_ent *e;

	if (pdev == NULL)
		return;
	e = malloc(sizeof(*e), M_DEVBUF, M_WAITOK | M_ZERO);
	e->pdev = pdev;
	e->drv = drv;
	mtx_lock(&lkpi_pdev_mtx);
	TAILQ_INSERT_TAIL(&lkpi_pdevs, e, link);
	mtx_unlock(&lkpi_pdev_mtx);
}

void
lkpi_platform_device_unregister(struct platform_device *pdev)
{
	struct lkpi_pdev_ent *e, *tmp;

	mtx_lock(&lkpi_pdev_mtx);
	TAILQ_FOREACH_SAFE(e, &lkpi_pdevs, link, tmp) {
		if (e->pdev != pdev)
			continue;
		TAILQ_REMOVE(&lkpi_pdevs, e, link);
		mtx_unlock(&lkpi_pdev_mtx);
		free(e, M_DEVBUF);
		return;
	}
	mtx_unlock(&lkpi_pdev_mtx);
}

/*
 * Iterator, matching the shape vc4_match_add_drivers() expects: pass NULL to
 * begin, then pass the previous result to continue. Returns NULL when the
 * driver has no more devices.
 *
 * Upstream also takes a reference on the returned device and expects the
 * caller to put_device() it. Nothing here is reference counted -- these
 * devices live exactly as long as their newbus attachment, which outlives the
 * master's bind -- so put_device() is the no-op it already was, and this hands
 * the pointer back directly.
 */
struct device *
platform_find_device_by_driver(struct device *start,
    const struct lkpi_driver *drv)
{
	struct lkpi_pdev_ent *e;
	struct device *found = NULL;
	bool seen;

	if (drv == NULL)
		return (NULL);

	seen = (start == NULL);
	mtx_lock(&lkpi_pdev_mtx);
	TAILQ_FOREACH(e, &lkpi_pdevs, link) {
		if (!seen) {
			if (&e->pdev->dev == start)
				seen = true;
			continue;
		}
		if (e->drv == drv) {
			found = &e->pdev->dev;
			break;
		}
	}
	mtx_unlock(&lkpi_pdev_mtx);
	return (found);
}

/*
 * FreeBSD half of platform_get_resource_byname(): the address and length of
 * the node's Nth reg entry, as plain integers.
 *
 * Split deliberately. The Linux-facing function returns LinuxKPI's struct
 * resource from <linux/ioport.h>, and this file needs FreeBSD's identically
 * named one from sys/rman.h to call bus_get_resource(). A translation unit
 * gets one or the other, never both -- conflating them is what broke the
 * amdgpu build in nextbsd/nextbsd-kernel#200. So the halves live in different
 * files and exchange integers.
 */
int
lkpi_of_reg_by_index(struct platform_device *pdev, int idx, uint64_t *startp,
    uint64_t *lenp)
{
	struct resource *res;
	rman_res_t start, count;
	int rid;

	if (pdev == NULL || pdev->dev.bsddev == NULL || idx < 0)
		return (-EINVAL);

	/*
	 * ALLOCATE to get the address, do not read the resource list (#51).
	 *
	 * bus_get_resource() returns the resource list entry, which on a
	 * simplebus child is the UNTRANSLATED child address. The ranges
	 * translation happens in simplebus_alloc_resource(), so the list and
	 * the real address differ whenever a bus has non-trivial ranges.
	 *
	 * On bcm2712 they differ by 4GB. soc@107c000000 has #address-cells 1
	 * under a root with #address-cells 2, and
	 *
	 *	ranges = <0x00000000  0x00000010 0x00000000  0x80000000>
	 *
	 * so hdmi0's core bank at child 0x7c701400 really lives at
	 * 0x107c701400. Reading the list gave 0x7c701400, devm_ioremap()
	 * mapped that, and every access landed on an unbacked page: the whole
	 * HDMI register window read as a floating bus, 0xffffffff with the odd
	 * bit flipped, even while the firmware was driving the panel.
	 *
	 * lkpi_platform_ioremap_resource() never had this problem because it
	 * allocates. This is the same fix for the by-name path.
	 *
	 * The resource is released immediately: the caller wants the physical
	 * address to ioremap, not the allocation, and holding it would make
	 * the driver's own later allocation of the same rid fail.
	 */
	rid = idx;
	res = bus_alloc_resource_any(pdev->dev.bsddev, SYS_RES_MEMORY, &rid,
	    RF_ACTIVE);
	if (res == NULL)
		return (-ENOENT);
	start = rman_get_start(res);
	count = rman_get_size(res);
	bus_release_resource(pdev->dev.bsddev, SYS_RES_MEMORY, rid, res);

	if (startp != NULL)
		*startp = (uint64_t)start;
	if (lenp != NULL)
		*lenp = (uint64_t)count;
	return (0);
}
