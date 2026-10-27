/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * sound/pcm_params.h -- empty (nextbsd-kernel-extensions-graphics#51).
 *
 * vc4_hdmi.c includes this for the ALSA PCM parameter helpers
 * (params_rate(), params_channels() and friends) used by its HDMI audio
 * support. That support is not wired up here -- there is no ALSA core for it
 * to register with -- and the code paths that would call these are unreachable
 * on this build.
 *
 * Empty rather than absent so the include resolves. If HDMI audio is ever
 * wanted, this is where the helpers go, and the compiler will name every one
 * that is missing.
 */
#ifndef _LINUXKPI_SOUND_PCM_PARAMS_H_
#define	_LINUXKPI_SOUND_PCM_PARAMS_H_

#include <sound/soc.h>

#endif /* _LINUXKPI_SOUND_PCM_PARAMS_H_ */
