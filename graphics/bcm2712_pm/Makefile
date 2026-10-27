# BCM2712 power management -- the V3D reset line (nextbsd-kernel-extensions-graphics#66).
#
# Plain newbus: an OFW reset provider of one line. v3d@2000000 takes
# "resets = <&pm 0>", which nothing could satisfy before this, and v3d's probe
# fails outright without it -- its fallback maps a "bridge" register bank a
# 2712 device tree does not have. See bcm2712_pm.c.
#
# No power-domain provider: v3d makes no genpd or pm_runtime calls.

KMOD=	bcm2712_pm
SRCS=	bcm2712_pm.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h hwreset_if.h opt_platform.h

.include <bsd.kmod.mk>
