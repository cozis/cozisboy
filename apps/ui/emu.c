// GBA emulation on libmgba. See emu.h.
#include <mgba/flags.h> // must come first: libmgba's struct layouts depend on it
#include <mgba/core/blip_buf.h>
#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/vfs.h>

#include "emu.h"
#include "ui.h"

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OUT_RATE 48000
#define MAX_GROUP 4

struct group;

struct emu_game {
	struct mCore *core;
	color_t video[EMU_W * EMU_H];
	uint32_t out[EMU_W * EMU_H];
	struct group *grp;
	// link cable (lockstep) state, as in mGBA's MultiplayerController
	struct GBASIOLockstepNode node;
	int awake;
	int32_t cycles_posted;
	unsigned wait_mask;
};

struct group {
	struct GBASIOLockstep cable; // first member: callbacks cast mLockstep* back to the group
	emu_game *g[MAX_GROUP];
	int n;
};

static int ready;
static _Atomic float volume = 0.8f;
static int color_fix;

/* ---- logging: errors only ------------------------------------------------------ */

static void quiet_log(struct mLogger *l, int cat, enum mLogLevel level, const char *fmt, va_list ap)
{
	(void)l, (void)cat;
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		fprintf(stderr, "[mgba] ");
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
}
static struct mLogger quiet = {quiet_log, NULL};

int emu_init(void)
{
	mLogSetDefaultLogger(&quiet);
	ready = 1;
	return 0;
}

int emu_available(void)
{
	return ready;
}

/* ---- audio ring (main thread writes 48 kHz mono, audio thread reads) ---------- */

#define RING 16384
#define TARGET_FILL 3200 // ~65 ms
static int16_t ring[RING];
static atomic_uint r_head, r_tail;
static atomic_int audio_live;	// set once the audio thread pulls samples
static atomic_uint emu_frames_run, underruns;	// for the stutter log

unsigned emu_audio_underruns(int *fill)
{
	if (fill)
		*fill = (int)(atomic_load(&r_head) - atomic_load(&r_tail));
	return atomic_exchange(&underruns, 0);
}

static void push_audio(emu_game *g, int keep)
{
	struct blip_t *l = g->core->getAudioChannel(g->core, 0);
	struct blip_t *r = g->core->getAudioChannel(g->core, 1);
	int16_t bl[1024], br[1024];
	for (;;) {
		int n = blip_samples_avail(l);
		if (n <= 0)
			break;
		if (n > 1024)
			n = 1024;
		blip_read_samples(l, bl, n, 0);
		blip_read_samples(r, br, n, 0);
		if (!keep)
			continue;
		unsigned h = atomic_load_explicit(&r_head, memory_order_relaxed);
		unsigned fill = h - atomic_load(&r_tail);
		for (int i = 0; i < n; i++) {
			// the emulator and the sound card clocks drift: drop a sample now
			// and then when the buffer runs too full
			if (fill > 2 * TARGET_FILL && (i & 31) == 0)
				continue;
			if (fill >= RING)
				break;
			ring[h++ % RING] = (int16_t)((bl[i] + br[i]) / 2);
			fill++;
		}
		atomic_store_explicit(&r_head, h, memory_order_release);
	}
}

void emu_audio_mix(int16_t *out, int frames, int rate)
{
	(void)rate; // produced at 48 kHz, like the UI sounds
	atomic_store(&audio_live, 1);
	float v = atomic_load(&volume);
	unsigned t = atomic_load_explicit(&r_tail, memory_order_relaxed);
	unsigned h = atomic_load_explicit(&r_head, memory_order_acquire);
	int i;
	for (i = 0; i < frames && t != h; i++, t++) {
		int s = out[i] + (int)(ring[t % RING] * v);
		out[i] = (int16_t)(s > 32767 ? 32767 : s < -32768 ? -32768 : s);
	}
	// ran dry while a game is running (it ran frames since the last pull)
	static unsigned last_frames;
	unsigned fr = atomic_load(&emu_frames_run);
	if (i < frames && fr != last_frames)
		atomic_fetch_add(&underruns, (unsigned)(frames - i));
	last_frames = fr;
	atomic_store_explicit(&r_tail, t, memory_order_release);
}

