// cozisboy-chime: plays the boot jingle (apps/common/sfx, SFX_BOOT) via alsa-lib.
// Started by the splash at its t=0; a separate process so the splash itself can
// stay statically linked (it runs as PID 1). Usage: cozisboy-chime [variant]
#include "sfx.h"

#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

#define RATE 48000
#define PERIOD 512
#define SECONDS 3.2f

static void mixer_setup(void)
{
	snd_mixer_t *m;
	if (snd_mixer_open(&m, 0) < 0)
		return;
	if (snd_mixer_attach(m, "hw:0") < 0 || snd_mixer_selem_register(m, NULL, NULL) < 0 ||
	    snd_mixer_load(m) < 0) {
		snd_mixer_close(m);
		return;
	}
	for (snd_mixer_elem_t *e = snd_mixer_first_elem(m); e; e = snd_mixer_elem_next(e)) {
		if (snd_mixer_selem_has_playback_switch(e))
			snd_mixer_selem_set_playback_switch_all(e, 1);
		if (snd_mixer_selem_has_playback_volume(e)) {
			long lo, hi;
			snd_mixer_selem_get_playback_volume_range(e, &lo, &hi);
			snd_mixer_selem_set_playback_volume_all(e, lo + (hi - lo) * 9 / 10);
		}
	}
	snd_mixer_close(m);
}

int main(int argc, char **argv)
{
	int variant = argc > 1 ? atoi(argv[1]) : SFX_BOOT_DEFAULT;
	snd_pcm_t *pcm;
	if (snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0) < 0)
		return 1;
	if (snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
			       2, RATE, 1, 50000) < 0) {
		snd_pcm_close(pcm);
		return 1;
	}
	mixer_setup();
	sfx_init(RATE);
	sfx_set_volume(0.6f);
	sfx_play(SFX_BOOT, variant);

	static int16_t mono[PERIOD], stereo[PERIOD * 2];
	for (int done = 0; done < (int)(SECONDS * RATE); done += PERIOD) {
		sfx_render(mono, PERIOD);
		for (int i = 0; i < PERIOD; i++)
			stereo[2 * i] = stereo[2 * i + 1] = mono[i];
		snd_pcm_sframes_t r = snd_pcm_writei(pcm, stereo, PERIOD);
		if (r < 0 && snd_pcm_recover(pcm, r, 1) < 0)
			break;
	}
	snd_pcm_drain(pcm);
	snd_pcm_close(pcm);
	return 0;
}
