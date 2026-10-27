/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * sound/pcm_drm_eld.h -- no-op (nextbsd-kernel-extensions-graphics#51).
 *
 * Constrains PCM parameters to what the sink's ELD advertises, so HDMI audio
 * only offers rates the display supports. Audio-only; see sound/soc.h beside
 * it for why the whole ASoC surface here is declarations without behaviour.
 */
#ifndef _LINUXKPI_SOUND_PCM_DRM_ELD_H_
#define	_LINUXKPI_SOUND_PCM_DRM_ELD_H_

#include <linux/errno.h>

struct snd_pcm_runtime;

static inline int
snd_pcm_hw_constraint_eld(struct snd_pcm_runtime *runtime, void *eld)
{

	return (-ENOSYS);
}

#endif /* _LINUXKPI_SOUND_PCM_DRM_ELD_H_ */