int emu_frames_due(float dt)
{
	static double acc;
	double rate = 59.7275;	// GBA frames per second
	if (atomic_load(&audio_live)) {
		int fill = (int)(atomic_load(&r_head) - atomic_load(&r_tail));
		double e = (double)(TARGET_FILL - fill) / TARGET_FILL;
		if (e > 1) e = 1;
		if (e < -1) e = -1;
		rate *= 1 + 0.005 * e;
		if (fill < TARGET_FILL / 4)	// nearly dry (just started, or a hiccup): catch up
			rate *= 1.05;
	}
	acc += dt * rate;
	int n = (int)acc;
	if (n > 4) {	// a long stall (loading, menu): don't race to catch up
		n = 4;
		acc = 0;
	}
	acc -= n;
	return n;
}

void emu_set_volume(float v)
{
	atomic_store(&volume, v);
}

/* ---- games ---------------------------------------------------------------------- */

static int copy_file(const char *from, const char *to)
{
	FILE *a = fopen(from, "rb");
	if (!a)
		return -1;
	FILE *b = fopen(to, "wb");
	if (!b) {
		fclose(a);
		return -1;
	}
	char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof buf, a)) > 0)
		fwrite(buf, 1, n, b);
	fclose(a);
	fclose(b);
	return 0;
}

emu_game *emu_open(const char *rom, const char *sav_in, const char *sav_out)
{
	emu_game *g = calloc(1, sizeof *g);
	if (!g)
		return NULL;
	g->core = mCoreFind(rom);
	if (!g->core || !g->core->init(g->core)) {
		free(g);
		return NULL;
	}
	mCoreInitConfig(g->core, NULL);
	g->core->setVideoBuffer(g->core, g->video, EMU_W);
	g->core->setAudioBufferSize(g->core, 2048);
	if (!mCoreLoadFile(g->core, rom)) {
		g->core->deinit(g->core);
		free(g);
		return NULL;
	}
	blip_set_rates(g->core->getAudioChannel(g->core, 0), g->core->frequency(g->core), OUT_RATE);
	blip_set_rates(g->core->getAudioChannel(g->core, 1), g->core->frequency(g->core), OUT_RATE);
	// battery save: mGBA writes it straight to sav_out as the game saves
	if (sav_out) {
		if (access(sav_out, F_OK) != 0 && sav_in)
			copy_file(sav_in, sav_out);
		struct VFile *vf = VFileOpen(sav_out, O_CREAT | O_RDWR);
		if (vf)
			g->core->loadSave(g->core, vf);
	}
	g->core->reset(g->core);
	g->awake = 1;
	return g;
}

static void unlink_game(emu_game *g);

void emu_close(emu_game *g)
{
	if (!g)
		return;
	unlink_game(g);
	g->core->unloadROM(g->core); // flushes the battery save
	g->core->deinit(g->core);
	free(g);
}

void emu_reset(emu_game *g)
{
	g->core->reset(g->core);
}

size_t emu_state_size(emu_game *g)
{
	return g->core->stateSize(g->core);
}

int emu_save_state(emu_game *g, void *buf, size_t size)
{
	return size >= emu_state_size(g) && g->core->saveState(g->core, buf) ? 0 : -1;
}

int emu_load_state(emu_game *g, const void *buf, size_t size)
{
	return size >= emu_state_size(g) && g->core->loadState(g->core, buf) ? 0 : -1;
}

/* ---- colour ---------------------------------------------------------------------- */

// GBA screen colour correction as a 15-bit -> XRGB table. The GBA's LCD showed
// colours darker and less saturated than they're stored; this approximates it
// in linear light.
static uint32_t lut_fix[32768], lut_raw[32768];

static void build_luts(void)
{
	static const float m[3][3] = {{0.82f, 0.24f, -0.06f}, {0.13f, 0.67f, 0.20f}, {0.17f, 0.17f, 0.66f}};
	for (int i = 0; i < 32768; i++) {
		int c5[3] = {i & 31, (i >> 5) & 31, (i >> 10) & 31};
		float lin[3], o[3];
		for (int k = 0; k < 3; k++)
			lin[k] = powf(c5[k] / 31.0f, 2.2f);
		for (int k = 0; k < 3; k++) {
			o[k] = m[k][0] * lin[0] + m[k][1] * lin[1] + m[k][2] * lin[2];
			o[k] = powf(o[k] < 0 ? 0 : o[k] > 1 ? 1 : o[k], 1 / 2.2f) * 0.94f;
		}
		lut_fix[i] = (uint32_t)(o[0] * 255) << 16 | (uint32_t)(o[1] * 255) << 8 | (uint32_t)(o[2] * 255);
		int r = c5[0] << 3 | c5[0] >> 2, gg = c5[1] << 3 | c5[1] >> 2, b = c5[2] << 3 | c5[2] >> 2;
		lut_raw[i] = (uint32_t)(r << 16 | gg << 8 | b);
	}
}

