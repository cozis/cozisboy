// Render every sound effect, one after another, into a mono 16-bit WAV.
//   ./sfx-demo out.wav
#include "sfx.h"

#include <stdio.h>
#include <stdlib.h>

#define RATE 48000

static void put32(FILE *f, unsigned v) { fputc(v, f), fputc(v >> 8, f), fputc(v >> 16, f), fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f), fputc(v >> 8, f); }

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "sounds.wav";
	// (id, param, gap after in seconds): home scroll melody, then the rest
	static const int seq[][2] = {
		{SFX_MOVE, 1}, {SFX_MOVE, 2}, {SFX_MOVE, 3}, {SFX_MOVE, 4}, {SFX_MOVE, 5},
		{SFX_MOVE, 6}, {SFX_MOVE, 7}, {SFX_BUMP, 0}, {SFX_OPEN, 6}, {SFX_WHOOSH, 0},
		{SFX_TICK, 0}, {SFX_TICK, 0}, {SFX_CHANGE, 4}, {SFX_CHANGE, 5}, {SFX_CHANGE, 6},
		{SFX_TOGGLE_OFF, 0}, {SFX_TOGGLE_ON, 0}, {SFX_CONFIRM, 0}, {SFX_POP, 0},
		{SFX_BACK, 0},
	};
	static const float gap[] = {.15f, .15f, .15f, .15f, .15f, .15f, .4f, .5f, .2f, .6f,
				    .15f, .4f, .12f, .12f, .4f, .4f, .5f, .5f, .5f, .8f};
	int n = sizeof seq / sizeof *seq;
	float total = 0;
	for (int i = 0; i < n; i++)
		total += gap[i];
	int frames = (int)(total * RATE);
	int16_t *buf = calloc(frames, sizeof *buf);

	sfx_init(RATE);
	sfx_set_volume(0.6f);
	int pos = 0, peak = 0;
	for (int i = 0; i < n; i++) {
		sfx_play(seq[i][0], seq[i][1]);
		int len = (int)(gap[i] * RATE);
		if (pos + len > frames)
			len = frames - pos;
		sfx_render(buf + pos, len);
		pos += len;
	}
	for (int i = 0; i < frames; i++)
		if (abs(buf[i]) > peak)
			peak = abs(buf[i]);

	FILE *f = fopen(path, "wb");
	if (!f)
		return perror(path), 1;
	fwrite("RIFF", 1, 4, f), put32(f, 36 + frames * 2), fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16), put16(f, 1), put16(f, 1), put32(f, RATE), put32(f, RATE * 2);
	put16(f, 2), put16(f, 16), fwrite("data", 1, 4, f), put32(f, frames * 2);
	fwrite(buf, 2, frames, f);
	fclose(f);
	printf("%s: %.2f s, peak %d/32767\n", path, total, peak);
	return 0;
}
