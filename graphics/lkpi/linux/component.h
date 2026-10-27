/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * The component framework, in the subset a single-device driver needs
 * (nextbsd-kernel#176).
 *
 * Linux uses this to defer binding until every sub-device of a multi-part
 * device has registered, then bind them together as one logical device. A
 * component calls component_add() from its probe and does nothing else; the
 * master calls component_bind_all() once it is ready.
 *
 * That is exactly the shape this implements, and no more: no match arrays, no
 * aggregate driver, no deferred probe. A driver whose parts all attach from
 * one FreeBSD device tree node has nothing to wait for, so the master drives
 * the sequence directly.
 *
 * Note there is a zero-length linux/component.h in compat/linuxkpi/dummy,
 * which resolves silently for anything that only needs the include to exist.
 * The common tree comes first in the include path, so this file wins for
 * anything that actually calls these.
 */
#ifndef	_LINUXKPI_LINUX_COMPONENT_H_
#define	_LINUXKPI_LINUX_COMPONENT_H_


/*
 * MODULE-LOCAL LinuxKPI (nextbsd-kernel-extensions-graphics#51).
 *
 * This header shadows the kernel's <linux/component.h>. Every drm-kmod module is
 * built with its own include paths ahead of the kernel's:
 *
 *	-I linuxkpi/gplv2/include			drm-kmod's own KPI
 *	-I linuxkpi/bsd/include				drm-kmod's own KPI
 *	-I ${SYSDIR}/compat/linuxkpi/common/include	the kernel's
 *
 * and this module puts -I${.CURDIR}/lkpi ahead of all of them, so what
 * follows is seen ONLY by objects compiled into this module. drm.ko, i915,
 * amdgpu and the kernel itself are untouched.
 *
 * That isolation is the entire point. Eleven of the kernel's LinuxKPI patches
 * touched shared headers and both regressions we have had came from those
 * eleven -- an include collision that broke the whole amdgpu build, and a
 * struct device field that broke KBI for every already-built module. This
 * code carries the same risk and none of the blast radius.
 *
 * The kernel still exports its own component_add(), of_match_device() and so
 * on, from patch 0040, which firmware KMS runs on. Ours would collide at load
 * time, so every function this module provides is renamed to vc4lkpi_* below
 * and the Linux spelling is #defined onto it. Vendored sources keep calling
 * the Linux names; the preprocessor renames the definitions in our .c files
 * for free, because they include this header too.
 */


/*
 * Symbols are prefixed PER MODULE.
 *
 * Both vc4_fkms and vc4_kms compile this code, and both are built with
 * EXPORT_SYMS=YES, so a fixed prefix would collide the moment the second one
 * loaded. LKPI_PFX comes from each Makefile (-DLKPI_PFX=vc4kms_), so each
 * module carries its own copy under its own names. They are also independent
 * at runtime -- separate component lists, separate masters -- which is what we
 * want: one driver's bind cannot disturb the other's.
 */
#ifndef LKPI_PFX
#error "LKPI_PFX must be defined by the module Makefile"
#endif
#define	LKPI_SYM2(p, n)	p ## n
#define	LKPI_SYM1(p, n)	LKPI_SYM2(p, n)
#define	LKPI_SYM(n)	LKPI_SYM1(LKPI_PFX, n)

#define	component_add	LKPI_SYM(component_add)
#define	component_del	LKPI_SYM(component_del)
#define	component_bind_all	LKPI_SYM(component_bind_all)
#define	component_unbind_all	LKPI_SYM(component_unbind_all)
#define	component_match_add	LKPI_SYM(component_match_add)
#define	component_master_add_with_match	LKPI_SYM(component_master_add_with_match)
#define	component_master_del	LKPI_SYM(component_master_del)
#define	component_compare_dev	LKPI_SYM(component_compare_dev)
#include <linux/device.h>

struct component_ops {
	int	(*bind)(struct device *comp, struct device *master,
		    void *master_data);
	void	(*unbind)(struct device *comp, struct device *master,
		    void *master_data);
};

int	component_add(struct device *dev, const struct component_ops *ops);
void	component_del(struct device *dev, const struct component_ops *ops);
int	component_bind_all(struct device *master, void *master_data);
void	component_unbind_all(struct device *master, void *master_data);

/*
 * Match-based masters (nextbsd-kernel-extensions-graphics#51).
 *
 * The simple form above binds every registered component to whichever master
 * asks. That is right for a driver with one component -- firmware KMS -- and
 * wrong for full vc4, which has several masters and expects each to bind only
 * the components it named.
 *
 * A master therefore builds a match list, and binds when every entry in it is
 * present. `struct component_match` is opaque to callers, exactly as in Linux.
 */
struct component_match;

struct component_master_ops {
	int	(*bind)(struct device *master);
	void	(*unbind)(struct device *master);
};

/*
 * Add one entry. `compare` decides whether a registered component's device is
 * the one wanted; `data` is passed to it. Linux's helper for the common case
 * is component_compare_dev, comparing the device pointer itself.
 *
 * Allocates the match structure on first call, so `*match` may start NULL.
 */
void	component_match_add(struct device *master, struct component_match **match,
	    int (*compare)(struct device *, void *), void *data);

int	component_compare_dev(struct device *dev, void *data);

int	component_master_add_with_match(struct device *master,
	    const struct component_master_ops *ops, struct component_match *match);
void	component_master_del(struct device *master,
	    const struct component_master_ops *ops);

#endif	/* _LINUXKPI_LINUX_COMPONENT_H_ */
