#include "emu.h"

#include "libretro_min.h"
#include "ui.h"

#include <dlfcn.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- core entry points ---------------------------------------------------- */

static struct {
	void *so;
	void (*init)(void);
	unsigned (*api_version)(void);
	void (*get_system_info)(struct retro_system_info *);
	void (*get_system_av_info)(struct retro_system_av_info *);
	void (*set_environment)(retro_environment_t);
	void (*set_video_refresh)(retro_video_refresh_t);
	void (*set_audio_sample)(retro_audio_sample_t);
	void (*set_audio_sample_batch)(retro_audio_sample_batch_t);
	void (*set_input_poll)(retro_input_poll_t);
	void (*set_input_state)(retro_input_state_t);
	void (*set_controller_port_device)(unsigned, unsigned);
	void (*reset)(void);
	void (*run)(void);
	size_t (*serialize_size)(void);
	bool (*serialize)(void *, size_t);
	bool (*unserialize)(const void *, size_t);
	bool (*load_game)(const struct retro_game_info *);
	void (*unload_game)(void);
	void *(*get_memory_data)(unsigned);
	size_t (*get_memory_size)(unsigned);
} core;

static int ready, loaded, need_fullpath;
static enum retro_pixel_format pixfmt = RETRO_PIXEL_FORMAT_0RGB1555;
static uint32_t frame[EMU_W * EMU_H];
static unsigned cur_buttons;
static int keep_audio;
static double core_rate = 32768;
static void *rom_data;

/* ---- audio ring (main thread writes, audio thread reads) ------------------- */

#define RING 16384
static int16_t ring[RING];
static atomic_uint r_head, r_tail;
static _Atomic float volume = 0.8f;
static double rs_pos; // resampler phase
static int16_t rs_last;
#define OUT_RATE 48000.0
#define TARGET_FILL 3200 // ~65 ms at 48 kHz

static unsigned ring_fill(void)
{
	return atomic_load(&r_head) - atomic_load(&r_tail);
}

// Linear resample stereo core audio to mono OUT_RATE, nudging the ratio to keep
// the ring near TARGET_FILL (core and display clocks differ slightly).
static void push_audio(const int16_t *data, size_t frames)
{
	if (!keep_audio)
		return;
	unsigned fill = ring_fill();
	double adjust = 1.0 + 0.005 * ((double)TARGET_FILL - fill) / TARGET_FILL;
	double step = core_rate / (OUT_RATE * adjust);
	unsigned h = atomic_load_explicit(&r_head, memory_order_relaxed);
	for (;;) {
		size_t i = (size_t)rs_pos;
		if (i >= frames)
			break;
		int16_t cur = (int16_t)((data[2 * i] + data[2 * i + 1]) / 2);
		int16_t prev = i ? (int16_t)((data[2 * i - 2] + data[2 * i - 1]) / 2) : rs_last;
		double f = rs_pos - (double)i;
		int16_t s = (int16_t)(prev + (cur - prev) * f);
		if (h - atomic_load(&r_tail) < RING)
			ring[h++ % RING] = s;
		rs_pos += step;
	}
	rs_pos -= (double)frames;
	rs_last = (int16_t)((data[2 * frames - 2] + data[2 * frames - 1]) / 2);
	atomic_store_explicit(&r_head, h, memory_order_release);
}

