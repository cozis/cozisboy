// Host-side preview: write the splash animation as raw RGB24 frames to stdout.
//   ./preview [seconds] [fps] [jingle.wav] [variant] | ffmpeg -f rawvideo -pix_fmt rgb24 -s 640x480 -r 30 -i - out.mp4
// Progress is faked as a smooth ramp over the whole duration. If a WAV path is
// given, the boot jingle (same timeline) is rendered there too.
#include "sfx.h"
#include "splash_anim.h"

#include <stdio.h>
#include <stdlib.h>

#define W 640
#define H 480

static void put32(FILE *f, unsigned v) { fputc(v, f), fputc(v >> 8, f), fputc(v >> 16, f), fputc(v >> 24, f); }
static void put16(FILE *f, unsigned v) { fputc(v, f), fputc(v >> 8, f); }

static void write_jingle(const char *path, float secs, int variant)
{
	const int rate = 48000, n = (int)(secs * rate);
	int16_t *pcm = calloc(n, sizeof *pcm);
	sfx_init(rate);
	sfx_set_volume(0.6f);
	sfx_play(SFX_BOOT, variant);
	sfx_render(pcm, n);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fwrite("RIFF", 1, 4, f), put32(f, 36 + n * 2), fwrite("WAVEfmt ", 1, 8, f);
	put32(f, 16), put16(f, 1), put16(f, 1), put32(f, rate), put32(f, rate * 2);
	put16(f, 2), put16(f, 16), fwrite("data", 1, 4, f), put32(f, n * 2);
	fwrite(pcm, 2, n, f);
	fclose(f);
	free(pcm);
}

int main(int argc, char **argv)
{
	float secs = argc > 1 ? strtof(argv[1], NULL) : 3.5f;
	int fps = argc > 2 ? atoi(argv[2]) : 30;
	static uint32_t buf[W * H];
	static unsigned char rgb[W * H * 3];
	int frames = (int)(secs * fps);

	if (argc > 3)
		write_jingle(argv[3], secs, argc > 4 ? atoi(argv[4]) : SFX_BOOT_DEFAULT);

	for (int f = 0; f < frames; f++) {
		float t = (float)f / fps;
		float p = t < 1.2f ? 0 : (t - 1.2f) / (secs - 1.4f);
		splash_render(buf, W, H, W, t, p > 1 ? 1 : p);
		for (int i = 0; i < W * H; i++) {
			rgb[i * 3 + 0] = buf[i] >> 16;
			rgb[i * 3 + 1] = buf[i] >> 8;
			rgb[i * 3 + 2] = buf[i];
		}
		fwrite(rgb, 1, sizeof rgb, stdout);
	}
	return 0;
}
