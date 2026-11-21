// cozisboy boot splash: device runner (fbdev).
//
// Draws splash_render() frames to /dev/fb0 from the first moment userspace
// runs. Boot progress comes from a file that init scripts update:
//     echo 40 > /run/splash.progress      (0..100)
// The splash exits (leaving its last frame on screen, so whatever starts next
// takes over without a black flash) when any of these happens:
//     - SIGTERM / SIGINT
//     - /run/splash.stop exists
//     - progress reaches 100 and the intro animation has finished
//     - the timeout (default 20 s) expires
//
// The console is switched to graphics mode so fbcon cannot draw boot text or
// a cursor over the splash. The backlight starts off (device tree) and is
// switched on right after the first frame, so the screen never shows lit black.
//
// The boot jingle (apps/common/sfx, SFX_BOOT) plays on an audio thread with the
// same t=0 as the animation. No sound card yet -> silently no sound.

#include "sfx.h"
#include "splash_anim.h"

#include <tinyalsa/asoundlib.h>
#include <pthread.h>

#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

#define PROGRESS_FILE "/run/splash.progress"
#define STOP_FILE "/run/splash.stop"
#define INTRO_SECONDS 1.6f	// let the letters land before we may exit
#define FRAME_NS (1000000000L / 30)

#define BACKLIGHT_DIR "/sys/class/backlight/backlight/"
#define AUDIO_RATE 48000
#define AUDIO_PERIOD 512
#define JINGLE_SECONDS 3.2f

static volatile sig_atomic_t stop;

static void sysfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return;
	if (write(fd, val, strlen(val)) < 0)
		perror(path);
	close(fd);
}

// The backlight starts powered off (no default-on in the device tree). Its
// power state gates it independently of brightness, so set both.
static void backlight_on(void)
{
	sysfs_write(BACKLIGHT_DIR "bl_power", "0");
	sysfs_write(BACKLIGHT_DIR "brightness", "1");
}

// Route the codec to the speaker/headphone mux at a sane volume. Controls that
// don't exist are skipped, so this is safe across driver versions.
static void mixer_setup(void)
{
	struct mixer *m = mixer_open(0);
	if (!m)
		return;
	static const struct { const char *name; int pct; } on[] = {
		{ "Line Out Playback Switch", 100 },
		{ "Line Out Playback Volume", 90 },
		{ "DAC Playback Switch", 100 },
		{ "DAC Playback Volume", 90 },
		{ "Speaker Playback Switch", 100 },
	};
	for (unsigned i = 0; i < sizeof on / sizeof on[0]; i++) {
		struct mixer_ctl *c = mixer_get_ctl_by_name(m, on[i].name);
		if (!c)
			continue;
		for (unsigned v = 0; v < mixer_ctl_get_num_values(c); v++) {
			if (mixer_ctl_get_type(c) == MIXER_CTL_TYPE_BOOL)
				mixer_ctl_set_value(c, v, 1);
			else
				mixer_ctl_set_percent(c, v, on[i].pct);
		}
	}
	mixer_close(m);
}

static void *audio_thread(void *arg)
{
	(void)arg;
	struct pcm_config cfg = {
		.channels = 2,
		.rate = AUDIO_RATE,
		.period_size = AUDIO_PERIOD,
		.period_count = 4,
		.format = PCM_FORMAT_S16_LE,
	};
	struct pcm *pcm = pcm_open(0, 0, PCM_OUT, &cfg);
	if (!pcm || !pcm_is_ready(pcm)) {
		if (pcm)
			pcm_close(pcm);
		return NULL;
	}
	mixer_setup();
	sfx_init(AUDIO_RATE);
	sfx_set_volume(0.6f);
	sfx_play(SFX_BOOT, SFX_BOOT_DEFAULT);

	static int16_t mono[AUDIO_PERIOD], stereo[AUDIO_PERIOD * 2];
	int total = (int)(JINGLE_SECONDS * AUDIO_RATE);
	for (int done = 0; done < total && !stop; done += AUDIO_PERIOD) {
		sfx_render(mono, AUDIO_PERIOD);
		for (int i = 0; i < AUDIO_PERIOD; i++)
			stereo[2 * i] = stereo[2 * i + 1] = mono[i];
		if (pcm_writei(pcm, stereo, AUDIO_PERIOD) < 0)
			break;
	}
	pcm_close(pcm);
	return NULL;
}

