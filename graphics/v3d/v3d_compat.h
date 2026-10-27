/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The gap between the v3d sources and the DRM that drm-kmod 6.12-lts provides
 * (nextbsd-kernel-extensions-graphics#66).
 *
 * Force-included from the Makefile rather than shipped as a shadowing
 * <drm/...> or <linux/...> header. Shadowing drm-kmod's own copies for this
 * module would silently drop whatever drm-kmod added to them -- the mistake
 * that broke the drm-kmod build on both arches during #51. Everything here
 * either has no drm-kmod counterpart at all, or is a name for one that does.
 */
#ifndef _V3D_COMPAT_H_
#define _V3D_COMPAT_H_

#include <linux/idr.h>
#include <drm/drm_gem_shmem_helper.h>
#include <sys/sysctl.h>

/*
 * linuxkpi's module_param() expands to a tunable under sysctl _hw_<module> and
 * declares that node in no translation unit. v3d_drv.c has three of them
 * (debug_mmu and friends), so the node has to be visible here -- this header
 * is force-included into every v3d file -- and DEFINED exactly once, which
 * v3d_newbus.c does. Two definitions is a duplicate symbol at link time rather
 * than a warning, which bochs found the hard way.
 */
SYSCTL_DECL(_hw_v3d);

/*
 * module_platform_driver() (v3d_drv.c).
 *
 * Upstream this expands to a module_init/module_exit pair that registers the
 * platform driver with the Linux platform bus. There is no platform bus here:
 * v3d_newbus.c attaches to the device-tree node and calls the vendored probe
 * itself, so registration would have nothing to register with.
 *
 * It expands to a redundant declaration rather than to nothing, so that the
 * trailing semicolon at file scope stays legal C and the macro still names the
 * driver it is given -- expanding to empty leaves a stray ";" that
 * -Wextra-semi rejects.
 */
#define	module_platform_driver(__drv)	extern struct platform_driver __drv


/*
 * idr_init_base() (v3d_perfmon.c). LinuxKPI has idr_init(), which is
 * idr_init_base() with a base of 0. v3d asks for base 1 so that a perfmon id
 * of 0 can mean "none" -- and it never relies on the allocator honouring that,
 * because every id it hands out comes back through idr_find(). A base of 0
 * therefore changes which integers get allocated and nothing else.
 *
 * Taking the base seriously would mean reimplementing the allocator; ignoring
 * it costs the id-0 sentinel, which v3d does not use as one.
 */
static inline void
idr_init_base(struct idr *idr, int base)
{

	(void)base;
	idr_init(idr);
}

/*
 * platform_get_irq_optional() (v3d_irq.c). The difference from
 * platform_get_irq() upstream is only that the optional form does not log an
 * error when the interrupt is absent. Ours does not log either, so this is the
 * same call.
 *
 * v3d uses it to ask for irq 1 -- the second line -- and treats "not there" as
 * meaning the part routes both hub and core through a single line, which it
 * then handles with v3d->single_irq_line. So a wrong answer here does not fail
 * the probe, it silently halves the interrupt handling; index 1 does exist on
 * a 2712 (interrupts = <0 250 4>, <0 249 4>).
 */
static inline int
platform_get_irq_optional(struct platform_device *pdev, unsigned int num)
{

	return (lkpi_platform_get_irq(pdev, num));
}

/*
 * Forward declaration at file scope, so the mnt parameter below does not
 * declare the type inside its own prototype -- where it would be a distinct,
 * function-local type and every caller passing v3d->gemfs would be a pointer
 * mismatch. There is no LinuxKPI <linux/mount.h> to include, and nothing here
 * ever dereferences it: it exists only so the NULL that v3d_gemfs_stub.c leaves
 * in v3d->gemfs has a type to travel as.
 */
struct vfsmount;

/*
 * drm_gem_shmem_create_with_mnt() (v3d_bo.c).
 *
 * Upstream v3d creates a private tmpfs mount with huge pages enabled and
 * allocates its GEM objects from it, so that large buffers are backed by 2MB
 * pages instead of 4K ones. That is v3d_gemfs.c, which is not vendored: it is
 * built on get_fs_type() and vfs_kern_mount(), and LinuxKPI has no VFS to
 * mount anything on.
 *
 * v3d_gemfs_init() therefore leaves v3d->gemfs NULL, and upstream's own
 * behaviour for a NULL mount is to fall back to the normal shmem allocation --
 * which is exactly drm_gem_shmem_create(). So this is not a stub that loses
 * correctness, only the huge-page optimisation. The assertion is here so that
 * if a mount ever does appear, this silently ignoring it is caught.
 */
static inline struct drm_gem_shmem_object *
drm_gem_shmem_create_with_mnt(struct drm_device *dev, size_t size,
    struct vfsmount *mnt)
{

	WARN_ON_ONCE(mnt != NULL);
	return (drm_gem_shmem_create(dev, size));
}

#endif /* _V3D_COMPAT_H_ */
