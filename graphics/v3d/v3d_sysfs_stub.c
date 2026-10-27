/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * v3d_sysfs.c stand-ins (nextbsd-kernel-extensions-graphics#66).
 *
 * Upstream v3d_sysfs.c exports one attribute group: per-queue GPU usage
 * counters under /sys/devices/.../gpu_stats. It is built on sysfs_create_group()
 * and struct device_attribute, neither of which LinuxKPI provides in a form
 * that would put files anywhere a FreeBSD userland looks.
 *
 * So the file is not vendored, and these stand in for the two entry points
 * v3d_drv.c calls unconditionally -- they are NOT inside a CONFIG_ guard the
 * way .debugfs_init is, so leaving them undefined would be a link error rather
 * than a compiled-out call.
 *
 * v3d_sysfs_init() returning 0 is correct rather than convenient: its failure
 * path in probe unwinds the whole driver, and there is nothing here that can
 * fail. The statistics themselves are still gathered by v3d_sched.c -- only
 * the reporting surface is missing, and a sysctl for it is the obvious follow-up
 * once the driver runs.
 */

#include <linux/device.h>
#include <linux/errno.h>

#include "v3d_drv.h"

int
v3d_sysfs_init(struct device *dev)
{

	return (0);
}

void
v3d_sysfs_destroy(struct device *dev)
{
}
