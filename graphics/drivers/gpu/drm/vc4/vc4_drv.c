// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2014-2015 Broadcom
 * Copyright (C) 2013 Red Hat
 */

/**
 * DOC: Broadcom VC4 Graphics Driver
 *
 * The Broadcom VideoCore 4 (present in the Raspberry Pi) contains a
 * OpenGL ES 2.0-compatible 3D engine called V3D, and a highly
 * configurable display output pipeline that supports HDMI, DSI, DPI,
 * and Composite TV output.
 *
 * The 3D engine also has an interface for submitting arbitrary
 * compute shader-style jobs using the same shader processor as is
 * used for vertex and fragment shaders in GLES 2.0.  However, given
 * that the hardware isn't able to expose any standard interfaces like
 * OpenGL compute shaders or OpenCL, it isn't supported by this
 * driver.
 */

#include <linux/clk.h>
#include <linux/component.h>
#include <linux/device.h>
#include <linux/dma-mapping.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/dma-direct.h>

#include <drm/drm_aperture.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_drv.h>
#include <drm/drm_fbdev_dma.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include <soc/bcm2835/raspberrypi-firmware.h>

#include "uapi/drm/vc4_drm.h"

#include "vc4_drv.h"
#include "vc4_regs.h"

#define DRIVER_NAME "vc4"
#define DRIVER_DESC "Broadcom VC4 graphics"
#define DRIVER_DATE "20140616"
#define DRIVER_MAJOR 0
#define DRIVER_MINOR 0
#define DRIVER_PATCHLEVEL 0

/* Helper function for mapping the regs on a platform device. */
void __iomem *vc4_ioremap_regs(struct platform_device *pdev, int index)
{
	void __iomem *map;

	map = devm_platform_ioremap_resource(pdev, index);
	if (IS_ERR(map))
		return map;

	return map;
}

int vc4_dumb_fixup_args(struct drm_mode_create_dumb *args)
{
	int min_pitch = DIV_ROUND_UP(args->width * args->bpp, 8);

	if (args->pitch < min_pitch)
		args->pitch = min_pitch;

	if (args->size < args->pitch * args->height)
		args->size = args->pitch * args->height;

	return 0;
}

static int vc5_dumb_create(struct drm_file *file_priv,
			   struct drm_device *dev,
			   struct drm_mode_create_dumb *args)
{
	int ret;

	ret = vc4_dumb_fixup_args(args);
	if (ret)
		return ret;

	return drm_gem_dma_dumb_create_internal(file_priv, dev, args);
}

static int vc4_get_param_ioctl(struct drm_device *dev, void *data,
			       struct drm_file *file_priv)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct drm_vc4_get_param *args = data;
	int ret;

	if (args->pad != 0)
		return -EINVAL;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	if (!vc4->v3d)
		return -ENODEV;

	switch (args->param) {
	case DRM_VC4_PARAM_V3D_IDENT0:
		ret = vc4_v3d_pm_get(vc4);
		if (ret)
			return ret;
		args->value = V3D_READ(V3D_IDENT0);
		vc4_v3d_pm_put(vc4);
		break;
	case DRM_VC4_PARAM_V3D_IDENT1:
		ret = vc4_v3d_pm_get(vc4);
		if (ret)
			return ret;
		args->value = V3D_READ(V3D_IDENT1);
		vc4_v3d_pm_put(vc4);
		break;
	case DRM_VC4_PARAM_V3D_IDENT2:
		ret = vc4_v3d_pm_get(vc4);
		if (ret)
			return ret;
		args->value = V3D_READ(V3D_IDENT2);
		vc4_v3d_pm_put(vc4);
		break;
	case DRM_VC4_PARAM_SUPPORTS_BRANCHES:
	case DRM_VC4_PARAM_SUPPORTS_ETC1:
	case DRM_VC4_PARAM_SUPPORTS_THREADED_FS:
	case DRM_VC4_PARAM_SUPPORTS_FIXED_RCL_ORDER:
	case DRM_VC4_PARAM_SUPPORTS_MADVISE:
	case DRM_VC4_PARAM_SUPPORTS_PERFMON:
		args->value = true;
		break;
	default:
		DRM_DEBUG("Unknown parameter %d\n", args->param);
		return -EINVAL;
	}

	return 0;
}

