/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * sound/soc.h -- ASoC types only (nextbsd-kernel-extensions-graphics#51).
 *
 * vc4_hdmi.c registers an ASoC card, component and DAI so audio can travel
 * over the HDMI link. FreeBSD has no ASoC, and #51 is scoped display-only.
 *
 * This exists so vc4_hdmi.c -- 102KB, the largest file in the port -- COMPILES
 * far enough to report its display gaps. Without it the whole translation unit
 * aborts at this include and the probe under-reports, which is the failure
 * mode this probe has hit repeatedly.
 *
 * Every registration entry point returns -ENOSYS rather than 0, so an audio
 * path that ever runs fails visibly instead of appearing to succeed and
 * producing silence. HDMI audio does not work and is not close to working.
 */
#ifndef _LINUXKPI_SOUND_SOC_H_
#define	_LINUXKPI_SOUND_SOC_H_

#include <linux/types.h>
#include <linux/device.h>
#include <linux/errno.h>

struct snd_soc_component;
struct snd_soc_dai;
struct snd_soc_pcm_runtime;
/*
 * Completed rather than forward-declared (#51): vc4_hdmi embeds these BY VALUE
 * -- struct vc4_hdmi_audio has three dai_link_components and struct vc4_hdmi
 * has an snd_soc_jack -- so an incomplete type makes those structs
 * incomplete and takes every user of them with it.
 *
 * The members are the ones vc4 names. Nothing reads them: HDMI audio is not
 * wired up here, and these exist so the display half compiles. Sizes need not
 * match Linux, because no ALSA core on this system ever sees one.
 */
/*
 * struct snd_soc_jack lives in <sound/jack.h>. Included rather than
 * re-declared: the inlines below take one by pointer, and a bare forward
 * declaration inside a prototype is scoped to that prototype
 * ("declaration will not be visible outside of this function").
 */
#include <sound/jack.h>
struct snd_soc_dai_link_component {
	const char		*name;
	struct device_node	*of_node;
	const char		*dai_name;
};
struct snd_pcm_substream;

struct snd_soc_dai_ops {
	int (*startup)(struct snd_pcm_substream *, struct snd_soc_dai *);
	void (*shutdown)(struct snd_pcm_substream *, struct snd_soc_dai *);
	int (*hw_params)(struct snd_pcm_substream *, void *, struct snd_soc_dai *);
	int (*set_fmt)(struct snd_soc_dai *, unsigned int);
	int (*trigger)(struct snd_pcm_substream *, int, struct snd_soc_dai *);
	int (*probe)(struct snd_soc_dai *);
};

struct snd_soc_dai_driver {
	const char			*name;
	unsigned int			id;
	const struct snd_soc_dai_ops	*ops;
	struct {
		const char	*stream_name;
		int		channels_min;
		int		channels_max;
		uint64_t	rates;
		uint64_t	formats;
	} playback, capture;
};

struct snd_soc_component_driver {
	const char	*name;
	int		(*probe)(struct snd_soc_component *);
	void		(*remove)(struct snd_soc_component *);
};

struct snd_soc_dai_link {
	const char	*name;
	const char	*stream_name;
	void		*cpus, *codecs, *platforms;
	unsigned int	num_cpus, num_codecs, num_platforms;
	int		(*init)(struct snd_soc_pcm_runtime *);
};

struct snd_soc_card {
	const char			*name;
	struct device			*dev;
	struct snd_soc_dai_link		*dai_link;
	int				num_links;
	void				*drvdata;
};

static inline void
snd_soc_card_set_drvdata(struct snd_soc_card *card, void *data)
{

	card->drvdata = data;
}

static inline void *
snd_soc_card_get_drvdata(struct snd_soc_card *card)
{

	return (card->drvdata);
}

static inline void *snd_soc_dai_get_drvdata(struct snd_soc_dai *dai) { return (NULL); }
static inline void snd_soc_dai_init_dma_data(struct snd_soc_dai *dai, void *p, void *c) { }
static inline struct snd_soc_dai *snd_soc_rtd_to_codec(struct snd_soc_pcm_runtime *rtd, int n) { return (NULL); }
static inline int snd_soc_card_jack_new(struct snd_soc_card *c, const char *n, int t, struct snd_soc_jack *j) { return (-ENOSYS); }
static inline int snd_soc_component_set_jack(struct snd_soc_component *c, struct snd_soc_jack *j, void *d) { return (-ENOSYS); }
static inline int snd_soc_register_card(struct snd_soc_card *card) { return (-ENOSYS); }
static inline int devm_snd_soc_register_card(struct device *dev, struct snd_soc_card *card) { return (-ENOSYS); }
static inline int snd_soc_register_component(struct device *dev,
    const struct snd_soc_component_driver *drv, struct snd_soc_dai_driver *dai, int num) { return (-ENOSYS); }
static inline int devm_snd_soc_register_component(struct device *dev,
    const struct snd_soc_component_driver *drv, struct snd_soc_dai_driver *dai, int num) { return (-ENOSYS); }
static inline void snd_soc_unregister_component(struct device *dev) { }

#endif /* _LINUXKPI_SOUND_SOC_H_ */
