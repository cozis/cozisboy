// Prototype: two GBA emulators (libmgba) connected by an emulated link cable.
//
//   linktest <rom> [rom2]            SDL window, both screens side by side
//   LT_SCRIPT="..." linktest <rom>   headless: scripted input + screenshots
//
// Keys (window): arrows, Z = A, X = B, Enter = Start, Backspace = Select,
// Tab = switch which player the keys control. Link stats go to stderr once a
// second.
//
// Script steps: w<sec> wait; p<key><player> tap (keys: A B S=start E=select
// U D L R); s<file> save both screens side by side to <file>.ppm.
//
// The lockstep protocol follows mGBA's own MultiplayerController (Qt): player 0
// (the link master) posts cycles, the others run that far and then wait, so
// link transfers line up. Unlike mGBA's desktop app, both emulators run in ONE
// thread: "wait" and "wake" are just flags, and emulate_frame() interleaves the
// cores in small slices. No locks, no thread hand-offs (those made linked play
// 10x slower).
#include <mgba/flags.h> // must come first: struct layouts depend on the build flags
#include <mgba/core/core.h>
#include <mgba/core/lockstep.h>
#include <mgba/internal/gba/gba.h>
#include <mgba/internal/gba/sio/lockstep.h>
#include <mgba-util/vfs.h>

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define NP 2
#define GW 240
#define GH 160

struct player {
	struct mCore *core;
	struct GBASIOLockstepNode node;
	int awake;               // 0: waiting on the link (don't run it)
	int32_t cyclesPosted;
	unsigned waitMask;
	int sync;                 // throttle to real time (off while catching up)
	color_t video[GW * GH];
	volatile unsigned frames;
	double next_frame;        // throttle deadline (seconds)
};

static struct player P[NP];
static struct GBASIOLockstep cable;

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

static struct player *player(int id)
{
	for (int i = 0; i < NP; i++)
		if (P[i].node.id == id)
			return &P[i];
	return &P[0];
}

/* ---- lockstep callbacks (as in mGBA's MultiplayerController) --------------- */


static bool ls_signal(struct mLockstep *l, unsigned mask)
{
	(void)l;
	struct player *p = player(0);
	bool woke = false;
	p->waitMask &= ~mask;
	if (!p->waitMask && p->awake < 1) {
		p->awake = 1;
		woke = true;
	}
	return woke;
}

static bool ls_wait(struct mLockstep *l, unsigned mask)
{
	(void)l;
	struct player *p = player(0);
	bool slept = false;
	p->waitMask |= mask;
	if (p->awake > 0) {
		p->awake = 0;
		slept = true;
	}
	p->sync = 1;
	return slept;
}

static void ls_addCycles(struct mLockstep *l, int id, int32_t cycles)
{
	(void)l;
	if (cycles < 0)
		abort();
	if (!id) {
		for (int i = 1; i < NP; i++) {
			struct player *p = player(i);
			if (p->node.d.p->mode > SIO_MULTI) {
				p->sync = 1;
				continue;
			}
			p->sync = 0;
			p->cyclesPosted += cycles;
			if (p->awake < 1)
				p->node.nextEvent += p->cyclesPosted;
			p->awake = 1;
		}
	} else {
		struct player *p = player(id);
		p->sync = 1;
		p->cyclesPosted += cycles;
	}
}

static int32_t ls_useCycles(struct mLockstep *l, int id, int32_t cycles)
{
	(void)l;
	struct player *p = player(id);
	p->cyclesPosted -= cycles;
	if (p->cyclesPosted <= 0) {
		p->awake = 0;
	}
	return p->cyclesPosted;
}

static int32_t ls_unusedCycles(struct mLockstep *l, int id)
{
	(void)l;
	return player(id)->cyclesPosted;
}