static int vc4_open(struct drm_device *dev, struct drm_file *file)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_file *vc4file;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return -ENODEV;

	vc4file = kzalloc(sizeof(*vc4file), GFP_KERNEL);
	if (!vc4file)
		return -ENOMEM;
	vc4file->dev = vc4;

	vc4_perfmon_open_file(vc4file);
	file->driver_priv = vc4file;
	return 0;
}

static void vc4_close(struct drm_device *dev, struct drm_file *file)
{
	struct vc4_dev *vc4 = to_vc4_dev(dev);
	struct vc4_file *vc4file = file->driver_priv;

	if (WARN_ON_ONCE(vc4->gen > VC4_GEN_4))
		return;

	if (vc4file->bin_bo_used)
		vc4_v3d_bin_bo_put(vc4);

	vc4_perfmon_close_file(vc4file);
	kfree(vc4file);
}

static struct drm_gem_object *
vc4_prime_import_sg_table(struct drm_device *dev,
			  struct dma_buf_attachment *attach,
			  struct sg_table *sgt)
{
	phys_addr_t phys = dma_to_phys(dev->dev, sg_dma_address(sgt->sgl));

	if (swiotlb_find_pool(dev->dev, phys))
		return ERR_PTR(-EINVAL);

	return drm_gem_dma_prime_import_sg_table(dev, attach, sgt);
}

DEFINE_DRM_GEM_FOPS(vc4_drm_fops);

