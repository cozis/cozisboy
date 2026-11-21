// Device front end (RG35XX SP): the UI on the framebuffer, the console's
// buttons through evdev, sound through ALSA.
//
// - Display: /dev/fb0 (640x480 XRGB, already switched on by the boot splash).
//   Each frame is drawn off-screen, then copied after the vertical blank.
// - Buttons: gpio-keys-gamepad, polled as a key bitmap (EVIOCGKEY) every frame.
//   Volume keys step the system volume (the codec's DAC, 20 steps evenly
//   spaced in dB) with an on-screen indicator; the level is kept in
//   /data/ui/system-volume. The UI's own volume setting is left alone. The power button and the lid put the console to sleep
//   (cozisboy-sleep).
// - Sound: a thread mixes UI sounds and the emulator into hw:0,0 (48 kHz).
#include "emu.h"
#include "gfx.h"
#include "sfx.h"
#include "ui.h"
#include "ui_priv.h"

// ui.h's BTN_A/B/X/Y/START/SELECT share their names with key-code macros in
// <linux/input.h> (BTN_A = 0x130, ...), which would win once it's included.
// Capture the UI's bit masks first, under names of our own.
enum {
	UI_UP = BTN_UP, UI_DOWN = BTN_DOWN, UI_LEFT = BTN_LEFT, UI_RIGHT = BTN_RIGHT,
	UI_A = BTN_A, UI_B = BTN_B, UI_X = BTN_X, UI_Y = BTN_Y, UI_L = BTN_L, UI_R = BTN_R,
	UI_START = BTN_START, UI_SELECT = BTN_SELECT, UI_MENU = BTN_MENU,
};

#include <alsa/asoundlib.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

#define RATE 48000
#define PERIOD 256

static volatile sig_atomic_t quit;

/* ---- volume ------------------------------------------------------------------- */
//
// System volume on the codec: "DAC Playback Volume" is 0..63 in 1.16 dB steps
// (63 = 0 dB, 0 = mute). Level 1..20 spans -40..0 dB evenly, so every press
// sounds like the same change; level 0 mutes.

#define VOLUME_FILE "/data/ui/system-volume"
#define VOL_STEPS 20
#define VOL_DEFAULT 14
#define OSD_SHOW 1.5f	// seconds the indicator stays after the last press

static float osd_t;	// time left on screen
static float osd_in;	// slide-in progress 0..1
static int vol_level = VOL_DEFAULT;
static int volume_dirty;
static float volume_save_t;

static long dac_value(int level)
{
	if (level <= 0)
		return 0;
	double db = -40.0 * (VOL_STEPS - level) / (VOL_STEPS - 1);
	long v = 63 + lround(db / 1.16);
	return v < 1 ? 1 : v > 63 ? 63 : v;
}

static void volume_apply(void)
{
	snd_mixer_t *m;
	if (snd_mixer_open(&m, 0) < 0)
		return;
	if (snd_mixer_attach(m, "hw:0") == 0 && snd_mixer_selem_register(m, NULL, NULL) == 0 &&
	    snd_mixer_load(m) == 0) {
		snd_mixer_selem_id_t *id;
		snd_mixer_selem_id_alloca(&id);
		snd_mixer_selem_id_set_name(id, "DAC");
		snd_mixer_elem_t *e = snd_mixer_find_selem(m, id);
		if (e)
			snd_mixer_selem_set_playback_volume_all(e, dac_value(vol_level));
	}
	snd_mixer_close(m);
}

static void volume_load(void)
{
	FILE *f = fopen(VOLUME_FILE, "r");
	int v;
	if (f) {
		if (fscanf(f, "%d", &v) == 1 && v >= 0 && v <= VOL_STEPS)
			vol_level = v;
		fclose(f);
	}
}

static void volume_save(void)
{
	FILE *f = fopen(VOLUME_FILE ".tmp", "w");
	if (!f)
		return;
	fprintf(f, "%d\n", vol_level);
	fclose(f);
	rename(VOLUME_FILE ".tmp", VOLUME_FILE);
}