static void ls_unload(struct mLockstep *l, int id)
{
	if (id) {
		struct player *p = player(id);
		p->sync = 1;
		p->cyclesPosted = 0;
		p = player(0); // release the master if it waits for this player
		p->waitMask &= ~(1u << id);
		if (!p->waitMask && p->awake < 1)
			p->awake = 1;
		return;
	}
	for (int i = 1; i < NP; i++) {
		struct player *p = player(i);
		p->sync = 1;
		p->cyclesPosted += ((struct GBASIOLockstep *)l)->players[0]->eventDiff;
		if (p->awake < 1) {
			p->node.nextEvent += p->cyclesPosted;
			p->awake = 1;
		}
	}
}

/* ---- emulators, one thread ---------------------------------------------------- */

// Run every core until player 0 has produced one more frame. A core runs in
// runLoop() slices (until its next scheduled event) while it's awake; followers
// also stop once they're a frame ahead, unless the master is waiting for them.
static void emulate_frame(void)
{
	uint32_t target = P[0].core->frameCounter(P[0].core) + 1;
	for (int guard = 0; guard < 2000000; guard++) {
		int ran = 0;
		if (P[0].awake && P[0].core->frameCounter(P[0].core) < target)
			P[0].core->runLoop(P[0].core), ran = 1;
		for (int i = 1; i < NP; i++) {
			struct player *p = &P[i];
			int needed = P[0].waitMask & (1u << p->node.id);
			if (p->awake && (needed || p->core->frameCounter(p->core) < target))
				p->core->runLoop(p->core), ran = 1;
		}
		int done = P[0].core->frameCounter(P[0].core) >= target;
		for (int i = 1; done && i < NP; i++)
			if (P[i].awake && P[i].core->frameCounter(P[i].core) < target &&
			    P[i].node.d.p->mode > SIO_MULTI)
				done = 0; // unlinked followers keep their own pace
		if (done)
			break;
		if (!ran) {
			fprintf(stderr, "linktest: every player is waiting (deadlock)\n");
			break;
		}
	}
	for (int i = 0; i < NP; i++)
		P[i].frames = P[i].core->frameCounter(P[i].core);
}

static int start_player(struct player *p, const char *rom, int idx)
{
	p->core = mCoreFind(rom);
	if (!p->core || !p->core->init(p->core)) {
		fprintf(stderr, "linktest: can't create core for %s\n", rom);
		return -1;
	}
	mCoreInitConfig(p->core, NULL);
	p->core->setVideoBuffer(p->core, p->video, GW);
	if (!mCoreLoadFile(p->core, rom)) {
		fprintf(stderr, "linktest: can't load %s\n", rom);
		return -1;
	}
	// each player gets its own battery save next to the test, never the original
	char sav[64];
	snprintf(sav, sizeof sav, "linktest-p%d.sav", idx + 1);
	struct VFile *vf = VFileOpen(sav, O_CREAT | O_RDWR);
	if (vf)
		p->core->loadSave(p->core, vf);
	p->core->reset(p->core);
	p->awake = 1;
	return 0;
}

static void attach_link(struct player *p)
{
	struct GBA *gba = p->core->board;
	GBASIOLockstepNodeCreate(&p->node);
	GBASIOLockstepAttachNode(&cable, &p->node);
	GBASIOSetDriver(&gba->sio, &p->node.d, SIO_MULTI);
	GBASIOSetDriver(&gba->sio, &p->node.d, SIO_NORMAL_32);
}

// Pace the main loop to the GBA's 59.73 fps (unless LT_NOTHROTTLE is set).
static void pace(void)
{
	static double next;
	static int nothrottle = -1;
	if (nothrottle < 0)
		nothrottle = getenv("LT_NOTHROTTLE") != NULL;
	if (nothrottle)
		return;
	double tnow = now_s();
	if (next == 0 || tnow - next > 0.25)
		next = tnow;
	next += 1.0 / 59.7275;
	if (next > tnow)
		usleep((useconds_t)((next - tnow) * 1e6));
}

/* ---- keys, stats, screenshots ------------------------------------------------- */