void emu_set_color_fix(int on)
{
	color_fix = on;
}

const uint32_t *emu_frame(emu_game *g)
{
	static int luts;
	if (!luts)
		build_luts(), luts = 1;
	const uint32_t *lut = color_fix ? lut_fix : lut_raw;
	for (int i = 0; i < EMU_W * EMU_H; i++) {
		color_t c = g->video[i]; // mGBA: XBGR8888, 5-bit channels in each byte's top bits
		unsigned idx = ((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10);
		g->out[i] = lut[idx];
	}
	return g->out;
}

/* ---- link cable: lockstep callbacks (one thread: wait/wake are flags) ----------- */

static struct group *cable_group(struct mLockstep *l)
{
	return (struct group *)l;
}

static emu_game *member(struct group *gr, int id)
{
	for (int i = 0; i < gr->n; i++)
		if (gr->g[i]->node.id == id)
			return gr->g[i];
	return gr->g[0];
}

static bool ls_signal(struct mLockstep *l, unsigned mask)
{
	emu_game *p = member(cable_group(l), 0);
	p->wait_mask &= ~mask;
	if (!p->wait_mask && p->awake < 1) {
		p->awake = 1;
		return true;
	}
	return false;
}

static bool ls_wait(struct mLockstep *l, unsigned mask)
{
	emu_game *p = member(cable_group(l), 0);
	p->wait_mask |= mask;
	if (p->awake > 0) {
		p->awake = 0;
		return true;
	}
	return false;
}

static void ls_add_cycles(struct mLockstep *l, int id, int32_t cycles)
{
	struct group *gr = cable_group(l);
	if (cycles < 0)
		return;
	if (id) {
		member(gr, id)->cycles_posted += cycles;
		return;
	}
	for (int i = 1; i < gr->n; i++) {
		emu_game *p = member(gr, i);
		if (p->node.d.p->mode > SIO_MULTI)
			continue;
		p->cycles_posted += cycles;
		if (p->awake < 1)
			p->node.nextEvent += p->cycles_posted;
		p->awake = 1;
	}
}

static int32_t ls_use_cycles(struct mLockstep *l, int id, int32_t cycles)
{
	emu_game *p = member(cable_group(l), id);
	p->cycles_posted -= cycles;
	if (p->cycles_posted <= 0)
		p->awake = 0;
	return p->cycles_posted;
}

static int32_t ls_unused_cycles(struct mLockstep *l, int id)
{
	return member(cable_group(l), id)->cycles_posted;
}

static void ls_unload(struct mLockstep *l, int id)
{
	struct group *gr = cable_group(l);
	if (id) {
		member(gr, id)->cycles_posted = 0;
		emu_game *m = member(gr, 0); // release the master if it waits for this one
		m->wait_mask &= ~(1u << id);
		if (!m->wait_mask && m->awake < 1)
			m->awake = 1;
		return;
	}
	for (int i = 1; i < gr->n; i++) {
		emu_game *p = member(gr, i);
		p->cycles_posted += gr->cable.players[0]->eventDiff;
		if (p->awake < 1) {
			p->node.nextEvent += p->cycles_posted;
			p->awake = 1;
		}
	}
}

static void attach(struct group *gr, emu_game *g)
{
	struct GBA *gba = g->core->board;
	GBASIOLockstepNodeCreate(&g->node);
	GBASIOLockstepAttachNode(&gr->cable, &g->node);
	GBASIOSetDriver(&gba->sio, &g->node.d, SIO_MULTI);
	GBASIOSetDriver(&gba->sio, &g->node.d, SIO_NORMAL_32);
	gr->g[gr->n++] = g;
	g->grp = gr;
}

int emu_link(emu_game *a, emu_game *b)
{
	if (b->grp || (a->grp && a->grp->n >= MAX_GROUP))
		return -1;
	if (!a->grp) {
		struct group *gr = calloc(1, sizeof *gr);
		if (!gr)
			return -1;
		GBASIOLockstepInit(&gr->cable);
		gr->cable.d.signal = ls_signal;
		gr->cable.d.wait = ls_wait;
		gr->cable.d.addCycles = ls_add_cycles;
		gr->cable.d.useCycles = ls_use_cycles;
		gr->cable.d.unusedCycles = ls_unused_cycles;
		gr->cable.d.unload = ls_unload;
		attach(gr, a);
	}
	attach(a->grp, b);
	return 0;
}

static void unlink_game(emu_game *g)
{
	struct group *gr = g->grp;
	if (!gr)
		return;
	struct GBA *gba = g->core->board;
	GBASIOSetDriver(&gba->sio, NULL, SIO_MULTI);
	GBASIOSetDriver(&gba->sio, NULL, SIO_NORMAL_32);
	GBASIOLockstepDetachNode(&gr->cable, &g->node);
	for (int i = 0; i < gr->n; i++)
		if (gr->g[i] == g) {
			memmove(&gr->g[i], &gr->g[i + 1], (size_t)(gr->n - i - 1) * sizeof *gr->g);
			gr->n--;
			break;
		}
	g->grp = NULL;
	g->awake = 1;
	if (gr->n <= 1) { // a group of one is no group
		for (int i = 0; i < gr->n; i++) {
			emu_game *o = gr->g[i];
			struct GBA *ogba = o->core->board;
			GBASIOSetDriver(&ogba->sio, NULL, SIO_MULTI);
			GBASIOSetDriver(&ogba->sio, NULL, SIO_NORMAL_32);
			GBASIOLockstepDetachNode(&gr->cable, &o->node);
			o->grp = NULL, o->awake = 1;
		}
		free(gr);
	}
}

int emu_linked(emu_game *g)
{
	return g && g->grp ? g->grp->n : 1;
}

int emu_same_group(emu_game *a, emu_game *b)
{
	return a && b && a != b && a->grp && a->grp == b->grp;
}

void emu_unlink(emu_game *g)
{
	unlink_game(g);
}

const uint32_t *emu_frame_peek(emu_game *g)
{
	return g->out;
}

/* ---- running ----------------------------------------------------------------------- */

static unsigned gba_keys(unsigned b)
{
	unsigned k = 0;
	if (b & BTN_A) k |= 1 << 0;
	if (b & BTN_B) k |= 1 << 1;
	if (b & BTN_SELECT) k |= 1 << 2;
	if (b & BTN_START) k |= 1 << 3;
	if (b & BTN_RIGHT) k |= 1 << 4;
	if (b & BTN_LEFT) k |= 1 << 5;
	if (b & BTN_UP) k |= 1 << 6;
	if (b & BTN_DOWN) k |= 1 << 7;
	if (b & BTN_R) k |= 1 << 8;
	if (b & BTN_L) k |= 1 << 9;
	return k;
}

// One frame of a link group: interleave the cores in runLoop() slices. Player 0
// (the link master) runs while awake; the others run while awake and behind, or
// while the master waits on them.
static void run_group_frame(struct group *gr)
{
	emu_game *m = member(gr, 0);
	uint32_t target = m->core->frameCounter(m->core) + 1;
	for (int guard = 0; guard < 4000000; guard++) {
		int ran = 0;
		if (m->awake && m->core->frameCounter(m->core) < target)
			m->core->runLoop(m->core), ran = 1;
		for (int i = 1; i < gr->n; i++) {
			emu_game *p = member(gr, i);
			int needed = m->wait_mask & (1u << p->node.id);
			if (p->awake && (needed || p->core->frameCounter(p->core) < target))
				p->core->runLoop(p->core), ran = 1;
		}
		int done = m->core->frameCounter(m->core) >= target;
		for (int i = 1; done && i < gr->n; i++) {
			emu_game *p = member(gr, i);
			if (p->awake && p->core->frameCounter(p->core) < target && p->node.d.p->mode > SIO_MULTI)
				done = 0; // unlinked-mode followers keep their own pace
		}
		if (done || !ran)
			break;
	}
}

void emu_run(emu_game *g, unsigned buttons, int frames)
{
	g->core->setKeys(g->core, gba_keys(buttons));
	if (g->grp) // the other linked games get no input while you play this one
		for (int i = 0; i < g->grp->n; i++)
			if (g->grp->g[i] != g)
				g->grp->g[i]->core->setKeys(g->grp->g[i]->core, 0);
	atomic_fetch_add(&emu_frames_run, (unsigned)frames);
	for (int f = 0; f < frames; f++) {
		if (g->grp)
			run_group_frame(g->grp);
		else
			g->core->runFrame(g->core);
		push_audio(g, f == 0);
		if (g->grp) // linked games make sound too; only g's is heard
			for (int i = 0; i < g->grp->n; i++)
				if (g->grp->g[i] != g)
					push_audio(g->grp->g[i], 0);
	}
}