void emu_audio_mix(int16_t *out, int frames, int rate)
{
	(void)rate; // the ring is produced at OUT_RATE (48 kHz), like the UI sounds
	float v = atomic_load(&volume);
	unsigned t = atomic_load_explicit(&r_tail, memory_order_relaxed);
	unsigned h = atomic_load_explicit(&r_head, memory_order_acquire);
	for (int i = 0; i < frames && t != h; i++, t++) {
		int s = out[i] + (int)(ring[t % RING] * v);
		out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
	atomic_store_explicit(&r_tail, t, memory_order_release);
}

void emu_set_volume(float v)
{
	atomic_store(&volume, v);
}

/* ---- core options (answers the core's GET_VARIABLE queries) ---------------- */

#define MAXOPT 16
static struct {
	char key[48], val[32];
} opts[MAXOPT];
static int nopts, opts_dirty;

void emu_set_option(const char *key, const char *val)
{
	int i = 0;
	while (i < nopts && strcmp(opts[i].key, key))
		i++;
	if (i == nopts) {
		if (nopts == MAXOPT)
			return;
		nopts++;
	}
	snprintf(opts[i].key, sizeof opts[i].key, "%s", key);
	snprintf(opts[i].val, sizeof opts[i].val, "%s", val);
	opts_dirty = 1;
}

static const char *get_option(const char *key)
{
	for (int i = 0; i < nopts; i++)
		if (!strcmp(opts[i].key, key))
			return opts[i].val;
	return NULL; // not set: the core keeps its default
}

/* ---- callbacks ------------------------------------------------------------- */

static void core_log(enum retro_log_level level, const char *fmt, ...)
{
	if (level < RETRO_LOG_WARN)
		return;
	va_list ap;
	va_start(ap, fmt);
	fprintf(stderr, "[core] ");
	vfprintf(stderr, fmt, ap);
	va_end(ap);
}

static bool environment(unsigned cmd, void *data)
{
	static const char *dir = NULL;
	if (!dir)
		dir = getenv("COZ_SAVES") ? getenv("COZ_SAVES") : "saves";
	switch (cmd) {
	case RETRO_ENVIRONMENT_GET_CAN_DUPE:
		*(bool *)data = true;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
		enum retro_pixel_format f = *(const enum retro_pixel_format *)data;
		if (f != RETRO_PIXEL_FORMAT_XRGB8888 && f != RETRO_PIXEL_FORMAT_RGB565 &&
		    f != RETRO_PIXEL_FORMAT_0RGB1555)
			return false;
		pixfmt = f;
		return true;
	}
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = dir;
		return true;
	case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
		((struct retro_log_callback *)data)->log = core_log;
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE: {
		struct retro_variable *v = data;
		v->value = v->key ? get_option(v->key) : NULL;
		return v->value != NULL;
	}
	case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
		*(bool *)data = opts_dirty;
		opts_dirty = 0;
		return true;
	}
	return false; // everything else: not supported, core uses its defaults
}

static void video_refresh(const void *data, unsigned w, unsigned h, size_t pitch)
{
	if (!data) // duplicate of the previous frame
		return;
	if (w > EMU_W)
		w = EMU_W;
	if (h > EMU_H)
		h = EMU_H;
	for (unsigned y = 0; y < h; y++) {
		const uint8_t *row = (const uint8_t *)data + y * pitch;
		uint32_t *out = frame + y * EMU_W;
		if (pixfmt == RETRO_PIXEL_FORMAT_XRGB8888) {
			memcpy(out, row, w * 4);
			continue;
		}
		const uint16_t *p = (const uint16_t *)row;
		for (unsigned x = 0; x < w; x++) {
			unsigned v = p[x], r, g, b;
			if (pixfmt == RETRO_PIXEL_FORMAT_RGB565)
				r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31, g = (g << 2) | (g >> 4);
			else
				r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31, g = (g << 3) | (g >> 2);
			r = (r << 3) | (r >> 2), b = (b << 3) | (b >> 2);
			out[x] = r << 16 | g << 8 | b;
		}
	}
}

static void audio_sample(int16_t l, int16_t r)
{
	int16_t s[2] = {l, r};
	push_audio(s, 1);
}

static size_t audio_batch(const int16_t *data, size_t frames)
{
	if (frames)
		push_audio(data, frames);
	return frames;
}

static void input_poll(void)
{
}

static int16_t input_state(unsigned port, unsigned device, unsigned index, unsigned id)
{
	(void)index;
	if (port || device != RETRO_DEVICE_JOYPAD)
		return 0;
	unsigned b = cur_buttons;
	switch (id) {
	case RETRO_DEVICE_ID_JOYPAD_A: return (b & BTN_A) != 0;
	case RETRO_DEVICE_ID_JOYPAD_B: return (b & BTN_B) != 0;
	case RETRO_DEVICE_ID_JOYPAD_X: return (b & BTN_X) != 0;
	case RETRO_DEVICE_ID_JOYPAD_Y: return (b & BTN_Y) != 0;
	case RETRO_DEVICE_ID_JOYPAD_L: return (b & BTN_L) != 0;
	case RETRO_DEVICE_ID_JOYPAD_R: return (b & BTN_R) != 0;
	case RETRO_DEVICE_ID_JOYPAD_START: return (b & BTN_START) != 0;
	case RETRO_DEVICE_ID_JOYPAD_SELECT: return (b & BTN_SELECT) != 0;
	case RETRO_DEVICE_ID_JOYPAD_UP: return (b & BTN_UP) != 0;
	case RETRO_DEVICE_ID_JOYPAD_DOWN: return (b & BTN_DOWN) != 0;
	case RETRO_DEVICE_ID_JOYPAD_LEFT: return (b & BTN_LEFT) != 0;
	case RETRO_DEVICE_ID_JOYPAD_RIGHT: return (b & BTN_RIGHT) != 0;
	}
	return 0;
}

/* ---- public API -------------------------------------------------------------- */

