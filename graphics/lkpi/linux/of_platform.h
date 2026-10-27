/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * <linux/of_platform.h> (nextbsd-kernel-extensions-graphics#66).
 *
 * v3d_drv.c includes this header, and nothing in this tree provided it. It is
 * the only header v3d names that did not already exist, and the module would
 * not compile a single file without it -- a fatal error stops the translation
 * unit before its body is parsed, so it also hid whatever else v3d_drv.c has
 * to say.
 *
 * Upstream this header declares the of_platform_* population helpers, which
 * walk a device-tree subtree and create a platform_device per child. Nothing
 * here needs them: on FreeBSD newbus already does that enumeration, and the
 * newbus shim attaches the vendored driver directly.
 *
 * What v3d actually uses out of it is of_device_get_match_data(), of_node_put()
 * and struct of_device_id -- all of which come from <linux/of.h> and
 * <linux/platform_device.h>, both already included by v3d_drv.c and both
 * already provided next to this file. So this header exists to satisfy the
 * include and pull in the two that carry the declarations, rather than to add
 * anything of its own.
 *
 * Deliberately NOT declaring of_platform_populate() and friends as stubs: a
 * driver that called one and silently got nothing would be far harder to
 * diagnose than one that fails to link.
 */
#ifndef _LKPI_LINUX_OF_PLATFORM_H_
#define _LKPI_LINUX_OF_PLATFORM_H_

#include <linux/of.h>
#include <linux/platform_device.h>

#endif /* _LKPI_LINUX_OF_PLATFORM_H_ */
