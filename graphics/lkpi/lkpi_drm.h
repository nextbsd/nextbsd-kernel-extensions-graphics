/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * DRM helpers vc4 needs that drm-kmod does not provide
 * (nextbsd-kernel-extensions-graphics#51).
 *
 * FORCE-INCLUDED, not a shadowing header. The Makefile adds
 *
 *	CFLAGS+= -include ${.CURDIR:H}/lkpi/lkpi_drm.h
 *
 * so this is pulled in ahead of every source in the module, and it includes
 * the real <drm/drm_managed.h> and <linux/dma-fence.h> itself before adding
 * what is missing.
 *
 * That is deliberate. The obvious alternative -- shipping our own
 * <linux/dma-fence.h> in lkpi/linux/ -- would shadow drm-kmod's copy of the
 * same header for this module and silently drop whatever drm-kmod had added
 * to it. That mistake broke `drm-kmod build` on both arches earlier in #51,
 * when lkpi/linux/{dma-mapping,mm,iosys-map}.h shadowed drm-kmod's versions.
 * The rule that came out of it: only shadow a header whose consumers are all
 * inside this module. dma-fence.h and drm_managed.h are drm core; they are
 * not.
 *
 * The earlier attempt at this (closed PR #56) patched drm-kmod instead. Two of
 * its three pieces did not need to -- they add functions, not struct members --
 * and those two are here.
 */
#ifndef _LKPI_DRM_H_
#define	_LKPI_DRM_H_

#ifndef LKPI_PFX
#error "LKPI_PFX must be defined by the module Makefile"
#endif
#define	LKPI_DRM_SYM2(p, n)	p ## n
#define	LKPI_DRM_SYM1(p, n)	LKPI_DRM_SYM2(p, n)
#define	LKPI_DRM_SYM(n)		LKPI_DRM_SYM1(LKPI_PFX, n)

#define	drm_print_regset32	LKPI_DRM_SYM(drm_print_regset32)
#define	lkpi_devm_ioremap	LKPI_DRM_SYM(lkpi_devm_ioremap)
#define	drmm_mutex_init		LKPI_DRM_SYM(drmm_mutex_init)
#define	dma_fence_match_context	LKPI_DRM_SYM(dma_fence_match_context)

#include <linux/types.h>
#include <linux/dma-fence.h>
#include <drm/drm_managed.h>


/*
 * Pixel formats the Raspberry Pi tree adds and drm-kmod does not carry.
 *
 * All are 4:2:0 YCbCr the HVS on gen6 can scan out directly, used only by
 * hvs6_only entries in vc4_plane.c's hvs_formats[]. Missing, they take the
 * whole table's initialiser with them -- which is where the "incomplete type
 * 'const struct hvs_format[]'" errors came from, rather than anything wrong
 * with the table itself.
 *
 * fourcc_code() is the standard encoding; these follow it exactly, so the
 * values match upstream and a buffer negotiated with a Linux client agrees.
 * Guarded in case drm-kmod picks them up later.
 */
#include <drm/drm_fourcc.h>

#ifndef DRM_FORMAT_P030
/* 2-plane 10-bit 4:2:0, 3 pixels packed per 32 bits */
#define	DRM_FORMAT_P030		fourcc_code('P', '0', '3', '0')
#endif
#ifndef DRM_FORMAT_S010
/* 3-plane 10-bit 4:2:0, samples in the low bits */
#define	DRM_FORMAT_S010		fourcc_code('S', '0', '1', '0')
#endif
#ifndef DRM_FORMAT_S012
/* 3-plane 12-bit 4:2:0 */
#define	DRM_FORMAT_S012		fourcc_code('S', '0', '1', '2')
#endif
#ifndef DRM_FORMAT_S016
/* 3-plane 16-bit 4:2:0 */
#define	DRM_FORMAT_S016		fourcc_code('S', '0', '1', '6')
#endif


/*
 * struct cec_msg (#51).
 *
 * LinuxKPI's <media/cec.h> is a passthrough that never defines it, and vc4_hdmi
 * embeds one BY VALUE (vc4_hdmi.h: struct cec_msg cec_rx_msg), so struct
 * vc4_hdmi is incomplete without it and every file touching a vc4_hdmi fails.
 *
 * All CEC *code* in vc4_hdmi.c is already behind CONFIG_DRM_VC4_HDMI_CEC,
 * which this module does not define -- so nothing here is ever read or
 * transmitted. Only the field's size matters, and it matters only to the
 * compiler. Laid out as upstream's uapi struct so it stays recognisable if CEC
 * is ever wired up.
 */
#ifndef CEC_MAX_MSG_SIZE
#define	CEC_MAX_MSG_SIZE	16
#endif

struct cec_msg {
	uint64_t	tx_ts;
	uint64_t	rx_ts;
	uint32_t	len;
	uint32_t	timeout;
	uint32_t	sequence;
	uint32_t	flags;
	uint8_t		msg[CEC_MAX_MSG_SIZE];
	uint8_t		reply;
	uint8_t		rx_status;
	uint8_t		tx_status;
	uint8_t		tx_arb_lost_cnt;
	uint8_t		tx_nack_cnt;
	uint8_t		tx_low_drive_cnt;
	uint8_t		tx_error_cnt;
};


/*
 * vc4_hdmi_regs.h dereferences hdmi->pdev->dev and calls
 * pm_runtime_status_suspended() in its register accessors, and it is included
 * by files that have not pulled either header in. Including them here -- this
 * header is force-included ahead of everything -- makes struct platform_device
 * complete and the pm_runtime helpers visible everywhere, which is 8 errors
 * from two includes.
 */
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>

/*
 * Module metadata macros. LinuxKPI carries MODULE_DEPEND and friends but not
 * these, and an undefined function-like macro at file scope parses as a
 * declaration -- which is why MODULE_DEVICE_TABLE and MODULE_ALIAS showed up
 * as "type specifier missing" and "expected ')'" rather than as anything
 * recognisable.
 */
/*
 * MODULE_DEVICE_TABLE has to be overridden, not merely supplied: LinuxKPI
 * already defines it, and its definition expands to
 *
 *	DRIVER_MODULE(lkpi_<table>, <bus>, ...)
 *
 * which manufactures a newbus driver attached to a bus named by the first
 * argument. That works for "pci"; vc4 passes "of", which is not a newbus bus
 * here, and the expansion lands as a malformed declaration -- reported as
 * "type specifier missing" on the MODULE_DEVICE_TABLE line itself.
 *
 * <linux/module.h> is included first so the #undef has something to remove;
 * this header is force-included, so without that our definition would be
 * replaced by LinuxKPI's the moment vc4_drv.c pulled module.h in.
 *
 * Matching is done by the newbus shims against the driver's own
 * of_match_table, so nothing is lost by dropping the table registration.
 */
#include <linux/module.h>
#undef MODULE_DEVICE_TABLE
#define	MODULE_DEVICE_TABLE(type, name)
#ifndef MODULE_ALIAS
#define	MODULE_ALIAS(x)
#endif
#ifndef MODULE_SOFTDEP
#define	MODULE_SOFTDEP(x)
#endif

/*
 * Request the interrupt but leave it disabled; the driver enables it when it
 * wants it. vc4_hvs uses this for the EOF interrupts, which are only enabled
 * while a channel is running.
 */
#ifndef IRQF_NO_AUTOEN
#define	IRQF_NO_AUTOEN		0
#endif

/*
 * Request a threaded handler that runs once per interrupt with the line
 * masked. LinuxKPI's request_irq() already serialises the thread half this
 * way, so the flag is a no-op rather than unimplemented.
 */
#ifndef IRQF_ONESHOT
#define	IRQF_ONESHOT		0
#endif

/*
 * linuxkpi's module_param() expands to a tunable under sysctl _hw_<module>,
 * and declares the node in no translation unit. vc4_master_newbus.c defines it
 * once with SYSCTL_NODE; every other file that uses module_param() -- which is
 * vc4_hdmi.c -- needs the declaration visible. This header is force-included
 * by vc4_kms and nothing else, so the module name can be spelled here.
 */
#include <sys/sysctl.h>
SYSCTL_DECL(_hw_vc4_kms);

/*
 * <linux/ioport.h> for LinuxKPI's struct resource and resource_size(), which
 * vc4_hdmi uses for its register banks.
 *
 * This is NOT FreeBSD's struct resource from sys/rman.h. They are different
 * types with the same name, and putting one where the other is expected is
 * what broke the entire amdgpu build in nextbsd/nextbsd-kernel#200 -- and then
 * broke this module too, when adding the include here dragged the Linux
 * definition into lkpi_of.c and lkpi_platform.c, which need FreeBSD's.
 *
 * Hence the guard. This header is force-included everywhere, so the few files
 * that talk to newbus and busdma compile with -DLKPI_NO_IOPORT and keep the
 * FreeBSD definition; everything else -- the vendored vc4 sources -- gets the
 * Linux one. A translation unit gets exactly one of the two, never both.
 */
#ifndef LKPI_NO_IOPORT
#include <linux/ioport.h>
#endif

/*
 * No swiotlb on this platform, so no buffer is ever bounced through one and
 * the answer is always "not from a swiotlb pool". vc4_drv.c uses it to reject
 * an imported dma-buf it would otherwise have to bounce.
 */
static inline void *
swiotlb_find_pool(struct device *dev __unused, phys_addr_t paddr __unused)
{

	return (NULL);
}

/*
 * Two CEC calls sit OUTSIDE vc4_hdmi.c's CONFIG_DRM_VC4_HDMI_CEC guards --
 * they are in the hotplug and EDID paths, which run whether or not CEC is
 * built. They tell a CEC adapter the physical address parsed from EDID.
 *
 * With no CEC adapter (see of_find_i2c_adapter_by_node in lkpi_of.c) there is
 * nothing to tell, so these do nothing. Not stubs for something unimplemented:
 * with CEC disabled, doing nothing is the correct behaviour.
 */
struct cec_adapter;
static inline void
cec_s_phys_addr(struct cec_adapter *adap __unused, uint16_t addr __unused,
    bool block __unused)
{
}

static inline void
cec_phys_addr_invalidate(struct cec_adapter *adap __unused)
{
}

/*
 * Register dump for debugfs. drm-kmod does not export it; see lkpi_drm.c for
 * why a no-op is the right answer here.
 */
struct drm_printer;
struct debugfs_regset32;
void	drm_print_regset32(struct drm_printer *p,
	    struct debugfs_regset32 *regset);

/*
 * devm_ioremap() maps registers; LinuxKPI's returns NULL unconditionally, so
 * vc4_hdmi failed to map all nine of its banks and reported a bare -ENOMEM.
 * See lkpi_drm.c. Overridden as a macro because <linux/io.h> defines it as a
 * static inline, and that header is included by drm core -- shadowing the whole
 * header would break the rule this port arrived at the hard way.
 */
#include <linux/io.h>
void	*lkpi_devm_ioremap(struct device *dev, resource_size_t offset,
	    resource_size_t size);
#undef devm_ioremap
#define	devm_ioremap(dev, off, sz)	lkpi_devm_ioremap((dev), (off), (sz))

struct mutex;
struct drm_device;

/*
 * A mutex destroyed when the drm_device is. The point of the drmm_ family is
 * that a driver need not unwind by hand: everything registered is released in
 * reverse order when the device goes, and vc4 uses it for locks that live
 * exactly as long as the device.
 */
int	drmm_mutex_init(struct drm_device *dev, struct mutex *lock);

/*
 * Is every fence here from `context`?
 *
 * vc4 uses it to skip waiting on a fence it produced itself -- a fence from
 * the caller's own context is already ordered by submission order.
 *
 * The array case is what matters for correctness: an array fence whose members
 * span contexts must NOT report a match. Reporting one makes the caller skip a
 * wait it genuinely needs, and that surfaces much later as rendering against a
 * buffer that was not ready.
 */
bool	dma_fence_match_context(struct dma_fence *fence, u64 context);

#endif /* _LKPI_DRM_H_ */
