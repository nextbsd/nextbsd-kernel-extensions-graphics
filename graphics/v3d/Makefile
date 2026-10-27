# v3d -- the Broadcom V3D GPU driver for BCM2712 (nextbsd-kernel-extensions-graphics#66).
#
# This is the RENDER half of the Pi 5 graphics stack. vc4_kms (#51) drives the
# display; on 2712 the 3D core is a separate device with its own driver, unlike
# BCM2711 and earlier where "vc4" meant both.
#
# It builds, links, loads and registers a render node on a Pi 500+:
#
#	v3d0: <Broadcom V3D GPU> ... irq 70,71 on simplebus1
#	[drm] Initialized v3d 1.0.0 for v3d.0 on minor 1
#	/dev/dri/renderD128
#
# GL on top of it still needs Mesa built with v3d in GALLIUM_DRIVERS
# (nextbsd#427). This is the kernel half.
#
# THE SCHEDULER IS NOT VENDORED HERE, and that is the main thing to know about
# this module. v3d submits work through drm_sched, and drm-kmod 6.12-lts
# already compiles the DRM scheduler into the DRM core -- 84 GLOBAL drm_sched_*
# symbols in the IOGraphics.kext this links against, which arrived for amdgpu.
# So the scheduler is a dependency, not a porting job.
#
# Sources vendored UNMODIFIED from raspberrypi/linux rpi-6.12.y, matching
# drm-kmod's 6.12-lts. The RPi tree rather than mainline v6.12 because 2712
# support only exists there: mainline's v3d_of_match carries no .data for
# "brcm,2712-v3d" and has no v3d_idle_sms()/V3D_SMS_* power sequencing, which
# is how the 2712 part is powered up and reset.
#
# Deliberately NOT vendored:
#   v3d_debugfs.c  -- CONFIG_DEBUG_FS is off here, same as vc4_kms
#   v3d_sysfs.c    -- sysfs attribute groups; v3d_sysfs_init() is stubbed
#   v3d_gemfs.c    -- a private huge-page tmpfs mount for GEM objects. Built on
#                     get_fs_type()/vfs_kern_mount(); LinuxKPI has no VFS. The
#                     NULL-mount path it leaves behind is upstream's own
#                     no-huge-pages fallback, so this costs throughput on large
#                     buffers and not correctness.
#
.include "../kconfig.mk"

SRCDIR=	${.CURDIR:H}/drivers/gpu/drm/v3d

.PATH:	${SRCDIR} ${.CURDIR} ${.CURDIR:H}/lkpi

KMOD=	v3d

EXPORT_SYMS=	YES

SRCS=	\
	v3d_drv.c \
	v3d_bo.c \
	v3d_gem.c \
	v3d_irq.c \
	v3d_mmu.c \
	v3d_fence.c \
	v3d_sched.c \
	v3d_submit.c \
	v3d_perfmon.c \
	v3d_trace_points.c

# The newbus shim plus the module-private LinuxKPI, same set vc4_kms uses
# minus the component framework -- v3d is a single device, not a component.
SRCS+=	\
	v3d_newbus.c \
	lkpi_of.c \
	lkpi_drm.c \
	lkpi_platform.c \
	lkpi_irq.c \
	lkpi_rpi_firmware.c \
	lkpi_rpi_firmware_mbox.c \
	v3d_sysfs_stub.c \
	v3d_gemfs_stub.c \
	v3d_state.c

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

CLEANFILES+= ${KMOD}.ko.full ${KMOD}.ko.debug

.include <bsd.kmod.mk>

.include "../linuxkpi_version.mk"
.include "../compiler_flags.mk"

# PRIVATE LinuxKPI, ahead of drm-kmod's and the kernel's -- see vc4_kms/Makefile
# for why this path MUST stay first. LKPI_PFX keeps the symbols unique, which
# matters more here than usual: vc4_kms compiles the same lkpi_*.c files and
# both modules are loaded at once on a Pi 5.
CFLAGS+= -I${.CURDIR:H}/lkpi -DLKPI_PFX=${KMOD}_

CFLAGS+= -I${.CURDIR:H}/linuxkpi/gplv2/include
CFLAGS+= -I${.CURDIR:H}/linuxkpi/bsd/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/common/include
CFLAGS+= -I${SYSDIR}/compat/linuxkpi/dummy/include

CFLAGS+= -I${.CURDIR:H}/include
CFLAGS+= -I${.CURDIR:H}/include/drm
CFLAGS+= -I${.CURDIR:H}/include/uapi
CFLAGS+= -I${.CURDIR:H}/drivers/gpu -I${SRCDIR}
CFLAGS+= -I${.CURDIR}

CFLAGS+= '-DKBUILD_MODNAME="${KMOD}"'
CFLAGS+= '-DLINUXKPI_PARAM_PREFIX=v3d_' -DDRM_SYSCTL_PARAM_PREFIX=_${KMOD}

# DEBUG_FS filtered for the same reason as vc4_kms: v3d_debugfs.c is not
# vendored, so the entry points its guarded calls name do not exist.
CFLAGS+= ${KCONFIG:NDEBUG_FS:C/(.*)/-DCONFIG_\1/}
CFLAGS+= -DCONFIG_RASPBERRYPI_FIRMWARE=1
CFLAGS+= -DCONFIG_MMU=1

CFLAGS+= -include ${.CURDIR:H}/lkpi/lkpi_drm.h

# The 6.12-lts DRM gap: idr_init_base, platform_get_irq_optional and
# drm_gem_shmem_create_with_mnt. See v3d_compat.h for why it is force-included
# rather than shipped as a shadowing <drm/...> header.
CFLAGS+= -include ${.CURDIR}/v3d_compat.h

# These talk to newbus and busdma, so they need FreeBSD's struct resource from
# sys/rman.h rather than LinuxKPI's identically named one that the force-include
# above would otherwise supply. One definition per translation unit.
CFLAGS.lkpi_of.c+=		-DLKPI_NO_IOPORT
CFLAGS.lkpi_platform.c+=	-DLKPI_NO_IOPORT
CFLAGS.lkpi_irq.c+=		-DLKPI_NO_IOPORT
CFLAGS.v3d_newbus.c+=		-DLKPI_NO_IOPORT

# vc4_ioremap_regs() comes from vc4_drv.c in the display module; nothing in v3d
# defines it, so the mailbox glue must not bring in its own copy either.
CFLAGS.lkpi_rpi_firmware_mbox.c+=	-DLKPI_NO_VC4_IOREMAP

CFLAGS+= -include ${.CURDIR:H}/include/kunit/test-bug.h

# A port measures the WHOLE gap, so do not let clang stop at its default
# 20-error cap.
CFLAGS+= -ferror-limit=0

CWARNFLAGS += -Wno-pointer-arith -Wno-format -Wno-cast-qual
CWARNFLAGS += -Wno-void-pointer-to-enum-cast
