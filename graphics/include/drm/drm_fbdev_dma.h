/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * drm/drm_fbdev_dma.h (nextbsd-kernel-extensions-graphics#51, #55).
 *
 * drm_fbdev_dma_setup() sets up fbdev emulation so the console can draw on the
 * KMS device. This used to be a no-op here, and the consequence was exactly
 * what the old comment warned about: the driver would bind, disable the
 * display the firmware had been driving, and put nothing back. A load left the
 * screen dark until some other KMS client -- an X server, or a test program --
 * programmed a mode by hand.
 *
 * drm-kmod does not carry the DMA variant, but it does carry the generic one.
 * drm_fbdev_ttm_setup() is misleadingly named: it was drm_fbdev_generic_setup()
 * until Linux 6.11 and is not TTM specific. It allocates through the dumb
 * buffer interface, which vc4 supports, and drm-kmod exports it with
 * CONFIG_DRM_FBDEV_EMULATION enabled -- the symbol is present in the built drm
 * core.
 *
 * THE TRADE, stated rather than hidden: the generic path keeps a shadow buffer
 * and blits damaged regions, where upstream's DMA path maps the scanout buffer
 * directly and needs no copy. So this is correct but does more work per update
 * than vc4 strictly requires. Vendoring drm_fbdev_dma.c would remove the copy;
 * it is a performance change, not a correctness one, and is not worth doing
 * before there is a console to measure.
 */
#ifndef _LINUXKPI_DRM_DRM_FBDEV_DMA_H_
#define	_LINUXKPI_DRM_DRM_FBDEV_DMA_H_

#include <linux/types.h>

#include <drm/drm_fbdev_ttm.h>

struct drm_device;

static inline void
drm_fbdev_dma_setup(struct drm_device *dev, unsigned int preferred_bpp)
{

	drm_fbdev_ttm_setup(dev, preferred_bpp);
}

#endif /* _LINUXKPI_DRM_DRM_FBDEV_DMA_H_ */
