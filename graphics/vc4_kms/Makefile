# vc4_kms -- the full vc4 KMS display pipeline (nextbsd-kernel-extensions-graphics#51).
#
# This STARTED as a compile probe: something to make clang enumerate what a
# real port was missing, exactly as the #176 probe did. It is no longer that.
# It builds, links, loads as VideoCore6KMS.kext, and brings up hvs, both
# pixelvalves, both HDMI controllers and the vc4 master on a Pi 500+.
#
# Firmware KMS (#44) brought the device up but cannot program a plane: the 2712
# firmware does not service SET_PLANE (#48, measured). This driver programs the
# HVS and HDMI PHY directly instead of asking the firmware, which is the only
# way to get hardware planes and therefore a hardware cursor.
#
# Display-only subset. Deliberately NOT vendored:
#   vc4_v3d.c, vc4_validate*.c, vc4_render_cl.c, vc4_perfmon.c   -- 3D
#   vc4_dsi.c, vc4_dpi.c, vc4_vec.c, vc4_txp.c                   -- non-HDMI outputs
#
# 2712 is supported upstream: vc4_of_match carries "brcm,bcm2712-vc6" and the
# DMA range table "brcm,bcm2712-hvs", so this is the right driver for a Pi 5.
#
# Sources vendored UNMODIFIED from raspberrypi/linux rpi-6.12.y, matching
# drm-kmod's 6.12-lts.

.include "../kconfig.mk"

SRCDIR=	${.CURDIR:H}/drivers/gpu/drm/vc4

.PATH:	${SRCDIR} ${.CURDIR} ${.CURDIR:H}/lkpi

KMOD=	vc4_kms

EXPORT_SYMS=	YES

# Vendored from raspberrypi/linux rpi-6.12.y. One deviation, marked in the
# file: vc4_platform_driver is no longer static, so the newbus master can reach
# its probe -- every other vc4 platform_driver is already non-static upstream.
SRCS=	\
	vc4_drv.c \
	vc4_kms.c \
	vc4_crtc.c \
	vc4_hvs.c \
	vc4_plane.c \
	vc4_hdmi.c \
	vc4_hdmi_phy.c \
	vc4_bo.c

# The newbus shims. LinuxKPI has no platform bus, so each vendored block is
# attached to its device-tree node by a newbus driver that calls the vendored
# probe -- the arrangement vc4_fkms_master.c already uses on hardware.
# Implementations behind the shadowing headers above. Renamed vc4lkpi_* by
# those headers so they cannot collide with the kernel's own component_add()
# and of_match_device(), which firmware KMS still runs on.
SRCS+=	\
	lkpi_component.c \
	lkpi_of.c \
	lkpi_drm.c \
	lkpi_platform.c \
	lkpi_irq.c \
	lkpi_rpi_firmware.c \
	lkpi_rpi_firmware_mbox.c

# The GEN_4 render path is not built -- vc5_drm_driver registers no ioctls and
# no DRIVER_RENDER, so vc4_gem.c/vc4_fence.c and the v3d, validate, render_cl
# and perfmon sources are unreachable on 2712. These stand in for the symbols
# vc4_drv.c still names in that unreachable ioctl table.
SRCS+=	\
	vc4_gen4_render_stubs.c

SRCS+=	\
	vc4_newbus.c \
	vc4_hvs_newbus.c \
	vc4_crtc_newbus.c \
	vc4_hdmi_newbus.c \
	vc4_master_newbus.c

SRCS	+=			\
	opt_acpi.h		\
	opt_drm.h		\
	opt_platform.h		\
	opt_splash.h		\
	opt_syscons.h		\
	opt_teken.h		\
	acpi_if.h		\
	bus_if.h		\
	fb_if.h			\
	device_if.h		\
	pci_if.h		\
	ofw_bus_if.h		\
	vnode_if.h		\
	clknode_if.h

# clknode_if.h is GENERATED from dev/clk/clknode_if.m. linux/clk.h includes
# dev/clk/clk.h, which includes it, so without this every file that touches
# a clock dies at the #include -- and, worse, reports FEWER errors, because
# the translation unit aborts before its body is parsed. Same arrangement as
# sys/modules/neta and sys/modules/vf_i2c.
CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

.include <bsd.kmod.mk>

.include "../linuxkpi_version.mk"
.include "../compiler_flags.mk"