enum { K_A = 1, K_B = 2, K_SELECT = 4, K_START = 8, K_RIGHT = 16, K_LEFT = 32, K_UP = 64,
       K_DOWN = 128 };

static unsigned key_bit(char c)
{
	switch (c) {
	case 'A': return K_A;
	case 'B': return K_B;
	case 'S': return K_START;
	case 'E': return K_SELECT;
	case 'U': return K_UP;
	case 'D': return K_DOWN;
	case 'L': return K_LEFT;
	case 'R': return K_RIGHT;
	}
	return 0;
}

static uint32_t to_xrgb(color_t c) // mGBA 32-bit colour is XBGR
{
	return ((c & 0xff) << 16) | (c & 0xff00) | ((c >> 16) & 0xff);
}

static const char *mode_name(int mode)
{
	switch (mode) {
	case SIO_NORMAL_8: return "NORMAL8";
	case SIO_NORMAL_32: return "NORMAL32";
	case SIO_MULTI: return "MULTI";
	case SIO_UART: return "UART";
	case SIO_GPIO: return "GPIO";
	case SIO_JOYBUS: return "JOYBUS";
	}
	return "?";
}

static void stats(void)
{
	static unsigned last[NP];

	fprintf(stderr, "[link] attached=%d ", cable.d.attached);
	for (int i = 0; i < NP; i++) {
		struct GBA *gba = P[i].core->board;
		fprintf(stderr, "| P%d id=%d %3u fps sio=%s ", i + 1, P[i].node.id, P[i].frames - last[i],
		       mode_name(gba->sio.mode));
		last[i] = P[i].frames;
	}
	fprintf(stderr, "| multiRecv=%04x %04x\n", cable.multiRecv[0], cable.multiRecv[1]);
	fflush(stderr);
}

static void save_both(const char *name)
{
	char path[256];
	snprintf(path, sizeof path, "%s.ppm", name);
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", GW * NP + 8 * (NP - 1), GH);
	for (int y = 0; y < GH; y++)
		for (int i = 0; i < NP; i++) {
			for (int x = 0; x < GW; x++) {
				uint32_t c = to_xrgb(P[i].video[y * GW + x]);
				unsigned char px[3] = {c >> 16, c >> 8, c};
				fwrite(px, 1, 3, f);
			}
			if (i < NP - 1)
				for (int k = 0; k < 8; k++)
					fwrite("\x40\x40\x40", 1, 3, f);
		}
	fclose(f);
}

static void run_script(const char *script)
{
	char *s = strdup(script), *save = NULL;
	double t_stats = now_s() + 1;
	// run n frames (scripts count time in GBA frames, so they work unthrottled too)
#define RUN_FRAMES(n)                                       \
	for (int f_ = 0; f_ < (n); f_++) {                  \
		emulate_frame();                            \
		pace();                                     \
		if (now_s() > t_stats)                      \
			stats(), t_stats += 1;              \
	}
	for (char *tok = strtok_r(s, " ", &save); tok; tok = strtok_r(NULL, " ", &save)) {
		if (tok[0] == 'w') {
			RUN_FRAMES((int)(atof(tok + 1) * 60));
		} else if (tok[0] == 'p' && tok[1] && tok[2]) {
			struct player *p = &P[(tok[2] - '1') % NP];
			p->core->setKeys(p->core, key_bit(tok[1]));
			RUN_FRAMES(6);
			p->core->setKeys(p->core, 0);
			RUN_FRAMES(6);
		} else if (tok[0] == 's') {
			save_both(tok + 1);
		}
	}
	free(s);
}

