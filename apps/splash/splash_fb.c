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
// The boot jingle is played by a separate helper (cozisboy-chime, alsa-lib)
// started at the animation's t=0, so this binary can stay static.
//
// Started as PID 1 (kernel init=/sbin/preinit, a link to this binary, statically
// linked so a cold start reads one file), it mounts /sys and /run, forks, hands
// PID 1 to busybox init in the parent and keeps animating in the child.

#include "splash_anim.h"

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
#include <sys/mount.h>
#include <unistd.h>

#ifndef FBIO_WAITFORVSYNC
#define FBIO_WAITFORVSYNC _IOW('F', 0x20, __u32)
#endif

#define PROGRESS_FILE "/run/splash.progress"
#define STOP_FILE "/run/splash.stop"
#define INTRO_SECONDS 1.6f	// let the letters land before we may exit
#define FRAME_NS (1000000000L / 30)

#define BACKLIGHT_DIR "/sys/class/backlight/backlight/"
#define CHIME "/usr/bin/cozisboy-chime"

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

// The backlight starts powered off. Power it on at its default level (PWM
// backlight: default-brightness-level from the device tree); if no level is
// set (brightness 0), use the maximum.
static void backlight_on(void)
{
	char buf[16] = "";
	int fd = open(BACKLIGHT_DIR "brightness", O_RDONLY | O_CLOEXEC);
	if (fd >= 0) {
		if (read(fd, buf, sizeof buf - 1) < 0)
			buf[0] = 0;
		close(fd);
	}
	if (atoi(buf) == 0) {
		fd = open(BACKLIGHT_DIR "max_brightness", O_RDONLY | O_CLOEXEC);
		if (fd >= 0) {
			ssize_t n = read(fd, buf, sizeof buf - 1);
			buf[n > 0 ? n : 0] = 0;
			close(fd);
			sysfs_write(BACKLIGHT_DIR "brightness", buf);
		}
	}
	sysfs_write(BACKLIGHT_DIR "bl_power", "0");
}

// Start the boot jingle in its own process (fire and forget).
static void start_chime(void)
{
	pid_t pid = fork();
	if (pid == 0) {
		execl(CHIME, CHIME, (char *)NULL);
		_exit(127);
	}
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

// As PID 1: mount what the splash needs, then let busybox init take over PID 1
// while the splash continues in a child process.
static void preinit(char **argv)
{
	mount("sysfs", "/sys", "sysfs", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
	mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV, "mode=0755");
	pid_t pid = fork();
	if (pid != 0) {	// parent (or fork failed): become the real init
		argv[0] = "init";
		execv("/sbin/init", argv);
		_exit(1);
	}
	setsid();
	int log = open("/run/splash.log", O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (log >= 0) {
		dup2(log, 2);
		close(log);
	}
}

int main(int argc, char **argv)
{
	int as_init = getpid() == 1;
	if (as_init)
		preinit(argv);
	float timeout = !as_init && argc > 1 ? strtof(argv[1], NULL) : 20.0f;
	double t0 = now();
	// Boot-time accounting: arch counter runs from SoC reset at CNTFRQ.
	uint64_t cnt, frq;
	__asm__ volatile("isb; mrs %0, cntvct_el0" : "=r"(cnt));
	__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(frq));
	struct timespec bt;
	clock_gettime(CLOCK_BOOTTIME, &bt);
	double t0_reset = (double)cnt / frq, t0_boot = bt.tv_sec + bt.tv_nsec / 1e9;

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
	// Without a framebuffer console nothing has switched the display on yet:
	// force the mode to be applied (DRM modeset: TCON + panel power-up) and
	// unblank, otherwise the picture would sit in memory, invisible.
	double ms0 = now();
	var.activate = FB_ACTIVATE_NOW | FB_ACTIVATE_FORCE;
	if (ioctl(fb, FBIOPUT_VSCREENINFO, &var))
		perror("FBIOPUT_VSCREENINFO");
	if (ioctl(fb, FBIOBLANK, FB_BLANK_UNBLANK))
		perror("FBIOBLANK");
	double modeset_ms = (now() - ms0) * 1000;

	// Render off-screen, then copy: no half-drawn frames on screen.
	uint32_t *back = malloc(size);
	if (!back)
		return 1;

	console_graphics(1);

	start_chime();

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

	// Leave the last frame on screen and the console in graphics mode:
	// the next program takes over the display without a flash.
	fprintf(stderr, "splash: %d frames, first after %.1f ms (display on: %.1f ms), ran %.2f s\n",
		frames, first_frame * 1000, modeset_ms, now() - t0);
	fprintf(stderr, "splash: started %.3f s after reset (%.3f s after kernel), first frame %.3f s after reset\n",
		t0_reset, t0_boot, t0_reset + first_frame);
	munmap(fbmem, size);
	close(fb);
	return 0;
}
