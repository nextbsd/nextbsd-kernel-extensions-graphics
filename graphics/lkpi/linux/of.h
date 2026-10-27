/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2023 Serenity Cyber Security, LLC.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _LINUXKPI_LINUX_OF_H
#define	_LINUXKPI_LINUX_OF_H


/*
 * MODULE-LOCAL LinuxKPI (nextbsd-kernel-extensions-graphics#51).
 *
 * This header shadows the kernel's <linux/of.h>. Every drm-kmod module is
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
#define	of_property_read_bool	LKPI_SYM(of_property_read_bool)
#define	of_find_i2c_adapter_by_node	LKPI_SYM(of_find_i2c_adapter_by_node)
#define	of_find_property	LKPI_SYM(of_find_property)
#define	of_property_match_string	LKPI_SYM(of_property_match_string)
#define	lkpi_of_match_table	LKPI_SYM(lkpi_of_match_table)
#define	dev_of_node	LKPI_SYM(dev_of_node)
#define	lkpi_set_of_node	LKPI_SYM(lkpi_set_of_node)
#define	lkpi_clear_of_node	LKPI_SYM(lkpi_clear_of_node)

#define	of_match_device	LKPI_SYM(of_match_device)
#define	of_parse_phandle	LKPI_SYM(of_parse_phandle)
#define	of_node_put	LKPI_SYM(of_node_put)
#define	of_node_get	LKPI_SYM(of_node_get)
#define	of_find_compatible_node	LKPI_SYM(of_find_compatible_node)
#define	of_find_matching_node_and_match	LKPI_SYM(of_find_matching_node_and_match)
#define	of_device_is_available	LKPI_SYM(of_device_is_available)
#define	of_device_is_compatible	LKPI_SYM(of_device_is_compatible)
#define	of_device_get_match_data	LKPI_SYM(of_device_get_match_data)
#define	of_dma_configure	LKPI_SYM(of_dma_configure)
#include <linux/kobject.h>
#include <linux/types.h>

/*
 * nextbsd-kernel#176: enough of the Linux device-tree model to carry an FDT
 * node through a LinuxKPI driver.
 *
 * struct device_node wraps a FreeBSD phandle_t. It is declared here as an
 * intptr_t rather than including <dev/ofw/openfirm.h>, so this header stays
 * usable on platforms built without FDT -- the implementation in
 * linux_of.c is what knows about OFW, and it compiles to stubs when FDT is
 * not configured.
 */
struct device_node {
	intptr_t	node;		/* phandle_t where FDT is present */
};

struct of_device_id {
	char		compatible[128];
	const void	*data;
};

struct device;

const struct of_device_id *of_match_device(const struct of_device_id *matches,
    const struct device *dev);
struct device_node *of_parse_phandle(const struct device_node *np,
    const char *name, int index);
struct device_node *of_node_get(struct device_node *np);
void of_node_put(struct device_node *np);

/*
 * Node lookup and property queries for the full vc4 KMS pipeline (#51).
 * These walk the live OF tree rather than a driver's own node, which is what a
 * display driver needs to find the HVS and HDMI controllers from a CRTC.
 *
 * Each returns a node the caller owns and must release with of_node_put().
 */
struct device_node *of_find_compatible_node(struct device_node *from,
    const char *type, const char *compat);
struct device_node *of_find_matching_node_and_match(struct device_node *from,
    const struct of_device_id *matches, const struct of_device_id **match);
const void *of_device_get_match_data(const struct device *dev);
bool	of_device_is_available(const struct device_node *np);
bool	of_device_is_compatible(const struct device_node *np, const char *compat);

/*
 * of_dma_configure() sets a device's DMA parameters from its device-tree node.
 * LinuxKPI derives no DMA tag from the tree and linux_dma_priv_init() already
 * establishes the masks a DRM driver needs, so this succeeds without acting.
 *
 * Honest on a SoC where the GPU sees the same physical addresses the CPU does
 * -- BCM2712 has no IOMMU and no DMA offset. A platform with either would be
 * given wrong addresses by this and needs real dma-ranges parsing.
 */
int	of_dma_configure(struct device *dev, struct device_node *np,
	    bool force_dma);


/*
 * dev_of_node() -- the device-tree node a device came from.
 *
 * Upstream Linux keeps this in struct device as dev->of_node, and we used to
 * do the same: kernel patch 0040 inserted the field into LinuxKPI's struct
 * device. That was a mistake with real consequences. The insert was
 * mid-struct, so devt, class, release, kobj, dma_priv and irq all shifted by
 * eight bytes -- and because struct pci_dev embeds struct device first, every
 * pci_dev member shifted too. Any module built before the change read its
 * fields eight bytes low. A Dell Wyse 5070 page-faulted in device_attach()
 * loading i915kms with a kernel 22 hours newer than its kexts
 * (gershwin-desktop#49). Nothing about that failure involved vc4.
 *
 * So the field is gone from the kernel and the mapping lives here instead,
 * private to this module. dev_of_node() IS a real upstream accessor
 * (include/linux/device.h), so vendored sources using it rather than the bare
 * field is idiomatic, not a deviation.
 *
 * The table is tiny and set up at attach: a vc4 pipeline is five devices.
 */
struct device_node	*dev_of_node(struct device *dev);
void			 lkpi_set_of_node(struct device *dev,
			     struct device_node *node,
			     const struct of_device_id *match);
const struct of_device_id *lkpi_of_match_table(struct device *dev);
void			 lkpi_clear_of_node(struct device *dev);


/*
 * Two more property helpers vc4_hdmi wants (#51). Both answer from the FDT
 * node the shim registered; both are safe to call with a NULL node, which is
 * what a non-FDT build gets.
 */
struct property;
struct property *of_find_property(const struct device_node *np,
	    const char *name, int *lenp);
int	 of_property_match_string(const struct device_node *np,
	    const char *propname, const char *string);


/*
 * The I2C adapter behind a device-tree node. See lkpi_of.c: this returns NULL
 * on bcm2712 because the DDC controller ("brcm,brcmstb-i2c") has no FreeBSD
 * driver, which makes vc4_hdmi defer. Declared so the call compiles and the
 * limitation is visible rather than a link error.
 */
struct i2c_adapter;
struct i2c_adapter *of_find_i2c_adapter_by_node(struct device_node *np);


/* Is the (valueless) property present? */
bool	of_property_read_bool(const struct device_node *np, const char *name);

#endif