static void volume_step(int dir)
{
	int v = vol_level + dir;
	if (v < 0 || v > VOL_STEPS) {
		sfx_play(SFX_BUMP, 0);
	} else {
		vol_level = v;
		volume_apply();
		sfx_play(SFX_CHANGE, v * 10 / VOL_STEPS);
		volume_dirty = 1;
		volume_save_t = 1.0f;	// write once the buttons settle
	}
	osd_t = OSD_SHOW;
}

static void volume_tick(float dt)
{
	if (volume_dirty && (volume_save_t -= dt) <= 0) {
		volume_save();
		volume_dirty = 0;
	}
	if (osd_t > 0)
		osd_t -= dt;
	float target = osd_t > 0 ? 1.0f : 0.0f;
	osd_in += (target - osd_in) * (1 - expf(-dt * 16));
	if (osd_in < 0.002f)
		osd_in = 0;
}

/* ---- sound -------------------------------------------------------------------- */

// Codec mixer: outputs on, Line Out at -6 dB headroom (27 of 31), and the DAC
// (the system volume) at the saved level.
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
		if (!strcmp(snd_mixer_selem_get_name(e), "Line Out") && snd_mixer_selem_has_playback_volume(e))
			snd_mixer_selem_set_playback_volume_all(e, 27);
	}
	snd_mixer_close(m);
	volume_apply();
}

static void *audio_thread(void *arg)
{
	(void)arg;
	snd_pcm_t *pcm = NULL;
	// The boot jingle may still own the device for a moment: retry.
	for (int i = 0; i < 100 && !quit; i++) {
		if (snd_pcm_open(&pcm, "hw:0,0", SND_PCM_STREAM_PLAYBACK, 0) == 0)
			break;
		pcm = NULL;
		usleep(100000);
	}
	if (!pcm || snd_pcm_set_params(pcm, SND_PCM_FORMAT_S16_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
				       2, RATE, 1, 40000) < 0) {
		fprintf(stderr, "cozisboy-ui: no sound\n");
		return NULL;
	}
	mixer_setup();
	static int16_t mono[PERIOD], stereo[PERIOD * 2];
	while (!quit) {
		sfx_render(mono, PERIOD);
		emu_audio_mix(mono, PERIOD, RATE);
		for (int i = 0; i < PERIOD; i++)
			stereo[2 * i] = stereo[2 * i + 1] = mono[i];
		snd_pcm_sframes_t r = snd_pcm_writei(pcm, stereo, PERIOD);
		if (r < 0 && snd_pcm_recover(pcm, r, 1) < 0)
			usleep(10000);	// e.g. after sleep: keep trying
	}
	snd_pcm_close(pcm);
	return NULL;
}

// Speaker with 0-3 sound waves, white, at (x, y), 2x scale pixel art.
static void draw_speaker(canvas *c, int x, int y, int level)
{
	fill(c, x, y + 8, 6, 8, WHITE);			// body
	for (int i = 0; i < 7; i++)			// cone
		fill(c, x + 6 + i, y + 7 - i, 1, 10 + 2 * i, WHITE);
	if (level == 0) {				// muted: a cross
		for (int i = 0; i < 9; i++) {
			fill(c, x + 17 + i, y + 7 + i, 2, 2, WHITE);
			fill(c, x + 25 - i, y + 7 + i, 2, 2, WHITE);
		}
		return;
	}
	int waves = level >= 8 ? 3 : level >= 4 ? 2 : 1;
	for (int w = 0; w < waves; w++) {
		int wx = x + 17 + w * 5, h = 8 + w * 6, wy = y + 12 - h / 2;
		fill(c, wx, wy + 2, 2, h - 4, WHITE);
		fill(c, wx - 1, wy, 2, 2, WHITE);
		fill(c, wx - 1, wy + h - 2, 2, 2, WHITE);
	}
}