# MODULE-LOCAL LinuxKPI, ahead of drm-kmod's and the kernel's.
#
# lkpi/linux/{component,of,platform_device}.h shadow the kernel's copies for
# objects compiled into THIS module only -- drm.ko, i915, amdgpu and the kernel
# never see them. That is deliberate: the platform-bus, component and OF
# support vc4 needs used to live in sys/compat/linuxkpi/common/include, where
# an include collision broke the entire amdgpu build and a struct field broke
# KBI for every already-built module (nextbsd/nextbsd-kernel#200, #203).
#
# This -I MUST stay first. If it falls below the kernel's include path the
# module silently builds against the kernel's smaller KPI instead.
# PRIVATE LinuxKPI, ahead of drm-kmod's and the kernel's.
#
# ../lkpi/linux/*.h shadow the kernel's copies for objects compiled into THIS
# module only. i915, amdgpu and radeonkms are built in the drm-kmod tree and
# never see this path, so nothing here can reach them -- which is the point.
# These additions used to be kernel patches against
# sys/compat/linuxkpi/common/include, where one of them collided with
# LinuxKPI's struct resource and broke the whole amdgpu build.
#
# LKPI_PFX makes the symbols unique per module, since several modules compile
# this code and all export their symbols.
#
# This -I MUST stay first, or the module silently builds against the kernel's
# smaller KPI instead.
CFLAGS+= -I${.CURDIR:H}/lkpi -DLKPI_PFX=${KMOD}_


CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include

CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${.CURDIR:H}/drivers/gpu -I${SRCDIR}
CFLAGS+= -I${.CURDIR}		# vc4_newbus.h, next to the shims

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=vc4_kms_' -DDRM_SYSCTL_PARAM_PREFIX=_${KMOD}
# CONFIG_DEBUG_FS is filtered out: vc4_debugfs.c is not vendored, so the
# debugfs entry points it defines -- vc4_debugfs_init(), vc4_debugfs_add_regset32()
# -- do not exist, and the guarded calls would link against nothing. drm's
# drm_print_regset32() goes with them. Turning the option off compiles the
# calls out instead, which is what the #ifdef CONFIG_DEBUG_FS guards are for.
CFLAGS+= ${KCONFIG:NDEBUG_FS:C/(.*)/-DCONFIG_\1/}
CFLAGS+= -DCONFIG_RASPBERRYPI_FIRMWARE=1
CFLAGS+= -DCONFIG_MMU=1

# FreeBSD's LinuxKPI already ships kunit/test-bug.h -- and it is EMPTY. It sits
# earlier in the -I order than our include dir, so it shadows ours and defines
# nothing, leaving kunit_fail_current_test() undeclared at all 161 register
# accesses. Force-include ours so the macro is defined before vc4_drv.h pulls
# in the empty one.
#
# The real fix belongs in the kernel: a dummy header that does not define the
# macro it exists to stub is not doing its job. Tracked in #51.
# DRM helpers drm-kmod does not provide (drmm_mutex_init,
# dma_fence_match_context). Force-included rather than shipped as
# lkpi/linux/<name>.h: shadowing <linux/dma-fence.h> or <drm/drm_managed.h>
# would override drm-kmod's own copies for this module and drop whatever
# drm-kmod added to them -- the mistake that broke drm-kmod build on both
# arches earlier in #51. This header includes the real ones and adds what is
# missing.
#
# vc4_kms ONLY. vc4_fkms does not use either symbol, and leaving it out keeps
# fkms byte-for-byte unaffected by this change.
CFLAGS+= -include ${.CURDIR:H}/lkpi/lkpi_drm.h

# These three talk to newbus and busdma, so they need FreeBSD's struct resource
# from sys/rman.h -- not LinuxKPI's identically named one from
# <linux/ioport.h>, which the force-include above would otherwise give them.
# One definition per translation unit; see the comment in lkpi_drm.h.
# vc4_kms gets vc4_ioremap_regs() from upstream vc4_drv.c; the fkms copy of
# it would be a duplicate symbol at link time.
CFLAGS.lkpi_rpi_firmware_mbox.c+=	-DLKPI_NO_VC4_IOREMAP

CFLAGS.lkpi_of.c+=		-DLKPI_NO_IOPORT
CFLAGS.lkpi_platform.c+=	-DLKPI_NO_IOPORT
CFLAGS.lkpi_irq.c+=		-DLKPI_NO_IOPORT
CFLAGS.vc4_newbus.c+=		-DLKPI_NO_IOPORT
CFLAGS.vc4_hvs_newbus.c+=	-DLKPI_NO_IOPORT
CFLAGS.vc4_crtc_newbus.c+=	-DLKPI_NO_IOPORT
CFLAGS.vc4_hdmi_newbus.c+=	-DLKPI_NO_IOPORT
CFLAGS.vc4_master_newbus.c+=	-DLKPI_NO_IOPORT

CFLAGS+= -include ${.CURDIR:H}/include/kunit/test-bug.h

# Same narrowing pattern as the fkms probe: of_device_id::data is a
# const void * and drivers cast it to a per-variant enum.
# A probe measures the WHOLE gap, so do not let clang stop at its default
# 20-error cap -- the first run capped three files at 19 and made the
# surface look smaller than it is.
CFLAGS+= -ferror-limit=0

CWARNFLAGS += -Wno-pointer-arith -Wno-format -Wno-cast-qual
CWARNFLAGS += -Wno-void-pointer-to-enum-cast
