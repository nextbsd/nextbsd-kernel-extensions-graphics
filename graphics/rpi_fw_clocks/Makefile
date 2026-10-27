# Raspberry Pi firmware clocks (nextbsd-kernel-extensions-graphics#51).
#
# Plain newbus/clkdev -- no LinuxKPI, no DRM. It provides the clocks that
# /soc/firmware/clocks advertises and nothing implements, which is why every
# clk_set_rate() in vc4_hdmi was a silent no-op on a NULL clock and the HDMI
# PHY never got a pixel clock.

KMOD=	rpi_fw_clocks
SRCS=	rpi_fw_clocks.c
SRCS+=	bus_if.h device_if.h ofw_bus_if.h clknode_if.h opt_platform.h

.include <bsd.kmod.mk>
