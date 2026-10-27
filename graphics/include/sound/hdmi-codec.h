/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * sound/hdmi-codec.h (nextbsd-kernel-extensions-graphics#51).
 *
 * The ASoC HDMI codec interface. vc4_hdmi.c registers itself as an HDMI audio
 * codec so audio can be carried over the HDMI link.
 *
 * Types only, no implementation. HDMI audio does not work and cannot until
 * there is an ASoC equivalent on FreeBSD -- same position as
 * sound/dmaengine_pcm.h next to it. #51 is scoped display-only; this exists so
 * the display path compiles rather than stopping at this include.
 *
 * Structure layouts follow upstream so the driver's initialisers still make
 * sense to read, but nothing consumes them.
 */
#ifndef _LINUXKPI_SOUND_HDMI_CODEC_H_
#define	_LINUXKPI_SOUND_HDMI_CODEC_H_

#include <linux/types.h>

struct device;
struct snd_soc_component;
struct snd_soc_dai;
struct snd_pcm_substream;

struct hdmi_codec_daifmt {
	int		fmt;
	unsigned int	bit_clk_inv:1;
	unsigned int	frame_clk_inv:1;
	unsigned int	bit_clk_provider:1;
	unsigned int	frame_clk_provider:1;
	unsigned int	bit_fmt;
};

struct hdmi_codec_params {
	int		iec;
	int		cea;
	unsigned int	sample_rate;
	unsigned int	sample_width;
	unsigned int	channels;
};

typedef void (*hdmi_codec_plugged_cb)(struct device *dev, bool plugged);

struct hdmi_codec_ops {
	int  (*audio_startup)(struct device *dev, void *data);
	int  (*prepare)(struct device *dev, void *data,
	     struct hdmi_codec_daifmt *fmt, struct hdmi_codec_params *hparms);
	void (*audio_shutdown)(struct device *dev, void *data);
	int  (*mute_stream)(struct device *dev, void *data, bool enable, int direction);
	int  (*get_eld)(struct device *dev, void *data, uint8_t *buf, size_t len);
	int  (*get_dai_id)(struct snd_soc_component *comment, void *data);
	int  (*hook_plugged_cb)(struct device *dev, void *data,
	     hdmi_codec_plugged_cb fn, struct device *codec_dev);
};

struct hdmi_codec_pdata {
	const struct hdmi_codec_ops	*ops;
	uint			i2s:1;
	uint			no_i2s_playback:1;
	uint			no_i2s_capture:1;
	uint			spdif:1;
	uint			no_spdif_playback:1;
	uint			no_spdif_capture:1;
	int			max_i2s_channels;
	void			*data;
};

#endif /* _LINUXKPI_SOUND_HDMI_CODEC_H_ */
