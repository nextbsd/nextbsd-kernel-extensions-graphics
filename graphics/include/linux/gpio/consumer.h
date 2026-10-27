/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/gpio/consumer.h (nextbsd-kernel-extensions-graphics#51).
 *
 * vc4_hdmi.c uses two symbols, both for the HDMI hotplug-detect line on parts
 * where HPD is wired to a GPIO rather than read from the HDMI block.
 *
 * These return "no GPIO present", which makes the driver fall back to reading
 * hotplug from the controller -- the path a Pi 5 takes anyway, since 2712
 * routes HPD through the HDMI block. So this is honest for the hardware in
 * question and NOT a general GPIO implementation.
 *
 * A board that really does wire HPD to a GPIO would silently never detect a
 * display with these stubs. If that turns up, this file is where a real
 * consumer over FreeBSD's gpio(4) belongs -- like linux/clk.h before it, the
 * emptiness here is a measurement of what BCM2712 needs, not an oversight.
 */
#ifndef _LINUXKPI_LINUX_GPIO_CONSUMER_H_
#define	_LINUXKPI_LINUX_GPIO_CONSUMER_H_

#include <linux/err.h>
#include <linux/device.h>

struct gpio_desc;

enum gpiod_flags {
	GPIOD_ASIS,
	GPIOD_IN,
	GPIOD_OUT_LOW,
	GPIOD_OUT_HIGH,
};

/*
 * "optional" in Linux means a missing GPIO is not an error: return NULL and
 * let the caller carry on. That is exactly the contract this needs.
 */
static inline struct gpio_desc *
devm_gpiod_get_optional(struct device *dev, const char *con_id,
    enum gpiod_flags flags)
{

	return (NULL);
}

static inline int
gpiod_get_value_cansleep(const struct gpio_desc *desc)
{

	/* No descriptor can exist, since the getter above never returns one. */
	return (0);
}

#endif /* _LINUXKPI_LINUX_GPIO_CONSUMER_H_ */
