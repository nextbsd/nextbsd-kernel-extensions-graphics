/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * linux/dma-direct.h (nextbsd-kernel-extensions-graphics#51).
 *
 * vc4_drv.c uses exactly one symbol from this header, to turn the DMA address
 * the firmware handed back into a physical address when setting up the GPU's
 * memory window. Only that one is declared -- the rest of Linux's dma-direct
 * surface is deliberately absent so a new user is a compile error rather than
 * a silent stub.
 *
 * On the Pi there is no IOMMU between the GPU and RAM and LinuxKPI's DMA
 * addresses are bus addresses, so the mapping is the identity. That is true
 * for this SoC, NOT in general: a platform with an IOMMU or a DMA offset would
 * need a real translation here.
 */
#ifndef _LINUXKPI_LINUX_DMA_DIRECT_H_
#define	_LINUXKPI_LINUX_DMA_DIRECT_H_

#include <linux/dma-mapping.h>
#include <linux/types.h>

static inline phys_addr_t
dma_to_phys(struct device *dev, dma_addr_t daddr)
{

	return ((phys_addr_t)daddr);
}

#endif /* _LINUXKPI_LINUX_DMA_DIRECT_H_ */
