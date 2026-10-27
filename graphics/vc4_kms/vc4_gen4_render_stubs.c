/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Stubs for the GEN_4 render path (nextbsd-kernel-extensions-graphics#51).
 *
 * vc4 is two drivers in one source tree: a display pipeline, and a 3D command
 * submission engine for the original VideoCore IV. This module is the display
 * half, for BCM2712, and the render half is unreachable here -- provably, not
 * by assumption:
 *
 *   vc4_drm_bind() selects vc5_drm_driver for gen > VC4_GEN_4 (vc4_drv.c:330),
 *   and vc5_drm_driver declares
 *
 *	.driver_features = (DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM)
 *
 *   with NO .ioctls table and no DRIVER_RENDER. Every entry point below is
 *   reached only through vc4_drm_ioctls[], which is attached to vc4_drm_driver
 *   -- the GEN_4 driver, which a 2712 never selects. On this SoC the 3D core is
 *   driven by the separate drm/v3d driver, not from here.
 *
 * So the render sources (vc4_gem.c, vc4_v3d.c, vc4_validate*.c, vc4_render_cl.c,
 * vc4_perfmon.c, vc4_irq.c) are not built, and these stand in for the symbols
 * vc4_drv.c still names in that unreachable table.
 *
 * Stubs rather than #ifdefs through the vendored sources: one clearly marked
 * file beats scattering conditionals through four files we want to keep
 * diffable against upstream.
 *
 * Each returns an error rather than succeeding quietly. If one is ever called,
 * something about the reachability argument above is wrong, and an -ENODEV a
 * caller checks is a far better outcome than a stub that pretends to work.
 */

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/printk.h>

#include <drm/drm_device.h>
#include <drm/drm_file.h>

#include "vc4_drv.h"

#define	VC4_GEN4_UNREACHABLE(what)					\
	pr_warn_once("vc4_kms: %s called -- the GEN_4 render path is "	\
	    "not built on this SoC (#51)\n", (what))

int
vc4_gem_init(struct drm_device *dev __unused)
{

	/*
	 * Called from vc4_drm_bind() only under if (gen == VC4_GEN_4).
	 * Reaching it means the gen detection is wrong.
	 */
	VC4_GEN4_UNREACHABLE("vc4_gem_init");
	return (-ENODEV);
}

int
vc4_queue_seqno_cb(struct drm_device *dev __unused,
    struct vc4_seqno_cb *cb __unused, uint64_t seqno __unused,
    void (*func)(struct vc4_seqno_cb *cb) __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_queue_seqno_cb");
	return (-ENODEV);
}

int
vc4_v3d_bin_bo_get(struct vc4_dev *vc4 __unused, bool *used)
{

	if (used != NULL)
		*used = false;
	VC4_GEN4_UNREACHABLE("vc4_v3d_bin_bo_get");
	return (-ENODEV);
}

void
vc4_v3d_bin_bo_put(struct vc4_dev *vc4 __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_v3d_bin_bo_put");
}

int
vc4_v3d_pm_get(struct vc4_dev *vc4 __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_v3d_pm_get");
	return (-ENODEV);
}

void
vc4_v3d_pm_put(struct vc4_dev *vc4 __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_v3d_pm_put");
}

struct vc4_validated_shader_info *
vc4_validate_shader(struct drm_gem_dma_object *shader_obj __unused)
{

	/*
	 * NULL is the documented failure return here, and vc4_bo.c checks it:
	 * a shader BO that cannot be validated must never be accepted.
	 */
	VC4_GEN4_UNREACHABLE("vc4_validate_shader");
	return (NULL);
}

void
vc4_perfmon_open_file(struct vc4_file *vc4file __unused)
{
}

void
vc4_perfmon_close_file(struct vc4_file *vc4file __unused)
{
}

/* The render ioctls, reachable only through vc4_drm_driver's table. */
int
vc4_submit_cl_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_submit_cl_ioctl");
	return (-ENODEV);
}

int
vc4_wait_seqno_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_wait_seqno_ioctl");
	return (-ENODEV);
}

int
vc4_wait_bo_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_wait_bo_ioctl");
	return (-ENODEV);
}

int
vc4_get_hang_state_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_get_hang_state_ioctl");
	return (-ENODEV);
}

int
vc4_perfmon_create_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_perfmon_create_ioctl");
	return (-ENODEV);
}

int
vc4_perfmon_destroy_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_perfmon_destroy_ioctl");
	return (-ENODEV);
}

int
vc4_perfmon_get_values_ioctl(struct drm_device *dev __unused,
    void *data __unused, struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_perfmon_get_values_ioctl");
	return (-ENODEV);
}

int
vc4_gem_madvise_ioctl(struct drm_device *dev __unused, void *data __unused,
    struct drm_file *file_priv __unused)
{

	VC4_GEN4_UNREACHABLE("vc4_gem_madvise_ioctl");
	return (-ENODEV);
}
