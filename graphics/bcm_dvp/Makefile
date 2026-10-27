# BCM2711/2712 DVP clocks and resets (nextbsd-kernel-extensions-graphics#51).
#
# Plain newbus: a clkdom of two gate clocks and an OFW reset provider of six
# lines. hdmi@7ef00700 takes "resets = <&dvp 1>", which nothing could satisfy
# before this, so the HDMI block stayed in reset and never produced a signal.

KMOD=	bcm_dvp
SRCS=	bcm_dvp.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h clkdev_if.h clknode_if.h hwreset_if.h opt_platform.h

.include <bsd.kmod.mk>