#define SYM(name)                                                            \
	do {                                                                 \
		*(void **)&core.name = dlsym(core.so, "retro_" #name);       \
		if (!core.name) {                                            \
			fprintf(stderr, "emu: core lacks retro_%s\n", #name); \
			return -1;                                           \
		}                                                            \
	} while (0)

int emu_init(const char *path)
{
	core.so = dlopen(path, RTLD_NOW | RTLD_LOCAL);
	if (!core.so) {
		fprintf(stderr, "emu: %s\n", dlerror());
		return -1;
	}
	SYM(init);
	SYM(api_version);
	SYM(get_system_info);
	SYM(get_system_av_info);
	SYM(set_environment);
	SYM(set_video_refresh);
	SYM(set_audio_sample);
	SYM(set_audio_sample_batch);
	SYM(set_input_poll);
	SYM(set_input_state);
	SYM(set_controller_port_device);
	SYM(reset);
	SYM(run);
	SYM(serialize_size);
	SYM(serialize);
	SYM(unserialize);
	SYM(load_game);
	SYM(unload_game);
	SYM(get_memory_data);
	SYM(get_memory_size);
	if (core.api_version() != RETRO_API_VERSION) {
		fprintf(stderr, "emu: unexpected libretro API version\n");
		return -1;
	}
	core.set_environment(environment);
	core.init();
	core.set_video_refresh(video_refresh);
	core.set_audio_sample(audio_sample);
	core.set_audio_sample_batch(audio_batch);
	core.set_input_poll(input_poll);
	core.set_input_state(input_state);
	struct retro_system_info si = {0};
	core.get_system_info(&si);
	need_fullpath = si.need_fullpath;
	if (getenv("COZ_DEBUG"))
		fprintf(stderr, "emu: %s %s\n", si.library_name, si.library_version);
	ready = 1;
	return 0;
}

int emu_available(void)
{
	return ready;
}

int emu_loaded(void)
{
	return loaded;
}

static void *read_file(const char *path, size_t *size)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	void *buf = n > 0 ? malloc((size_t)n) : NULL;
	if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
		free(buf);
		buf = NULL;
	}
	fclose(f);
	*size = buf ? (size_t)n : 0;
	return buf;
}

void emu_write_sav(const char *sav_path)
{
	if (!loaded || !sav_path)
		return;
	void *p = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (!p || !n)
		return;
	FILE *f = fopen(sav_path, "wb");
	if (!f)
		return;
	fwrite(p, 1, n, f);
	fclose(f);
}

void emu_unload(const char *sav_path)
{
	if (!loaded)
		return;
	emu_write_sav(sav_path);
	core.unload_game();
	free(rom_data);
	rom_data = NULL;
	loaded = 0;
}

int emu_load(const char *rom_path, const void *state, size_t state_size, const char *sav_path)
{
	if (!ready)
		return -1;
	emu_unload(NULL); // callers write the previous game's .sav themselves
	struct retro_game_info gi = {rom_path, NULL, 0, NULL};
	if (!need_fullpath) {
		rom_data = read_file(rom_path, &gi.size);
		if (!rom_data)
			return -1;
		gi.data = rom_data;
	}
	if (!core.load_game(&gi)) {
		free(rom_data);
		rom_data = NULL;
		return -1;
	}
	loaded = 1;
	struct retro_system_av_info av = {0};
	core.get_system_av_info(&av);
	if (av.timing.sample_rate > 0)
		core_rate = av.timing.sample_rate;
	core.set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
	// battery save
	size_t sn;
	void *sav = sav_path ? read_file(sav_path, &sn) : NULL;
	void *p = core.get_memory_data(RETRO_MEMORY_SAVE_RAM);
	size_t n = core.get_memory_size(RETRO_MEMORY_SAVE_RAM);
	if (sav && p && n)
		memcpy(p, sav, sn < n ? sn : n);
	free(sav);
	if (state && state_size)
		core.unserialize(state, state_size);
	memset(frame, 0, sizeof frame);
	return 0;
}

size_t emu_state_size(void)
{
	return loaded ? core.serialize_size() : 0;
}

int emu_save_state(void *buf, size_t size)
{
	return loaded && core.serialize(buf, size) ? 0 : -1;
}

int emu_load_state(const void *buf, size_t size)
{
	return loaded && core.unserialize(buf, size) ? 0 : -1;
}

void emu_reset(void)
{
	if (loaded)
		core.reset();
}

void emu_run(unsigned buttons, int frames)
{
	if (!loaded)
		return;
	cur_buttons = buttons;
	for (int i = 0; i < frames; i++) {
		keep_audio = i == 0;
		core.run();
	}
	keep_audio = 1;
}

const uint32_t *emu_frame(void)
{
	return frame;
}