static void on_signal(int sig)
{
	(void)sig;
	stop = 1;
}

static double now(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static float read_progress(float last)
{
	char buf[16];
	int fd = open(PROGRESS_FILE, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return last;
	ssize_t n = read(fd, buf, sizeof buf - 1);
	close(fd);
	if (n <= 0)
		return last;
	buf[n] = 0;
	float p = strtof(buf, NULL) / 100.0f;
	if (p < 0)
		p = 0;
	if (p > 1)
		p = 1;
	return p > last ? p : last;	// never move backwards
}

static void console_graphics(int on)
{
	int fd = open("/dev/tty0", O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return;
	ioctl(fd, KDSETMODE, on ? KD_GRAPHICS : KD_TEXT);
	close(fd);
}

int main(int argc, char **argv)
{
	float timeout = argc > 1 ? strtof(argv[1], NULL) : 20.0f;
	double t0 = now();

	signal(SIGTERM, on_signal);
	signal(SIGINT, on_signal);

	int fb = open("/dev/fb0", O_RDWR | O_CLOEXEC);
	if (fb < 0) {
		perror("/dev/fb0");
		return 1;
	}
	struct fb_var_screeninfo var;
	struct fb_fix_screeninfo fix;
	if (ioctl(fb, FBIOGET_VSCREENINFO, &var) || ioctl(fb, FBIOGET_FSCREENINFO, &fix)) {
		perror("fb ioctl");
		return 1;
	}
	if (var.bits_per_pixel != 32) {
		fprintf(stderr, "splash: need 32 bpp, got %u\n", var.bits_per_pixel);
		return 1;
	}
	int w = var.xres, h = var.yres, stride = fix.line_length / 4;
	size_t size = (size_t)fix.line_length * h;
	uint32_t *fbmem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fb, 0);
	if (fbmem == MAP_FAILED) {
		perror("mmap fb");
		return 1;
	}
	// Render off-screen, then copy: no half-drawn frames on screen.
	uint32_t *back = malloc(size);
	if (!back)
		return 1;

	console_graphics(1);

	pthread_t audio;
	int have_audio = pthread_create(&audio, NULL, audio_thread, NULL) == 0;

	float progress = 0;
	int frames = 0;
	double first_frame = 0;
	struct timespec next;
	clock_gettime(CLOCK_MONOTONIC, &next);

	while (!stop) {
		float t = (float)(now() - t0);
		progress = read_progress(progress);

		splash_render(back, w, h, stride, t, progress);
		__u32 crtc = 0;
		ioctl(fb, FBIO_WAITFORVSYNC, &crtc);	// best effort: avoid tearing
		memcpy(fbmem, back, size);
		if (!frames++) {
			backlight_on();
			first_frame = now() - t0;
		}

		if (t >= timeout || access(STOP_FILE, F_OK) == 0 ||
		    (progress >= 1.0f && t >= INTRO_SECONDS))
			break;

		// 30 fps pacing
		next.tv_nsec += FRAME_NS;
		if (next.tv_nsec >= 1000000000L) {
			next.tv_nsec -= 1000000000L;
			next.tv_sec++;
		}
		while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR && !stop)
			;
	}

	// Let the jingle finish (it is short) before exiting.
	if (have_audio)
		pthread_join(audio, NULL);

	// Leave the last frame on screen and the console in graphics mode:
	// the next program takes over the display without a flash.
	fprintf(stderr, "splash: %d frames, first after %.1f ms, ran %.2f s\n",
		frames, first_frame * 1000, now() - t0);
	munmap(fbmem, size);
	close(fb);
	return 0;
}