// The indicator: a pill sliding down from the top edge.
static void volume_draw(uint32_t *buf)
{
	if (osd_in <= 0)
		return;
	canvas cv = {buf, UI_W, UI_H, UI_W}, *c = &cv;
	const int segs = VOL_STEPS, sw = 8, sg = 3, h = 44;
	const int w = 16 + 30 + 14 + segs * (sw + sg) - sg + 16 + 64;
	int x = (UI_W - w) / 2, y = (int)(-h - 8 + (h + 22) * osd_in);
	int v = vol_level;

	rrect(c, x - 3, y - 3, w + 6, h + 6, 5, BLACK);			// outline
	rrect(c, x + 2, y + h + 1, w, 4, 2, 0x050505);			// drop shadow
	rrect(c, x, y, w, h, 4, BAR);
	draw_speaker(c, x + 16, y + 10, v * 10 / VOL_STEPS + (v > 0));
	int sx = x + 16 + 30 + 14;
	for (int i = 0; i < segs; i++) {
		int on = i < v;
		uint32_t col = on ? PAL[i * 8 / segs] : 0x2a2a2a;
		int bh = 8 + i;					// rising steps
		fill(c, sx + i * (sw + sg) + 2, y + h - 10 - bh + 2, sw, bh, on ? 0x050505 : BAR);
		fill(c, sx + i * (sw + sg), y + h - 10 - bh, sw, bh, col);
	}
	char num[8];
	if (v)
		snprintf(num, sizeof num, "%d%%", v * 100 / VOL_STEPS);
	else
		snprintf(num, sizeof num, "MUTE");
	int tx = x + w - 16 - text_width(num, 2);
	text(c, tx + 1, y + 16, num, 2, BLACK);
	text(c, tx, y + 15, num, 2, WHITE);
}

/* ---- input -------------------------------------------------------------------- */

static int pad_fd = -1, vol_fd = -1, pek_fd = -1, lid_fd = -1;