#ifdef WITH_SDL
#include <SDL2/SDL.h>
static void run_window(void)
{
	SDL_Init(SDL_INIT_VIDEO);
	int ww = (GW * NP + 8) * 2, wh = GH * 2;
	SDL_Window *win = SDL_CreateWindow("cozisboy link test", SDL_WINDOWPOS_CENTERED,
					   SDL_WINDOWPOS_CENTERED, ww, wh, 0);
	SDL_Renderer *ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_PRESENTVSYNC);
	SDL_RenderSetLogicalSize(ren, GW * NP + 8, GH);
	SDL_Texture *tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_XRGB8888,
					     SDL_TEXTUREACCESS_STREAMING, GW * NP + 8, GH);
	static uint32_t buf[(GW * NP + 8) * GH];
	int active = 0;
	double t_stats = now_s() + 1;
	for (;;) {
		SDL_Event e;
		while (SDL_PollEvent(&e)) {
			if (e.type == SDL_QUIT ||
			    (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_ESCAPE))
				return;
			if (e.type == SDL_KEYDOWN && e.key.keysym.scancode == SDL_SCANCODE_TAB) {
				P[active].core->setKeys(P[active].core, 0);
				active = (active + 1) % NP;
			}
		}
		const Uint8 *k = SDL_GetKeyboardState(NULL);
		unsigned keys = (k[SDL_SCANCODE_Z] ? K_A : 0) | (k[SDL_SCANCODE_X] ? K_B : 0) |
				(k[SDL_SCANCODE_RETURN] ? K_START : 0) |
				(k[SDL_SCANCODE_BACKSPACE] ? K_SELECT : 0) |
				(k[SDL_SCANCODE_RIGHT] ? K_RIGHT : 0) | (k[SDL_SCANCODE_LEFT] ? K_LEFT : 0) |
				(k[SDL_SCANCODE_UP] ? K_UP : 0) | (k[SDL_SCANCODE_DOWN] ? K_DOWN : 0);
		P[active].core->setKeys(P[active].core, keys);
		emulate_frame();
		int bw = GW * NP + 8;
		for (int i = 0; i < bw * GH; i++)
			buf[i] = 0x202020;
		for (int i = 0; i < NP; i++)
			for (int y = 0; y < GH; y++)
				for (int x = 0; x < GW; x++) {
					uint32_t c = to_xrgb(P[i].video[y * GW + x]);
					if (i != active)
						c = (c >> 1) & 0x7f7f7f; // dim the player you're not driving
					buf[y * bw + i * (GW + 8) + x] = c;
				}
		SDL_UpdateTexture(tex, NULL, buf, bw * 4);
		SDL_RenderClear(ren);
		SDL_RenderCopy(ren, tex, NULL, NULL);
		SDL_RenderPresent(ren);
		if (now_s() > t_stats)
			stats(), t_stats += 1;
	}
}
#endif



// Only show mGBA's errors (its debug logging drowns the link stats).
static void quiet_log(struct mLogger *l, int cat, enum mLogLevel level, const char *fmt, va_list ap)
{
	(void)l, (void)cat;
	if (level & (mLOG_FATAL | mLOG_ERROR)) {
		vfprintf(stderr, fmt, ap);
		fputc('\n', stderr);
	}
}
static struct mLogger quiet = {quiet_log, NULL};

int main(int argc, char **argv)
{
	mLogSetDefaultLogger(&quiet);
	if (argc < 2) {
		fprintf(stderr, "usage: %s <rom> [rom2]\n", argv[0]);
		return 2;
	}
	GBASIOLockstepInit(&cable);
	cable.d.signal = ls_signal;
	cable.d.wait = ls_wait;
	cable.d.addCycles = ls_addCycles;
	cable.d.useCycles = ls_useCycles;
	cable.d.unusedCycles = ls_unusedCycles;
	cable.d.unload = ls_unload;
	for (int i = 0; i < NP; i++)
		if (start_player(&P[i], argc > 2 && i ? argv[2] : argv[1], i))
			return 1;
	for (int i = 0; i < NP; i++)
		attach_link(&P[i]);

	const char *script = getenv("LT_SCRIPT");
	if (script) {
		run_script(script);
	} else {
#ifdef WITH_SDL
		run_window();
#else
		run_script("w10");
#endif
	}
	for (int i = 0; i < NP; i++)
		P[i].core->deinit(P[i].core);
	return 0;
}