static const struct drm_ioctl_desc vc4_drm_ioctls[] = {
	DRM_IOCTL_DEF_DRV(VC4_SUBMIT_CL, vc4_submit_cl_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_WAIT_SEQNO, vc4_wait_seqno_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_WAIT_BO, vc4_wait_bo_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_CREATE_BO, vc4_create_bo_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_MMAP_BO, vc4_mmap_bo_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_CREATE_SHADER_BO, vc4_create_shader_bo_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_GET_HANG_STATE, vc4_get_hang_state_ioctl,
			  DRM_ROOT_ONLY),
	DRM_IOCTL_DEF_DRV(VC4_GET_PARAM, vc4_get_param_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_SET_TILING, vc4_set_tiling_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_GET_TILING, vc4_get_tiling_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_LABEL_BO, vc4_label_bo_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_GEM_MADVISE, vc4_gem_madvise_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_PERFMON_CREATE, vc4_perfmon_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_PERFMON_DESTROY, vc4_perfmon_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(VC4_PERFMON_GET_VALUES, vc4_perfmon_get_values_ioctl, DRM_RENDER_ALLOW),
};

const struct drm_driver vc4_drm_driver = {
	.driver_features = (DRIVER_MODESET |
			    DRIVER_ATOMIC |
			    DRIVER_GEM |
			    DRIVER_RENDER |
			    DRIVER_SYNCOBJ),
	.open = vc4_open,
	.postclose = vc4_close,

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = vc4_debugfs_init,
#endif

	.gem_create_object = vc4_create_object,

	.dumb_create		= vc4_bo_dumb_create,
	.gem_prime_import_sg_table = vc4_prime_import_sg_table,

	.ioctls = vc4_drm_ioctls,
	.num_ioctls = ARRAY_SIZE(vc4_drm_ioctls),
	.fops = &vc4_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

const struct drm_driver vc5_drm_driver = {
	.driver_features = (DRIVER_MODESET |
			    DRIVER_ATOMIC |
			    DRIVER_GEM),

#if defined(CONFIG_DEBUG_FS)
	.debugfs_init = vc4_debugfs_init,
#endif

	.dumb_create		= vc5_dumb_create,
	.gem_prime_import_sg_table = vc4_prime_import_sg_table,

	.fops = &vc4_drm_fops,

	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

static void vc4_match_add_drivers(struct device *dev,
				  struct component_match **match,
				  struct platform_driver *const *drivers,
				  int count)
{
	int i;

	for (i = 0; i < count; i++) {
		/*
		 * DEVIATION (#51): struct lkpi_driver, not struct
		 * device_driver. The kernel's device_driver has no
		 * of_match_table and is shared with drm.ko, so a module cannot
		 * add one; struct platform_driver carries its own type here
		 * instead. Same member names, so every initialiser below is
		 * unchanged.
		 */
		const struct lkpi_driver *drv = &drivers[i]->driver;
		struct device *p = NULL, *d;

		while ((d = platform_find_device_by_driver(p, drv))) {
			put_device(p);
			component_match_add(dev, match, component_compare_dev, d);
			p = d;
		}
		put_device(p);
	}
}

static void vc4_component_unbind_all(void *ptr)
{
	struct vc4_dev *vc4 = ptr;

	component_unbind_all(vc4->dev, &vc4->base);
}

static const struct of_device_id vc4_dma_range_matches[] = {
	{ .compatible = "brcm,bcm2711-hvs" },
	{ .compatible = "brcm,bcm2712-hvs" },
	{ .compatible = "brcm,bcm2835-hvs" },
	{ .compatible = "brcm,bcm2835-v3d" },
	{ .compatible = "brcm,cygnus-v3d" },
	{ .compatible = "brcm,vc4-v3d" },
	{}
};

/*
 * we need this helper function for determining presence of fkms
 * before it's been bound
 */
static bool firmware_kms(void)
{
	return of_device_is_available(of_find_compatible_node(NULL, NULL,
	       "raspberrypi,rpi-firmware-kms")) ||
	       of_device_is_available(of_find_compatible_node(NULL, NULL,
	       "raspberrypi,rpi-firmware-kms-2711"));
}

/*
 * The initial modeset can be switched off (#51).
 *
 * This gate was added while the machine was panicking on every kextload and
 * drm_fbdev_dma_setup() was the suspect. It was not the cause: the panic was
 * vc4_hdmi_disable_scrambling() driving SCDC over a NULL DDC adapter, from
 * vc4_crtc_disable_at_boot(), and it is fixed. So the default is on, which is
 * upstream behaviour.
 *
 * The knob stays because it is the one switch that separates "the driver binds"
 * from "the driver programs the display", and those fail differently. Set
 * compat.linuxkpi.vc4_kms.enable_fbdev=0 to bind without touching the display.
 *
 * Note it is a sysctl, not a loader tunable: LinuxKPI's module_param() does not
 * register one, so setting it with kenv before kextload does nothing.
 */
static int enable_fbdev = 1;
module_param(enable_fbdev, int, 0644);
MODULE_PARM_DESC(enable_fbdev,
    "Run fbdev emulation's initial modeset (default on) (#51)");

/*
 * Whether to tell the firmware to let go of the display.
 *
 * On by default, which is what a driver taking over the display must do. It
 * was briefly turned off to test whether the firmware powering down on release
 * was what killed the HDMI register window; it was not -- the window read dead
 * while the firmware was still driving the panel, which is what pointed at the
 * address translation bug instead.
 */
static int notify_display_done = 1;
module_param(notify_display_done, int, 0644);
MODULE_PARM_DESC(notify_display_done,
    "Tell the firmware to release the display (default on) (#51)");

static int vc4_drm_bind(struct device *dev)
{
	struct platform_device *pdev = to_platform_device(dev);
	const struct drm_driver *driver;
	struct rpi_firmware *firmware = NULL;
	struct drm_device *drm;
	struct vc4_dev *vc4;
	struct device_node *node;
	struct drm_crtc *crtc;
	enum vc4_gen gen;
	int ret = 0;

	/*
	 * DEVIATION (#51): LinuxKPI's struct device has no coherent_dma_mask,
	 * and the mask is not set from here in any case -- the newbus shim
	 * calls linux_dma_priv_init() at attach, which builds the busdma tags
	 * this device actually allocates through. The
	 * dma_set_mask_and_coherent() calls below still run and still widen it
	 * to 36 bits on gen6.
	 */

	gen = (enum vc4_gen)of_device_get_match_data(dev);

	if (gen > VC4_GEN_4)
		driver = &vc5_drm_driver;
	else
		driver = &vc4_drm_driver;

	if (gen >= VC4_GEN_6_C)
		dma_set_mask_and_coherent(dev, DMA_BIT_MASK(36));
	else
		dma_set_mask_and_coherent(dev, DMA_BIT_MASK(32));

	node = of_find_matching_node_and_match(NULL, vc4_dma_range_matches,
					       NULL);
	if (node) {
		ret = of_dma_configure(dev, node, true);
		of_node_put(node);

		if (ret)
			return ret;
	}

	vc4 = devm_drm_dev_alloc(dev, driver, struct vc4_dev, base);
	if (IS_ERR(vc4))
		return PTR_ERR(vc4);
	vc4->gen = gen;
	vc4->dev = dev;

	drm = &vc4->base;
	platform_set_drvdata(pdev, drm);

	if (gen == VC4_GEN_4) {
		ret = drmm_mutex_init(drm, &vc4->bin_bo_lock);
		if (ret)
			goto err;

		ret = vc4_bo_cache_init(drm);
		if (ret)
			goto err;
	}

	ret = drmm_mode_config_init(drm);
	if (ret)
		goto err;

	if (gen == VC4_GEN_4) {
		ret = vc4_gem_init(drm);
		if (ret)
			goto err;
	}

	node = of_find_compatible_node(NULL, NULL, "raspberrypi,bcm2835-firmware");
	if (node) {
		firmware = rpi_firmware_get(node);
		of_node_put(node);

		if (!firmware) {
			ret = -EPROBE_DEFER;
			goto err;
		}
	}

	ret = drm_aperture_remove_framebuffers(driver);
	if (ret)
		goto err;

	/*
	 * DEVIATION (#51): keep the firmware handle.
	 *
	 * vc4->firmware is only ever assigned by vc4_firmware_kms.c, so on the
	 * full KMS path it stayed NULL and vc4_hdmi_fw_get_edid_block() failed
	 * -ENODEV on its first check -- every time. That is why both connectors
	 * came up "connected" with no EDID and no modes at all:
	 *
	 *	"EDID" (immutable): blob = 0
	 *	(no Modes section)
	 *
	 * from drm_info on the running system. No modes means fbdev has nothing
	 * to pick, so nothing is ever programmed and the screen keeps whatever
	 * the firmware left. The EDID has to come from the mailbox here,
	 * because there is no DDC adapter on this board.
	 *
	 * The reference is deliberately NOT put: the mailbox is needed for as
	 * long as connectors can be probed, which is the life of the driver.
	 */
	if (firmware) {
		vc4->firmware = firmware;


		if (!firmware_kms() && notify_display_done) {
			ret = rpi_firmware_property(firmware,
						    RPI_FIRMWARE_NOTIFY_DISPLAY_DONE,
						    NULL, 0);
			if (ret)
				drm_warn(drm, "Couldn't stop firmware display driver: %d\n", ret);
		} else if (!firmware_kms()) {
			drm_info(drm, "NOT sending NOTIFY_DISPLAY_DONE (#51)\n");
		}
	}



	ret = component_bind_all(dev, drm);
	if (ret)
		goto err;

	ret = devm_add_action_or_reset(dev, vc4_component_unbind_all, vc4);
	if (ret)
		goto err;

	if (!vc4->firmware_kms) {
		ret = vc4_plane_create_additional_planes(drm);
		if (ret)
			goto err;
	}

	ret = vc4_kms_load(drm);
	if (ret < 0)
		goto err;

	if (!vc4->firmware_kms) {
		drm_for_each_crtc(crtc, drm)
			vc4_crtc_disable_at_boot(crtc);
	}

	ret = drm_dev_register(drm, 0);
	if (ret < 0)
		goto err;

	if (enable_fbdev) {
		struct drm_connector_list_iter conn_iter;
		struct drm_connector *conn;

		drm_fbdev_dma_setup(drm, 16);

		/*
		 * DEVIATION (#51): probe the connectors explicitly.
		 *
		 * Nothing else does. The poll worker only calls detect(), and
		 * raises a hotplug event on a CHANGE of status -- the status
		 * here is "connected" from the first probe onwards, so no
		 * event is ever raised. Measured: vc4_hdmi_read_edid() is
		 * called over and over by the poll's detect path, while
		 * vc4_hdmi_connector_get_modes() is never called once.
		 *
		 * get_modes() is the only thing that turns an EDID into modes,
		 * and it runs from drm_helper_probe_single_connector_modes().
		 * Without this the connector holds a complete EDID -- the
		 * panel is identified by name -- and still has no modes, so
		 * the CRTC is never programmed (ACTIVE = 0, MODE_ID = 0).
		 */
		mutex_lock(&drm->mode_config.mutex);
		drm_connector_list_iter_begin(drm, &conn_iter);
		drm_for_each_connector_iter(conn, &conn_iter)
			(void)drm_helper_probe_single_connector_modes(conn,
			    4096, 4096);
		drm_connector_list_iter_end(&conn_iter);
		mutex_unlock(&drm->mode_config.mutex);

		/*
		 * DEVIATION (#51): kick the clients once, after setup.
		 *
		 * drm_fbdev_dma_setup() configures from whatever modes the
		 * connectors have at that instant, and here they have none:
		 * the first successful EDID read does not happen during bind
		 * at all, it happens later on the connector poll. Measured --
		 * no vc4_hdmi_fw_get_edid_block() call appears in the log
		 * until seconds after the load completes.
		 *
		 * By the time the EDID arrives the connector is already
		 * "connected", so the poll sees no change in status, raises no
		 * hotplug event, and nothing ever asks fbdev to reconsider.
		 * The result is a connector with a full EDID and a CRTC that
		 * was never programmed: ACTIVE = 0, MODE_ID = 0.
		 *
		 * One explicit hotplug event makes the client re-probe now
		 * that the firmware can answer.
		 */
		drm_kms_helper_hotplug_event(drm);
	} else
		drm_info(drm, "fbdev emulation off by request (#51)\n");

	return 0;

err:
	platform_set_drvdata(pdev, NULL);
	return ret;
}

static void vc4_drm_unbind(struct device *dev)
{
	struct drm_device *drm = dev_get_drvdata(dev);

	drm_dev_unplug(drm);
	drm_atomic_helper_shutdown(drm);
	dev_set_drvdata(dev, NULL);
}

static const struct component_master_ops vc4_drm_ops = {
	.bind = vc4_drm_bind,
	.unbind = vc4_drm_unbind,
};

/*
 * This list determines the binding order of our components, and we have
 * a few constraints:
 *   - The TXP driver needs to be bound before the PixelValves (CRTC)
 *     but after the HVS to set the possible_crtc field properly
 *   - The HDMI driver needs to be bound after the HVS so that we can
 *     lookup the HVS maximum core clock rate and figure out if we
 *     support 4kp60 or not.
 */
/*
 * DEVIATION (nextbsd-kernel-extensions-graphics#51). Upstream lists nine drivers here;
 * this build carries the four that can match on bcm2712.
 *
 * vc4_vec (composite), vc4_dpi (parallel) and vc4_dsi have no bcm2712
 * compatible in their match tables at all, so they can never bind on this SoC.
 * vc4_v3d is the GEN_4 render engine -- on vc6 the 3D core is driven by the
 * separate drm/v3d driver, not from here. vc4_txp matches mop/moplet, which do
 * exist on 2712, but it provides the writeback connector, which HDMI output
 * does not depend on; it can be added back when writeback is wanted.
 * vc4_firmware_kms belongs to the firmware KMS module, which is a separate
 * kmod and a different master.
 *
 * Order matters and is preserved: this array is the master's match list, and
 * component_bind_all() binds in match-list order (nextbsd-kernel#199). The
 * HDMI encoders MUST be registered before the pixelvalves, or
 * vc4_set_crtc_possible_masks() leaves possible_crtcs == 0 on every encoder
 * and no modeset can succeed.
 */
static struct platform_driver *const component_drivers[] = {
	&vc4_hvs_driver,
	&vc4_hdmi_driver,
	&vc4_crtc_driver,
};

static int vc4_platform_drm_probe(struct platform_device *pdev)
{
	struct component_match *match = NULL;
	struct device *dev = &pdev->dev;

	vc4_match_add_drivers(dev, &match,
			      component_drivers, ARRAY_SIZE(component_drivers));

	return component_master_add_with_match(dev, &vc4_drm_ops, match);
}

static void vc4_platform_drm_remove(struct platform_device *pdev)
{
	component_master_del(&pdev->dev, &vc4_drm_ops);
}

static void vc4_platform_drm_shutdown(struct platform_device *pdev)
{
	drm_atomic_helper_shutdown(platform_get_drvdata(pdev));
}

static const struct of_device_id vc4_of_match[] = {
	{ .compatible = "brcm,bcm2711-vc5", .data = (void *)VC4_GEN_5 },
	/* NB GEN_6_C will be corrected on D0 hw to GEN_6_D via vc4_hvs_bind */
	{ .compatible = "brcm,bcm2712-vc6", .data = (void *)VC4_GEN_6_C },
	{ .compatible = "brcm,bcm2835-vc4", .data = (void *)VC4_GEN_4 },
	{ .compatible = "brcm,cygnus-vc4", .data = (void *)VC4_GEN_4 },
	{},
};
MODULE_DEVICE_TABLE(of, vc4_of_match);

/*
 * NEXTBSD DEVIATION from the vendored source (#51): `static` dropped.
 *
 * Every other vc4 platform_driver is already non-static upstream, which is how
 * the newbus shims reach their .probe without touching those files. This one
 * is not, and the newbus master needs it for the same reason -- LinuxKPI has
 * no platform bus, so something has to call the probe.
 *
 * Kept to one keyword deliberately. The alternative was duplicating
 * vc4_platform_drm_probe() and vc4_drm_ops into the master, which would mean
 * maintaining a copy of upstream logic that changes.
 */
struct platform_driver vc4_platform_driver = {
	.probe		= vc4_platform_drm_probe,
	.remove_new	= vc4_platform_drm_remove,
	.shutdown	= vc4_platform_drm_shutdown,
	.driver		= {
		.name	= "vc4-drm",
		.of_match_table = vc4_of_match,
	},
};

static int __init vc4_drm_register(void)
{
	int ret;

	if (drm_firmware_drivers_only())
		return -ENODEV;

	ret = platform_register_drivers(component_drivers,
					ARRAY_SIZE(component_drivers));
	if (ret)
		return ret;

	ret = platform_driver_register(&vc4_platform_driver);
	if (ret)
		platform_unregister_drivers(component_drivers,
					    ARRAY_SIZE(component_drivers));

	return ret;
}

static void __exit vc4_drm_unregister(void)
{
	platform_unregister_drivers(component_drivers,
				    ARRAY_SIZE(component_drivers));
	platform_driver_unregister(&vc4_platform_driver);
}

module_init(vc4_drm_register);
module_exit(vc4_drm_unregister);

MODULE_ALIAS("platform:vc4-drm");
MODULE_SOFTDEP("pre: snd-soc-hdmi-codec");
MODULE_DESCRIPTION("Broadcom VC4 DRM Driver");
MODULE_AUTHOR("Eric Anholt <eric@anholt.net>");
MODULE_LICENSE("GPL v2");
