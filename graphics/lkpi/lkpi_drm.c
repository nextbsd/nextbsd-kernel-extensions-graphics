/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * DRM helpers vc4 needs that drm-kmod does not provide
 * (nextbsd-kernel-extensions-graphics#51). See lkpi_drm.h for why these live in the
 * module rather than in a drm-kmod patch.
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/malloc.h>

#include <linux/mutex.h>
#include <linux/dma-fence.h>
#include <linux/dma-fence-array.h>
#include <linux/err.h>

#include <drm/drm_device.h>
#include <drm/drm_managed.h>

#include <linux/ioport.h>
#include <linux/platform_device.h>
#include <linux/of.h>

#include "lkpi_drm.h"

static void
drmm_mutex_release(struct drm_device *dev __unused, void *p)
{

	linux_mutex_destroy((struct mutex *)p);
}

/*
 * Ordering matters: the mutex is initialised BEFORE the release action is
 * registered. If registration fails, an already-initialised mutex is destroyed
 * rather than left live, and the caller sees the error.
 */
int
drmm_mutex_init(struct drm_device *dev, struct mutex *lock)
{
	int ret;

	if (dev == NULL || lock == NULL)
		return (-EINVAL);

	linux_mutex_init(lock, "drmm_mutex", SX_NOWITNESS);
	ret = drmm_add_action(dev, drmm_mutex_release, lock);
	if (ret != 0)
		linux_mutex_destroy(lock);
	return (ret);
}

bool
dma_fence_match_context(struct dma_fence *fence, u64 context)
{
	struct dma_fence_array *array;
	struct dma_fence *f;
	unsigned int i;

	if (fence == NULL)
		return (false);

	if (!dma_fence_is_array(fence))
		return (fence->context == context);

	/*
	 * Every member must match. One member from another context means the
	 * caller still has to wait, so a single mismatch fails the whole test.
	 */
	array = to_dma_fence_array(fence);
	for (i = 0; i < array->num_fences; i++) {
		f = array->fences[i];
		if (f->context != context)
			return (false);
	}
	return (true);
}

/*
 * Linux half of platform_get_resource_byname(): the register bank a device
 * tree calls `name` in reg-names. vc4_hdmi has eight and asks for each by
 * name, so index lookup is not enough.
 *
 * Here rather than in lkpi_platform.c because the return type is LinuxKPI's
 * struct resource; that file needs FreeBSD's to call bus_get_resource(), and a
 * translation unit gets one or the other. The two halves exchange integers.
 *
 * The resource is allocated per lookup and never freed: callers keep it for
 * the life of the device and there are eight per controller, so a free path
 * would be more code than the leak is worth. Stated, not hidden.
 */
struct resource *
platform_get_resource_byname(struct platform_device *pdev, unsigned int type,
    const char *name)
{
	struct device_node *dn;
	struct resource *res;
	uint64_t start, len;
	int idx;

	if (pdev == NULL || name == NULL || type != IORESOURCE_MEM)
		return (NULL);
	dn = dev_of_node(&pdev->dev);
	if (dn == NULL)
		return (NULL);
	idx = of_property_match_string(dn, "reg-names", name);
	if (idx < 0)
		return (NULL);
	if (lkpi_of_reg_by_index(pdev, idx, &start, &len) != 0)
		return (NULL);

	res = malloc(sizeof(*res), M_DEVBUF, M_WAITOK | M_ZERO);
	res->start = (resource_size_t)start;
	res->end = (resource_size_t)(start + len - 1);
	res->name = name;
	return (res);
}

/*
 * devm_platform_ioremap_resource_byname() (v3d, #66): resolve the bank name
 * through reg-names, then map it by the index that lookup produced. The
 * mapping itself is the existing index path -- this only translates the name,
 * so the two spellings cannot map differently.
 */
void *
lkpi_platform_ioremap_resource_byname(struct platform_device *pdev,
    const char *name)
{
	struct device_node *dn;
	int idx;

	if (pdev == NULL || name == NULL)
		return (ERR_PTR(-EINVAL));
	dn = dev_of_node(&pdev->dev);
	if (dn == NULL)
		return (ERR_PTR(-ENODEV));
	idx = of_property_match_string(dn, "reg-names", name);
	if (idx < 0)
		return (ERR_PTR(-ENOENT));
	return (lkpi_platform_ioremap_resource(pdev, (unsigned int)idx));
}

/*
 * drm_print_regset32() -- register dump for debugfs (#51).
 *
 * drm-kmod's drm.ko does not export it, so the kext failed the resolution
 * gate with ENOEXEC on this one symbol.
 *
 * The callers are debugfs show handlers -- vc4_hdmi_debugfs_regs(),
 * vc4_crtc_debugfs_regs(), vc4_hvs_debugfs_regs(). They are compiled because
 * upstream does not guard the handler bodies themselves, only their
 * registration, and this module builds with CONFIG_DEBUG_FS filtered out
 * because vc4_debugfs.c is not vendored. So nothing registers them and nothing
 * can call them.
 *
 * A no-op rather than a real dump: struct debugfs_regset32 here has no base
 * member to read from (see the deviations in vc4_hvs.c and vc4_crtc.c), so
 * there is nothing to print even if a caller existed. It logs once if it is
 * ever reached, because that would mean the reachability argument above is
 * wrong.
 */
void
drm_print_regset32(struct drm_printer *p __unused,
    struct debugfs_regset32 *regset __unused)
{

	pr_warn_once("vc4_kms: drm_print_regset32 called -- debugfs is not "
	    "wired up in this module (#51)\n");
}

/*
 * devm_ioremap() -- map a device register range (#51).
 *
 * LinuxKPI's is a stub that returns NULL unconditionally
 * (sys/compat/linuxkpi/common/include/linux/io.h). vc4_hdmi calls it for each
 * of its NINE register banks, and every call site is
 *
 *	x = devm_ioremap(dev, res->start, resource_size(res));
 *	if (!x)
 *		return -ENOMEM;
 *
 * so the bind failed with a bare "-12" and no message from vc4 at all -- the
 * driver thinks it is an ordinary allocation failure. That is what
 * "lkpi component: master bind failed: -12" was, after the clocks were sorted.
 *
 * ioremap() itself IS implemented in LinuxKPI, as _ioremap_attr() with
 * VM_MEMATTR_DEVICE, which is the right attribute for registers. So this is a
 * thin wrapper over it.
 *
 * "devm" does not auto-release: LinuxKPI's devres does not track these
 * mappings. A display driver maps its registers at bind and holds them until
 * unload, so the mapping lives as long as the module -- the same bargain
 * devm_clk_get() already makes here, and stated rather than assumed.
 */
void *
lkpi_devm_ioremap(struct device *dev __unused, resource_size_t offset,
    resource_size_t size)
{

	if (offset == 0 || size == 0)
		return (NULL);
	return (ioremap(offset, size));
}
