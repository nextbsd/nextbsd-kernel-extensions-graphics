/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/reset.h (nextbsd-kernel-extensions-graphics#51).
 *
 * Real resets now, not stubs.
 *
 * This file used to return -ENOENT from devm_reset_control_get() and describe
 * the consequence honestly: "the driver skips the reset ... a part that
 * genuinely needs an explicit reset would come up in an undefined state". That
 * turned out to be exactly what happened. With clocks working, the HDMI block
 * accepted register writes, never became ready, and produced no signal:
 *
 *	vc40: [drm] *ERROR* Failed to wait for infoframe to go idle: -60
 *
 * ETIMEDOUT on a Pi 500+, with the panel's backlight going out and staying
 * out. hdmi@7ef00700 has resets = <&dvp 1> and dvp had no driver, so nothing
 * could take it out of reset.
 *
 * These map onto FreeBSD's hwreset framework, which resolves the same device
 * tree property, so bcm_dvp(4) is what actually answers.
 */
#ifndef _LINUXKPI_LINUX_RESET_H_
#define	_LINUXKPI_LINUX_RESET_H_

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/bus.h>

#include <dev/hwreset/hwreset.h>
#include <dev/ofw/ofw_bus.h>

#include <linux/err.h>
#include <linux/device.h>

struct reset_control;

/*
 * A hwreset_t is the handle; the cast keeps the Linux-shaped API without a
 * wrapper allocation. NULL stays NULL, which is what the optional form needs.
 */
static inline struct reset_control *
lkpi_reset_control_get(struct device *dev, const char *id, bool optional)
{
	hwreset_t rst;
	int error;

	if (dev == NULL || dev->bsddev == NULL)
		return (optional ? NULL : ERR_PTR(-ENODEV));

	if (id == NULL)
		error = hwreset_get_by_ofw_idx(dev->bsddev,
		    ofw_bus_get_node(dev->bsddev), 0, &rst);
	else
		/*
		 * __DECONST because hwreset_get_by_ofw_name() takes a
		 * char *, while every Linux caller passes a string literal
		 * through a const char *. It only reads the name.
		 */
		error = hwreset_get_by_ofw_name(dev->bsddev,
		    ofw_bus_get_node(dev->bsddev), __DECONST(char *, id),
		    &rst);
	if (error != 0)
		return (optional ? NULL : ERR_PTR(-error));

	return ((struct reset_control *)rst);
}

static inline struct reset_control *
devm_reset_control_get(struct device *dev, const char *id)
{

	return (lkpi_reset_control_get(dev, id, false));
}

/*
 * The "exclusive" spelling (v3d, #66). Upstream distinguishes exclusive from
 * shared resets so two drivers cannot both drive one line; FreeBSD's hwreset
 * has no shared mode at all, so every reset here is already exclusive and this
 * is the same call under the name v3d asks for.
 */
static inline struct reset_control *
devm_reset_control_get_exclusive(struct device *dev, const char *id)
{

	return (lkpi_reset_control_get(dev, id, false));
}

static inline struct reset_control *
devm_reset_control_get_optional(struct device *dev, const char *id)
{

	return (lkpi_reset_control_get(dev, id, true));
}

static inline int
reset_control_assert(struct reset_control *rstc)
{

	if (rstc == NULL)
		return (0);
	return (-hwreset_assert((hwreset_t)rstc));
}

static inline int
reset_control_deassert(struct reset_control *rstc)
{

	if (rstc == NULL)
		return (0);
	return (-hwreset_deassert((hwreset_t)rstc));
}

/*
 * Linux's reset_control_reset() is a pulse: assert, then release. FreeBSD has
 * no single call for it, so it is spelled out. The delay is deliberate -- a
 * reset that is released in the same instant it is asserted is not a reset --
 * and 10us is what Linux's own drivers use for this block class.
 */
static inline int
reset_control_reset(struct reset_control *rstc)
{
	int error;

	if (rstc == NULL)
		return (0);

	error = hwreset_assert((hwreset_t)rstc);
	if (error != 0)
		return (-error);
	DELAY(10);
	error = hwreset_deassert((hwreset_t)rstc);
	return (-error);
}

static inline void
reset_control_put(struct reset_control *rstc)
{

	if (rstc == NULL)
		return;
	hwreset_release((hwreset_t)rstc);
}

#endif /* _LINUXKPI_LINUX_RESET_H_ */
