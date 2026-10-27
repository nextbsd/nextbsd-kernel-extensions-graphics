/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * sound/jack.h -- types only (nextbsd-kernel-extensions-graphics#51).
 *
 * The last include standing between vc4_hdmi.c and a complete measurement.
 * ASoC jack reporting, used to tell userspace an HDMI sink appeared. Same
 * position as sound/soc.h beside it: no ASoC on FreeBSD, #51 is display-only,
 * and this exists so the file compiles rather than aborting at the include.
 */
#ifndef _LINUXKPI_SOUND_JACK_H_
#define	_LINUXKPI_SOUND_JACK_H_

#include <linux/types.h>

#define	SND_JACK_LINEOUT	0x0002
#define	SND_JACK_MECHANICAL	0x0004
#define	SND_JACK_VIDEOOUT	0x0010
#define	SND_JACK_AVOUT		(SND_JACK_LINEOUT | SND_JACK_VIDEOOUT)

struct snd_jack;

struct snd_soc_jack {
	struct snd_jack	*jack;
	int		status;
};

static inline void
snd_jack_report(struct snd_jack *jack, int status)
{
}

#endif /* _LINUXKPI_SOUND_JACK_H_ */
