/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * sound/dmaengine_pcm.h (nextbsd-kernel-extensions-graphics#51).
 *
 * HDMI audio. vc4_hdmi.c registers an ASoC DAI so audio can be carried over
 * HDMI, which needs Linux's sound stack -- ASoC, dmaengine, the PCM layer --
 * none of which exists on FreeBSD.
 *
 * This declares only what vc4_hdmi.c touches so the display path compiles.
 * HDMI audio does NOT work with this, and cannot until there is an ASoC
 * equivalent; the goal here is a picture on the screen first
 * (nextbsd-kernel-extensions-graphics#51 is scoped display-only and says so).
 *
 * snd_dmaengine_pcm_prepare_slave_config() returns -ENOSYS rather than 0, so
 * an audio path that ever runs fails visibly instead of appearing to succeed
 * and producing silence.
 */
#ifndef _LINUXKPI_SOUND_DMAENGINE_PCM_H_
#define	_LINUXKPI_SOUND_DMAENGINE_PCM_H_

#include <linux/errno.h>
#include <linux/types.h>

struct snd_pcm_substream;
struct snd_pcm_hw_params;
struct dma_slave_config;

struct snd_dmaengine_dai_dma_data {
	void		*addr;
	u32		addr_width;
	u32		maxburst;
	unsigned int	slave_id;
	void		*filter_data;
	const char	*chan_name;
	unsigned int	fifo_size;
	unsigned int	flags;
};

struct snd_dmaengine_pcm_config {
	int (*prepare_slave_config)(struct snd_pcm_substream *substream,
	    struct snd_pcm_hw_params *params, struct dma_slave_config *slave_config);
	unsigned int	prealloc_buffer_size;
	unsigned int	pcm_hardware;
	unsigned int	flags;
};

static inline int
snd_dmaengine_pcm_prepare_slave_config(struct snd_pcm_substream *substream,
    struct snd_pcm_hw_params *params, struct dma_slave_config *slave_config)
{

	return (-ENOSYS);
}

#endif /* _LINUXKPI_SOUND_DMAENGINE_PCM_H_ */