static int open_input(const char *want)
{
	DIR *d = opendir("/dev/input");
	if (!d)
		return -1;
	struct dirent *e;
	int found = -1;
	while (found < 0 && (e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		char path[300], name[128] = "";
		snprintf(path, sizeof path, "/dev/input/%s", e->d_name);
		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;
		ioctl(fd, EVIOCGNAME(sizeof name), name);
		if (!strcmp(name, want))
			found = fd;
		else
			close(fd);
	}
	closedir(d);
	return found;
}

static int key_down(const unsigned char *bits, int code)
{
	return bits[code / 8] >> (code % 8) & 1;
}

// Buttons held right now. Measured on the device: A = BTN_EAST, B = BTN_SOUTH,
// X = BTN_WEST, Y = BTN_NORTH (the device tree's labels for X and Y are swapped).
static unsigned read_buttons(void)
{
	static const struct {
		int code;
		unsigned btn;
	} map[] = {
		{BTN_DPAD_UP, UI_UP},       {BTN_DPAD_DOWN, UI_DOWN},   {BTN_DPAD_LEFT, UI_LEFT},
		{BTN_DPAD_RIGHT, UI_RIGHT}, {BTN_EAST, UI_A},           {BTN_SOUTH, UI_B},
		{BTN_WEST, UI_X},           {BTN_NORTH, UI_Y},          {BTN_TL, UI_L},
		{BTN_TR, UI_R},             {BTN_START, UI_START},      {BTN_SELECT, UI_SELECT},
		{BTN_MODE, UI_MENU},
	};
	unsigned char bits[KEY_MAX / 8 + 1] = {0};
	unsigned b = 0;
	if (pad_fd >= 0 && ioctl(pad_fd, EVIOCGKEY(sizeof bits), bits) >= 0)
		for (size_t i = 0; i < sizeof map / sizeof *map; i++)
			if (key_down(bits, map[i].code))
				b |= map[i].btn;
	// drain queued events (state is read above)
	struct input_event ev[16];
	if (pad_fd >= 0)
		while (read(pad_fd, ev, sizeof ev) > 0)
			;
	return b;
}

static void sleep_now(void)
{
	pid_t p = fork();
	if (p == 0) {
		execl("/usr/sbin/cozisboy-sleep", "cozisboy-sleep", (char *)NULL);
		_exit(127);
	}
	if (p > 0)
		waitpid(p, NULL, 0);	// returns after wake-up
}

// Volume keys (with repeat) and power/lid events.
static void system_keys(float dt)
{
	struct input_event ev[16];
	ssize_t n;
	static float vol_hold;
	static int vol_dir;
	if (vol_fd >= 0)
		while ((n = read(vol_fd, ev, sizeof ev)) > 0)
			for (int i = 0; i < n / (ssize_t)sizeof *ev; i++)
				if (ev[i].type == EV_KEY && (ev[i].code == KEY_VOLUMEUP || ev[i].code == KEY_VOLUMEDOWN)) {
					int dir = ev[i].code == KEY_VOLUMEUP ? 1 : -1;
					if (ev[i].value == 1) {
						vol_dir = dir;
						vol_hold = 0;
						volume_step(dir);
					} else if (ev[i].value == 0 && vol_dir == dir) {
						vol_dir = 0;
					}
				}
	if (vol_dir) {
		vol_hold += dt;
		if (vol_hold > 0.4f) {
			vol_hold -= 0.12f;
			volume_step(vol_dir);
		}
	}
	int sleep_req = 0;
	if (pek_fd >= 0)
		while ((n = read(pek_fd, ev, sizeof ev)) > 0)
			for (int i = 0; i < n / (ssize_t)sizeof *ev; i++)
				if (ev[i].type == EV_KEY && ev[i].code == KEY_POWER && ev[i].value == 0)
					sleep_req = 1;	// on release, so the press doesn't wake us again
	if (lid_fd >= 0)
		while ((n = read(lid_fd, ev, sizeof ev)) > 0)
			for (int i = 0; i < n / (ssize_t)sizeof *ev; i++)
				if (ev[i].type == EV_SW && ev[i].code == SW_LID && ev[i].value == 1)
					sleep_req = 1;
	if (sleep_req) {
		sleep_now();
		// drop whatever woke us (the power press, the lid opening)
		usleep(200000);
		for (int fd, k = 0; k < 2; k++)
			if ((fd = k ? pek_fd : lid_fd) >= 0)
				while (read(fd, ev, sizeof ev) > 0)
					;
	}
}

/* ---- main --------------------------------------------------------------------- */

static void on_signal(int s)
{
	(void)s;
	quit = 1;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(void)
{
	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (fb < 0 || ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix) ||
	    var.bits_per_pixel != 32 || var.xres < UI_W || var.yres < UI_H) {
		fprintf(stderr, "cozisboy-ui: need a %dx%d 32 bpp framebuffer\n", UI_W, UI_H);
		return 1;
	}
	size_t size = (size_t)fix.line_length * var.yres;
	uint32_t *fbmem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (fbmem == MAP_FAILED) {
		perror("mmap fb");
		return 1;
	}
	ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK);
	int stride = fix.line_length / 4;
	static uint32_t buf[UI_W * UI_H];

	pad_fd = open_input("gpio-keys-gamepad");
	vol_fd = open_input("gpio-keys-volume");
	pek_fd = open_input("axp20x-pek");
	lid_fd = open_input("gpio-keys-lid");
	if (pad_fd < 0)
		fprintf(stderr, "cozisboy-ui: no gamepad input\n");

	ui_init();
	volume_load();
	sfx_init(RATE);
	pthread_t at;
	pthread_create(&at, NULL, audio_thread, NULL);

	double last = now();
	while (!quit) {
		double t = now();
		float dt = (float)(t - last);
		last = t;
		if (dt > 0.1f)
			dt = 0.1f;
		system_keys(dt);
		ui_frame(buf, UI_W, dt, read_buttons());
		volume_tick(dt);
		volume_draw(buf);
		__u32 crtc = 0;
		if (ioctl(fb, FBIO_WAITFORVSYNC, &crtc) < 0) {
			double left = 1.0 / 60 - (now() - t);	// no vblank wait: pace by time
			if (left > 0)
				usleep((useconds_t)(left * 1e6));
		}
		for (int y = 0; y < UI_H; y++)
			memcpy(fbmem + (size_t)y * stride, buf + (size_t)y * UI_W, UI_W * 4);
	}
	pthread_join(at, NULL);
	return 0;
}
