// Games: games hub, running games, Control Center.
// MOCK DATA: the titles are made up and each "game" is a tiny built-in scene
// (walk, jump, collect coins) standing in for an emulator core. The screens,
// controls and state handling are the real design; later the scene is
// replaced by a libretro core (mGBA) that fills the same 240x160 frame.
//
// Menu button (like the PS button), handled in ui.c:
//   tap         Control Center: Home / running games / all games, plus quick
//               actions for the game on screen
//   hold        go Home (games stay suspended)
//   double-tap  jump back to the previous game
//
// Layout variations (picked in Settings while we decide):
//   hub:  0 SHELF (PS5), 1 COVERS (big carousel), 2 GRID (grid + details)
//   cc:   0 BOTTOM BAR (PS5), 1 SIDE PANEL (Steam Deck), 2 CARD FAN (full screen)
#include "ui_priv.h"

#include "emu.h"
#include "library.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/stat.h>

#define GW 240 // GBA screen
#define GH 160

/* ---- library (mock) ------------------------------------------------------ */

typedef struct {
	const char *title, *genre;
	uint32_t col;
	int hours, fav;
	int recent;       // higher = played more recently, 0 = never played
	const char *last; // "last played" text
	const char *img;  // optional real cover art in mock/<img>-{cover,art}.ppm
	char *rom;        // ROM path (NULL: built-in mock scene)
	char *sav_in;     // battery save to start from if sav_out doesn't exist (NULL: none)
	char *sav_out;    // battery save (the library's, written by the emulator)
	int has_art;      // art[] holds a picture
	char *thumb;      // ROM base name: thumbs/{cover,screen}/<thumb>.ppm
	long last_t;      // when last played (unix time), 0 = never (ROM games)
	const coz_game *cg; // the game in the library (NULL: mock game): points at cgv
	int gone;         // removed from the library while the launcher runs: hidden
	coz_game cgv;     // its own copy of the library's record (rescans don't move it)
} game_t;

// Fallback library when there is no emulator core or no ROMs. (The fields
// after cg are left zero.)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
static const game_t mock_lib[] = {
	{"STAR TRAIL", "RPG", 0x5ee05e, 41, 1, 7, "TODAY", "star-trail", NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"PIXEL KNIGHT", "PLATFORMER", 0xff4a4a, 12, 0, 5, "YESTERDAY", "pixel-knight", NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"DUNGEON DASH", "ACTION", 0xffd93d, 23, 0, 6, "TODAY", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"MOON MINER", "PUZZLE", 0x5ee05e, 5, 1, 3, "3 DAYS AGO", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"TINY TANKS", "STRATEGY", 0x3fc8ff, 17, 0, 4, "2 DAYS AGO", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"SKY PIRATES", "SHOOTER", 0x6a7dff, 0, 0, 0, "NEVER", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"BLOCK DROP", "PUZZLE", 0xc56bff, 64, 1, 2, "LAST WEEK", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"NEON NINJA", "PLATFORMER", 0xff6bc1, 0, 0, 0, "NEVER", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"SLIME FARM", "SIMULATION", 0x5ee05e, 30, 0, 1, "LAST MONTH", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
	{"BOLT BOTS", "ACTION", 0x3fc8ff, 0, 0, 0, "NEVER", NULL, NULL, NULL, NULL, 0, NULL, 0, NULL},
};
#pragma GCC diagnostic pop
#define MAXLIB 512
static game_t lib[MAXLIB];
static int nlib;
#define NGAMES nlib
static int recent_counter = 10;

/* ---- running games --------------------------------------------------------- */

typedef struct {
	float t, px, py, vy, scroll;
	int face, score;
	uint8_t taken[64]; // collected coins (bitmap)
} gstate_t;

typedef struct {
	int game;
	int copy;              // 0: the game itself, n: linked copy shown as "#n+1"
	int last_used;
	gstate_t st;           // mock scene state
	emu_game *eg;          // its emulator while loaded (NULL: unloaded or mock scene)
	uint8_t *state;        // snapshot while unloaded (paused, not linked)
	size_t state_n;
	int boot;              // frames left of "starting up": blank frames show as black
	float fade;            // 0..1: the picture fades in when a start-up ends
	uint32_t frame[GW * GH];
} inst_t;

#define MAXRUN 8 // open games (each: last frame + snapshot, under 1 MB)
static inst_t run[MAXRUN];
static int nrun, cur = -1, use_counter;
static int nopen;   // open games, each counted once even with copies (hub_order)

#define NSLOTS 8 // save states per game
#define TW (GW / 2)
#define TH (GH / 2)
typedef struct {
	int valid;
	gstate_t st;     // mock
	uint8_t *blob;   // emulator save state
	size_t n;
	long t;          // when it was saved (unix time)
	char sid[24];    // id in the library ("" while only in memory: mock games)
	uint32_t thumb[TW * TH];
} slot_t;
static slot_t slots[MAXLIB][NSLOTS];

// key art for the hub background (a frame of each game's scene, or real art)
static uint32_t art[MAXLIB][GW * GH];
// real cover thumbnails, when mock/<img>-cover.ppm exists
#define COVER_N 160
static uint32_t cover_img[MAXLIB][COVER_N * COVER_N];
static int has_cover[MAXLIB];

// Load a binary PPM (P6, 8-bit) of exactly w x h into XRGB pixels.
static int load_ppm(const char *path, int w, int h, uint32_t *out)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return 0;
	int fw, fh, maxv, ok = fscanf(f, "P6 %d %d %d", &fw, &fh, &maxv) == 3 &&
			       fw == w && fh == h && maxv == 255 && fgetc(f) != EOF;
	for (int i = 0; ok && i < w * h; i++) {
		int r = fgetc(f), g = fgetc(f), b = fgetc(f);
		if (b == EOF)
			ok = 0;
		out[i] = (uint32_t)(r << 16 | g << 8 | b);
	}
	fclose(f);
	return ok;
}
// Thumbnails from tools/fetch-thumbnails.sh, loaded the first time they're
// drawn: thumbs/cover/<name>.ppm (160x160) and thumbs/screen/<name>.ppm (240x160).
static signed char cover_tried[MAXLIB], screen_tried[MAXLIB];

static const char *thumbs_dir(void)
{
	return getenv("COZ_THUMBS") ? getenv("COZ_THUMBS") : "thumbs";
}

static void ensure_cover(int g)
{
	if (cover_tried[g] || (!lib[g].thumb && !lib[g].cg))
		return;
	cover_tried[g] = 1;
	// the library's box art (added from the web app), else thumbs/
	if (lib[g].cg && lib[g].cg->has_cover && coz_cover_load(lib[g].cg, COVER_N, COVER_N, cover_img[g]) == 0) {
		has_cover[g] = 1;
		return;
	}
	if (!lib[g].thumb)
		return;
	char path[600];
	snprintf(path, sizeof path, "%s/cover/%s.ppm", thumbs_dir(), lib[g].thumb);
	if (load_ppm(path, COVER_N, COVER_N, cover_img[g]))
		has_cover[g] = 1;
}

static void ensure_screen(int g)
{
	if (screen_tried[g] || !lib[g].thumb)
		return;
	screen_tried[g] = 1;
	char path[600];
	snprintf(path, sizeof path, "%s/screen/%s.ppm", thumbs_dir(), lib[g].thumb);
	if (load_ppm(path, GW, GH, art[g]))
		lib[g].has_art = 1;
}

// the last frame of each game when it was closed (preview for "last played")
static uint32_t lastshot[MAXLIB][GW * GH];
static int has_lastshot[MAXLIB];

/* ---- UI state ----------------------------------------------------------------- */


enum { HUB_TILES, HUB_BUTTONS, HUB_CARDS };
enum { BT_PLAY, BT_FAV, BT_CLOSE };

// Control Center quick actions (only when a game is on screen)
// In-game Control Center: session (cards + reset/close), state (save/load/slot),
// and two small popups for audio & video and controls. No speed controls for now.
enum { QA_SAVE, QA_LOAD, QA_SLOT, QA_AV, QA_INPUT, QA_RESET, QA_CLOSE, NQA };

static const char *const SCREEN_OPTS[] = {"2X SHARP", "FULL", "STRETCH"};
static const char *const FILTER_OPTS[] = {"NONE", "LCD GRID", "SCANLINES"};
enum { AV_VOLUME, AV_SCREEN, AV_FILTER, AV_COLOR };
static item_t av_items[] = {
	{"VOLUME", IT_SLIDER, 5, 10, NULL, NULL, NULL},
	{"SCREEN", IT_CHOICE, 1, 3, SCREEN_OPTS, NULL, NULL}, // FULL by default
	{"FILTER", IT_CHOICE, 0, 3, FILTER_OPTS, NULL, NULL},
	{"COLOR FIX", IT_TOGGLE, 0, 1, NULL, NULL, NULL},
};
enum { IN_SWAP, IN_TURBO_A, IN_TURBO_B };
static item_t input_items[] = {
	{"SWAP A AND B", IT_TOGGLE, 0, 1, NULL, NULL, NULL},
	{"TURBO A ON X", IT_TOGGLE, 0, 1, NULL, NULL, NULL},
	{"TURBO B ON Y", IT_TOGGLE, 0, 1, NULL, NULL, NULL},
};
static list_t av_list = {av_items, 4, 0, 0, 0};
static list_t input_list = {input_items, 3, 0, 0, 0};

int games_prefs(pref_t *p)
{
	int n = 0;
	p[n++] = (pref_t){"screen", &av_items[AV_SCREEN].val, 3};
	p[n++] = (pref_t){"filter", &av_items[AV_FILTER].val, 3};
	p[n++] = (pref_t){"color_fix", &av_items[AV_COLOR].val, 2};
	p[n++] = (pref_t){"swap_ab", &input_items[IN_SWAP].val, 2};
	p[n++] = (pref_t){"turbo_a", &input_items[IN_TURBO_A].val, 2};
	p[n++] = (pref_t){"turbo_b", &input_items[IN_TURBO_B].val, 2};
	return n;
}

static struct {
	int hub_style, cc_style;
	// hub
	int game;           // selected game id
	int hri;            // games view: selected open copy of it (-1: none / any)
	int row, btn, card;
	float cam, changed; // tile scroll, time since the selection changed
	float camx;         // covers layout: scroll in pixels (has a gap between groups)
	float bump;         // time since bumping into an end (wiggle, as on the home screen)
	int bump_dir;
	float bump_off, bump_offx; // wiggle currently added to cam / camx
	// settings applied to every game
	int ff, screen_mode, filter, slot;
	int cc_sub; // 0 none, QA_AV or QA_INPUT: popup open
	unsigned frame_no;
	// control center
	int cc, cc_row, cc_card, cc_act, cc_item;
	float cc_t, cc_cam;
	int hinted;
	// Buttons still held when the game takes over (the A that loaded a
	// snapshot or started the game, the B that closed a menu) don't reach it
	// until they're released.
	int from_menu;      // the game wasn't running last frame (menu, sheet, start)
	unsigned swallow;   // those held buttons
	// SAVES page (X on a game, or SAVES in the in-game menu)
	int pick, pick_sel, pick_n, pick_game;
	int pick_ingame;    // opened from the in-game menu (B goes back to it)
	int pick_focus;     // 0: the list of saves, 1: the selected save's actions
	int pick_act;       // selected action
	float pick_hold;    // hold A on a destructive action
	int pick_done;      // the hold fired: wait for A to be released
	int pick_scroll;    // first list row shown
	coz_save pick_saves[COZ_SAVE_SLOTS];
	int pick_new; // save number of the NEW SAVE row (0: no room)
} G;

static char toast_buf[48];

void games_set_style(int hub, int cc)
{
	G.hub_style = hub;
	G.cc_style = cc;
}

/* ---- the mock game ------------------------------------------------------- */

#define GROUND 128
#define COIN_X(k) (60 + (k) * 44)
#define COIN_Y(k) (GROUND - 28 - 18 * ((k) * 7 % 3))

static void game_reset(gstate_t *s)
{
	memset(s, 0, sizeof *s);
	s->px = 40;
	s->py = GROUND;
	s->face = 1;
}

static void game_step(gstate_t *s, unsigned held, unsigned pressed, float dt)
{
	s->t += dt;
	float vx = (held & BTN_RIGHT) ? 80.0f : (held & BTN_LEFT) ? -80.0f : 0;
	if (vx)
		s->face = vx > 0 ? 1 : -1;
	s->px += vx * dt;
	if (s->px < 8)
		s->px = 8;
	int on_ground = s->py >= GROUND;
	if ((pressed & BTN_A) && on_ground) {
		s->vy = -200;
		sfx_play(SFX_JUMP, 0);
	}
	s->vy += 560 * dt;
	s->py += s->vy * dt;
	if (s->py > GROUND)
		s->py = GROUND, s->vy = 0;
	s->scroll = s->px - 90 < 0 ? 0 : s->px - 90;

	int k0 = (int)((s->px - 60) / 44) - 1;
	for (int k = k0 < 0 ? 0 : k0; k <= k0 + 2 && k < 512; k++) {
		if (s->taken[k / 8] & (1 << (k % 8)))
			continue;
		float cx = COIN_X(k), cy = COIN_Y(k);
		if (fabsf(cx - s->px) < 10 && fabsf(cy - (s->py - 8)) < 12) {
			s->taken[k / 8] |= (uint8_t)(1 << (k % 8));
			s->score += 10;
			sfx_play(SFX_COIN, 0);
		}
	}
}

static void game_render(const gstate_t *s, int game, uint32_t *out, int hud)
{
	canvas cv = {out, GW, GH, GW}, *c = &cv;
	uint32_t col = lib[game].col;
	float sc = s->scroll;

	for (int y = 0; y < GH; y++)
		fill(c, 0, y, GW, 1, mix(mix(col, 0x10102a, 0.6f), mix(col, WHITE, 0.45f), y / (float)GROUND));
	fill(c, 180, 20, 18, 18, mix(col, WHITE, 0.8f));
	for (int k = 0; k < 4; k++) {
		int x = (int)(fmodf(k * 83 - sc * 0.2f - s->t * 6 + 2000, GW + 60)) - 40;
		int y = 18 + (k * 37) % 40;
		fill(c, x, y, 36, 8, WHITE), fill(c, x + 6, y - 6, 20, 6, WHITE);
	}
	for (int x = 0; x < GW; x++) {
		float wx = x + sc * 0.3f;
		int h = 34 + (int)(14 * sinf(wx * 0.035f + game) + 6 * sinf(wx * 0.11f));
		fill(c, x, GROUND - h, 1, h, mix(col, 0x101020, 0.5f));
		wx = x + sc * 0.6f;
		h = 16 + (int)(9 * sinf(wx * 0.06f + game * 2) + 4 * sinf(wx * 0.2f));
		fill(c, x, GROUND - h, 1, h, mix(col, 0x101020, 0.3f));
	}
	fill(c, 0, GROUND, GW, 4, mix(0x5ee05e, col, 0.25f));
	for (int ty = GROUND + 4; ty < GH; ty += 8)
		for (int tx = -((int)sc % 16); tx < GW; tx += 8) {
			int k = (int)((tx + (int)sc) / 8 + ty / 8);
			fill(c, tx, ty, 8, 8, (k & 1) ? 0x6b4226 : 0x7d5232);
		}
	for (int k = (int)(sc / 44) - 2; k < (int)(sc / 44) + 8; k++) {
		if (k < 0 || k >= 512 || (s->taken[k / 8] & (1 << (k % 8))))
			continue;
		int x = COIN_X(k) - (int)sc, y = COIN_Y(k);
		int w = 2 + (int)(6 * fabsf(sinf(s->t * 5 + k)));
		fill(c, x - w / 2 - 1, y - 5, w + 2, 10, 0x6b4a00);
		fill(c, x - w / 2, y - 4, w, 8, 0xffd93d);
	}
	int px = (int)(s->px - sc), py = (int)s->py;
	int squash = s->py < GROUND ? 2 : 0;
	fill(c, px - 7, py - 15 - squash, 14, 15 + squash, BLACK);
	fill(c, px - 6, py - 14 - squash, 12, 13 + squash, WHITE);
	int ex = px + s->face * 2;
	fill(c, ex - 3, py - 11 - squash, 2, 3, BLACK), fill(c, ex + 1, py - 11 - squash, 2, 3, BLACK);
	fill(c, px - 6, py - 1, 4, 2, BLACK), fill(c, px + 2, py - 1, 4, 2, BLACK);
	if (!hud)
		return;
	char buf[24];
	snprintf(buf, sizeof buf, "SCORE %05d", s->score);
	text(c, 7, 7, buf, 1, BLACK), text(c, 6, 6, buf, 1, WHITE);
	text(c, GW - 6 - text_width(lib[game].title, 1) + 1, 7, lib[game].title, 1, BLACK);
	text(c, GW - 6 - text_width(lib[game].title, 1), 6, lib[game].title, 1, WHITE);
}

/* ---- last played: saves/last-played.txt, one line per game that was played ---- *
 * <unix time><TAB><ROM base name>. Only the most recent time is kept per game. */

#include <time.h>

static const char *history_path(void)
{
	static char path[256];
	snprintf(path, sizeof path, "%s/last-played.txt",
		 getenv("COZ_SAVES") ? getenv("COZ_SAVES") : "saves");
	return path;
}

static void history_save(void);

static int by_last_t(const void *a, const void *b)
{
	long x = lib[*(const int *)a].last_t, y = lib[*(const int *)b].last_t;
	return (x > y) - (x < y);
}

static void history_load(void)
{
	FILE *f = fopen(history_path(), "r");
	if (!f)
		return;
	char line[700], name[600];
	long t;
	int stale = 0; // entries for games that are no longer in the library
	while (fgets(line, sizeof line, f)) {
		if (sscanf(line, "%ld\t%599[^\n]", &t, name) != 2) {
			stale++;
			continue;
		}
		int found = 0;
		for (int g = 0; g < nlib; g++)
			if (lib[g].thumb && !strcmp(lib[g].thumb, name))
				lib[g].last_t = t, found = 1;
		stale += !found;
	}
	fclose(f);
	// play order follows the times: oldest gets the lowest rank
	int order[MAXLIB], n = 0;
	for (int g = 0; g < nlib; g++)
		if (lib[g].last_t)
			order[n++] = g;
	qsort(order, (size_t)n, sizeof *order, by_last_t);
	for (int k = 0; k < n; k++)
		lib[order[k]].recent = ++recent_counter;
	if (stale)
		history_save(); // rewrite without the removed games
}

static void history_save(void)
{
	if (!nlib || !lib[0].rom) // mock library: nothing to remember
		return;
	char tmp[300];
	snprintf(tmp, sizeof tmp, "%s.tmp", history_path());
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	for (int g = 0; g < nlib; g++)
		if (lib[g].last_t && lib[g].thumb)
			fprintf(f, "%ld\t%s\n", lib[g].last_t, lib[g].thumb);
	fclose(f);
	rename(tmp, history_path());
}

// "TODAY", "YESTERDAY", "3 DAYS AGO"...
static const char *last_text(int g)
{
	static char buf[24];
	if (!lib[g].last_t)
		return lib[g].last ? lib[g].last : "NEVER";
	time_t now = time(NULL), then = lib[g].last_t;
	struct tm a, b;
	localtime_r(&now, &a);
	localtime_r(&then, &b);
	long days = (long)(difftime(mktime(&(struct tm){.tm_year = a.tm_year, .tm_mon = a.tm_mon,
							.tm_mday = a.tm_mday, .tm_isdst = -1}),
				    mktime(&(struct tm){.tm_year = b.tm_year, .tm_mon = b.tm_mon,
							.tm_mday = b.tm_mday, .tm_isdst = -1})) /
			  86400 + 0.5);
	if (days <= 0)
		return "TODAY";
	if (days == 1)
		return "YESTERDAY";
	if (days < 30) {
		snprintf(buf, sizeof buf, "%ld DAYS AGO", days);
		return buf;
	}
	return "A WHILE AGO";
}

/* ---- running game management --------------------------------------------- */

static int find_running(int game)
{
	for (int i = 0; i < nrun; i++)
		if (run[i].game == game)
			return i;
	return -1;
}

static int find_running_copy(int game, int copy)
{
	for (int i = 0; i < nrun; i++)
		if (run[i].game == game && run[i].copy == copy)
			return i;
	return -1;
}

static int is_emu(int g)
{
	return lib[g].rom != NULL;
}

// Title of an open game; copies get "#2", "#3"...
static const char *inst_title(int i)
{
	static char buf[4][80];
	static int k;
	if (!run[i].copy)
		return lib[run[i].game].title;
	char *b = buf[k++ & 3];
	snprintf(b, 80, "%s #%d", lib[run[i].game].title, run[i].copy + 1);
	return b;
}

// Battery save of copy n of game g: for library games, the library's save
// n+1 (copy 0 is the main save); for the built-in demo games, the launcher's
// own folder <saves>/copies/<name>-copyN.sav.
static void old_copy_sav_path(int g, int n, char *out, size_t sz);
static void copy_sav_path(int g, int n, char *out, size_t sz)
{
	if (lib[g].cg && coz_save_slot_path(lib[g].cg, n + 1, out, sz) == 0)
		return;
	old_copy_sav_path(g, n, out, sz);
}

static void old_copy_sav_path(int g, int n, char *out, size_t sz)
{
	const char *dir = getenv("COZ_SAVES") ? getenv("COZ_SAVES") : "saves";
	char d[512];
	snprintf(d, sizeof d, "%s/copies", dir);
	mkdir(d, 0755);
	snprintf(out, sz, "%s/%s-copy%d.sav", d, lib[g].cg ? lib[g].cg->id : lib[g].thumb, n + 1);
}

static int linked_with_cur(int i)
{
	return cur >= 0 && i != cur && emu_same_group(run[i].eg, run[cur].eg);
}

// Battery save of open game i (copies have their own).
static void inst_sav_path(int i, char *out, size_t sz)
{
	if (run[i].copy)
		copy_sav_path(run[i].game, run[i].copy, out, sz);
	else
		snprintf(out, sz, "%s", lib[run[i].game].sav_out);
}

/* Paused games are snapshotted and unloaded; linked games stay loaded (they
 * need each other) and run together while one of them is on screen. */
static void park(int i)
{
	inst_t *in = &run[i];
	if (!in->eg || emu_linked(in->eg) > 1)
		return;
	size_t n = emu_state_size(in->eg);
	free(in->state);
	in->state = malloc(n);
	in->state_n = in->state && !emu_save_state(in->eg, in->state, n) ? n : 0;
	emu_close(in->eg); // writes the battery save
	in->eg = NULL;
}

// Load open game i again (restoring its snapshot). Returns 0 on success.
static int wake(int i)
{
	inst_t *in = &run[i];
	if (in->eg || !is_emu(in->game))
		return 0;
	char sav[600];
	inst_sav_path(i, sav, sizeof sav);
	in->eg = emu_open(lib[in->game].rom, in->copy ? NULL : lib[in->game].sav_in, sav);
	if (!in->eg)
		return -1;
	if (in->state_n)
		emu_load_state(in->eg, in->state, in->state_n);
	free(in->state);
	in->state = NULL, in->state_n = 0;
	return 0;
}

static void close_running(int i)
{
	emu_close(run[i].eg); // writes the battery save
	run[i].eg = NULL;
	free(run[i].state);
	run[i].state = NULL, run[i].state_n = 0;
	memcpy(lastshot[run[i].game], run[i].frame, sizeof run[i].frame);
	has_lastshot[run[i].game] = 1;
	lib[run[i].game].last = "JUST NOW";
	lib[run[i].game].last_t = (long)time(NULL);
	history_save();
	if (cur == i)
		cur = -1;
	else if (cur > i)
		cur--;
	memmove(&run[i], &run[i + 1], (size_t)(nrun - i - 1) * sizeof *run);
	nrun--;
}

// Running games, most recently used first.
static int running_order(int *v)
{
	for (int i = 0; i < nrun; i++)
		v[i] = i;
	for (int i = 1; i < nrun; i++)
		for (int j = i; j > 0 && run[v[j]].last_used > run[v[j - 1]].last_used; j--) {
			int t = v[j];
			v[j] = v[j - 1], v[j - 1] = t;
		}
	return nrun;
}

static void enter_game(int i, int x, int y)
{
	G.from_menu = 1;
	if (wake(i)) {
		snprintf(toast_buf, sizeof toast_buf, "COULD NOT START %s", inst_title(i));
		sfx_play(SFX_BUMP, 0);
		toast(toast_buf);
		return;
	}
	for (int k = 0; k < nrun; k++)
		if (k != i && !emu_same_group(run[k].eg, run[i].eg))
			park(k);
	cur = i;
	run[i].last_used = ++use_counter;
	lib[run[i].game].recent = ++recent_counter;
	lib[run[i].game].last = "NOW";
	lib[run[i].game].last_t = (long)time(NULL);
	history_save();
	sfx_play(SFX_OPEN, run[i].game % 8);
	go(SCR_GAME, lib[run[i].game].col, x, y);
	if (!G.hinted) {
		G.hinted = 1;
		toast("TAP MENU: CONTROL CENTER");
	}
}

/* ---- save states on disk (the game library) --------------------------------- */

// Read a PNG written by coz_png_write (8-bit RGB, stored deflate blocks, no
// filters) into a TW x TH thumbnail. Anything else is refused (returns -1).
static int read_state_png(const char *path, uint32_t *thumb)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return -1;
	unsigned char *z = NULL, hdr[8];
	size_t zn = 0;
	int w = 0, h = 0, ok = fread(hdr, 1, 8, f) == 8 && hdr[0] == 0x89 && hdr[1] == 'P';
	while (ok) {
		unsigned char ch[8];
		if (fread(ch, 1, 8, f) != 8)
			break;
		uint32_t len = (uint32_t)ch[0] << 24 | ch[1] << 16 | ch[2] << 8 | ch[3];
		if (!memcmp(ch + 4, "IHDR", 4)) {
			unsigned char ih[13];
			ok = len == 13 && fread(ih, 1, 13, f) == 13 && ih[8] == 8 && ih[9] == 2 && !ih[12];
			w = ih[0] << 24 | ih[1] << 16 | ih[2] << 8 | ih[3];
			h = ih[4] << 24 | ih[5] << 16 | ih[6] << 8 | ih[7];
			fseek(f, 4, SEEK_CUR);
		} else if (!memcmp(ch + 4, "IDAT", 4)) {
			unsigned char *nz = realloc(z, zn + len);
			ok = nz && fread(nz + zn, 1, len, f) == len;
			z = nz, zn += len;
			fseek(f, 4, SEEK_CUR);
		} else if (!memcmp(ch + 4, "IEND", 4)) {
			break;
		} else {
			fseek(f, (long)len + 4, SEEK_CUR);
		}
	}
	fclose(f);
	size_t rl = (size_t)h * (1 + 3 * (size_t)w);
	unsigned char *raw = ok && w > 0 && h > 0 && w <= 1024 && h <= 1024 ? malloc(rl) : NULL;
	size_t rp = 0, zp = 2;
	while (raw && zp + 5 <= zn && rp < rl) { // stored blocks only
		unsigned char bh = z[zp];
		size_t n = (size_t)(z[zp + 1] | z[zp + 2] << 8);
		if ((bh & 6) || zp + 5 + n > zn || rp + n > rl)
			break;
		memcpy(raw + rp, z + zp + 5, n);
		rp += n, zp += 5 + n;
		if (bh & 1)
			break;
	}
	int r = -1;
	if (raw && rp == rl) {
		for (int y = 0; y < h; y++)
			if (raw[(size_t)y * (1 + 3 * (size_t)w)] != 0)
				goto out; // filtered rows: not ours
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++) {
				const unsigned char *px = raw + (size_t)(y * h / TH) * (1 + 3 * (size_t)w) + 1 +
							  3 * (size_t)(x * w / TW);
				thumb[y * TW + x] = (uint32_t)px[0] << 16 | px[1] << 8 | px[2];
			}
		r = 0;
	}
out:
	free(raw);
	free(z);
	return r;
}

// Fill game g's save states from the library (newest NSLOTS).
static void states_load_disk(int g)
{
	if (!lib[g].cg)
		return;
	coz_state *st;
	int n;
	if (coz_states_list(lib[g].cg, &st, &n))
		return;
	for (int k = 0; k < n && k < NSLOTS; k++) {
		slot_t *sl = &slots[g][k];
		memset(sl, 0, sizeof *sl);
		sl->valid = 1;
		sl->t = (long)st[k].time;
		snprintf(sl->sid, sizeof sl->sid, "%s", st[k].id);
		char p[1024];
		if (!st[k].has_thumb || coz_state_thumb_path(lib[g].cg, st[k].id, p, sizeof p) ||
		    read_state_png(p, sl->thumb))
			for (int i = 0; i < TW * TH; i++)
				sl->thumb[i] = 0x202020;
	}
	free(st);
}

// The state's data, read from the library the first time it's needed.
static int slot_data(int g, slot_t *sl)
{
	if (sl->blob)
		return 0;
	char p[1024];
	if (!lib[g].cg || !sl->sid[0] || coz_state_path(lib[g].cg, sl->sid, p, sizeof p))
		return -1;
	FILE *f = fopen(p, "rb");
	if (!f)
		return -1;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	sl->blob = n > 0 ? malloc((size_t)n) : NULL;
	sl->n = sl->blob && fread(sl->blob, 1, (size_t)n, f) == (size_t)n ? (size_t)n : 0;
	fclose(f);
	if (!sl->n) {
		free(sl->blob);
		sl->blob = NULL;
		return -1;
	}
	return 0;
}

// Forget a state, and delete it from the library.
static void slot_delete(int g, slot_t *sl)
{
	if (lib[g].cg && sl->sid[0])
		coz_state_delete(lib[g].cg, sl->sid);
	free(sl->blob);
	memset(sl, 0, sizeof *sl);
}

// Open game with its save number copy+1 (0: the main save), unlinked.
static void open_game_copy(int game, int copy, int slot, int x, int y)
{
	int i = find_running_copy(game, copy);
	if (i < 0) {
		if (nrun == MAXRUN) { // free the least recently used one
			int lru = 0;
			for (int k = 1; k < nrun; k++)
				if (run[k].last_used < run[lru].last_used)
					lru = k;
			snprintf(toast_buf, sizeof toast_buf, "CLOSED %s TO MAKE ROOM",
				 lib[run[lru].game].title);
			toast(toast_buf);
			close_running(lru);
		}
		i = nrun++;
		memset(&run[i], 0, sizeof run[i]);
		run[i].game = game, run[i].copy = copy;
		run[i].boot = lib[game].rom ? 120 : 0, run[i].fade = lib[game].rom ? 0 : 1;
		game_reset(&run[i].st);
	}
	slot_t *sl = slot >= 0 ? &slots[game][slot] : NULL;
	if (is_emu(game)) {
		if (wake(i)) {
			snprintf(toast_buf, sizeof toast_buf, "COULD NOT START %s", lib[game].title);
			sfx_play(SFX_BUMP, 0);
			toast(toast_buf);
			nrun--;
			return;
		}
		if (sl && sl->valid && slot_data(game, sl) == 0)
			emu_load_state(run[i].eg, sl->blob, sl->n);
	} else {
		if (sl && sl->valid)
			run[i].st = sl->st;
		game_render(&run[i].st, game, run[i].frame, 1);
	}
	enter_game(i, x, y);
}

static void open_game(int game, int slot, int x, int y)
{
	open_game_copy(game, 0, slot, x, y);
}

/* ---- shared drawing -------------------------------------------------------- */

// Blit a sw x sh frame scaled into (dx, dy, dw, dh) with an optional filter.
static void blit_game(canvas *c, const uint32_t *src, int sw, int sh, int dx, int dy,
		      int dw, int dh, int filter)
{
	for (int y = 0; y < dh; y++) {
		int sy = y * sh / dh, row_edge = (y + 1) * sh / dh != sy;
		int py = dy + y;
		if (py < 0 || py >= c->h)
			continue;
		uint32_t *out = c->px + (size_t)py * c->stride;
		const uint32_t *in = src + (size_t)sy * sw;
		for (int x = 0; x < dw; x++) {
			if (dx + x < 0 || dx + x >= c->w)
				continue;
			int sx = x * sw / dw;
			uint32_t p = in[sx];
			int col_edge = (x + 1) * sw / dw != sx;
			if ((filter == 1 && (row_edge || col_edge)) || (filter == 2 && row_edge))
				p = mix(p, BLACK, filter == 1 ? 0.28f : 0.4f);
			out[dx + x] = p;
		}
	}
}

// Set while the covers hub draws its own frames around covers.
static int skip_frame;

static void draw_cover(canvas *c, int g, int x, int y, int size)
{
	ensure_cover(g);
	uint32_t col = lib[g].col;
	if (!skip_frame)
		wframe(c, x, y, size, size, WHITE);
	if (has_cover[g]) { // real box art, nearest-neighbour scaled
		for (int yy = 0; yy < size; yy++) {
			int py = y + yy;
			if (py < 0 || py >= c->h)
				continue;
			const uint32_t *in = cover_img[g] + (size_t)(yy * COVER_N / size) * COVER_N;
			uint32_t *out = c->px + (size_t)py * c->stride;
			for (int xx = 0; xx < size; xx++)
				if (x + xx >= 0 && x + xx < c->w)
					out[x + xx] = in[xx * COVER_N / size];
		}
		return;
	}
	fill(c, x, y, size, size, mix(col, BLACK, 0.15f));
	int sw = size / 8 > 2 ? size / 8 : 2;
	for (int yy = 0; yy < size; yy += 2)
		for (int xx = 0; xx < size; xx += 2)
			if (((xx + yy) / sw) % 2 == 0)
				fill(c, x + xx, y + yy, 2, 2, mix(col, WHITE, 0.18f));
	char ini[2] = {lib[g].title[0], 0};
	int s = size / 14 > 1 ? size / 14 : 1;
	text_arcade(c, x + (size - text_width(ini, s)) / 2, y + (size - 7 * s) / 2 - size / 14,
		    ini, s, col);
	if (size >= 120) { // cartridge label strip
		fill(c, x, y + size - 22, size, 22, BLACK);
		text(c, x + 8, y + size - 17, "GBA", 2, WHITE);
		for (int k = 0; k < 8; k++)
			fill(c, x + size - 70 + k * 8, y + size - 15, 6, 8, PAL[k]);
	}
}

static void running_badge(canvas *c, int x, int y)
{
	rrect(c, x, y, 20, 20, 3, BLACK);
	rrect(c, x + 2, y + 2, 16, 16, 2, PAL[3]);
	fill(c, x + 7, y + 5, 2, 10, BLACK), fill(c, x + 9, y + 7, 2, 6, BLACK),
		fill(c, x + 11, y + 9, 2, 2, BLACK);
}

static void pill(canvas *c, int x, int y, int w, const char *label, int focused, int primary)
{
	uint32_t f = primary ? WHITE : 0x2a2a2a;
	wbox(c, x, y, w, 36, f, focused ? W_SEL : W_NORMAL);
	text(c, x + (w - text_width(label, 2)) / 2, y + 11, label, 2, wink(f, focused ? W_SEL : W_NORMAL));
}

// Full-screen key art of game g, faded towards the background for legibility.
// Best picture of a game: live frame, last frame when closed, or its art.
static const uint32_t *game_picture(int g)
{
	int ri = find_running(g);
	if (ri >= 0)
		return run[ri].frame;
	if (has_lastshot[g])
		return lastshot[g];
	ensure_screen(g);
	return lib[g].has_art ? art[g] : NULL;
}

static void draw_key_art(canvas *c, int g, float strength)
{
	const uint32_t *bg = game_picture(g);
	if (bg)
		blit_game(c, bg, GW, GH, 0, 0, W, H, 0);
	else
		for (int y = 0; y < H; y++)
			fill(c, 0, y, W, 1, mix(mix(lib[g].col, BLACK, 0.5f), BG0, (float)y / H));
	float fade = clampf(G.changed / 0.25f, 0, 1);
	for (int y = 0; y < H; y++) {
		uint32_t *row = c->px + (size_t)y * c->stride;
		float vy = (float)y / H;
		for (int x = 0; x < W; x++) {
			float a = strength + 0.55f * vy + 0.25f * (1 - (float)x / W);
			a = 1 - (1 - clampf(a, 0, 0.92f)) * fade;
			row[x] = mix(row[x], BG0, a);
		}
	}
}

static void info_line(int g, char *buf, size_t n)
{
	if (find_running(g) >= 0)
		snprintf(buf, n, "RUNNING - PAUSED");
	else if (lib[g].recent)
		snprintf(buf, n, "%s - LAST PLAYED %s", lib[g].genre, last_text(g));
	else
		snprintf(buf, n, "%s - NOT PLAYED YET", lib[g].genre);
}

/* ---- hub ----------------------------------------------------------------- */

/* Games view items: an open game is one item per open copy (HUB_RUN + run
 * index), any other game one item (its game id). */
#define HUB_RUN 100000

static int item_game(int it)
{
	return it >= HUB_RUN ? run[it - HUB_RUN].game : it;
}

static int item_run(int it)
{
	return it >= HUB_RUN ? it - HUB_RUN : -1;
}

// The open copy selected in the games view (left group), or -1.
static int hub_sel_run(void)
{
	if (G.hri >= 0 && G.hri < nrun && run[G.hri].game == G.game)
		return G.hri;
	return -1;
}

static int hub_sel_item(void)
{
	int ri = hub_sel_run();
	return ri >= 0 ? HUB_RUN + ri : G.game;
}

static void hub_select(int it)
{
	G.game = item_game(it);
	G.hri = item_run(it);
}

// Order: open copies (most recently used first), then all games: most
// recently played first, never-played ones A-Z at the end.
static int hub_order(int *v)
{
	int n = 0;
	for (int i = 0; i < nrun; i++)
		v[n++] = HUB_RUN + i;
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && run[v[j] - HUB_RUN].last_used > run[v[j - 1] - HUB_RUN].last_used; j--) {
			int t = v[j];
			v[j] = v[j - 1], v[j - 1] = t;
		}
	nopen = n;
	int first = n;
	for (int g = 0; g < NGAMES; g++) // all games, open ones too (A: another copy)
		if (!lib[g].gone)
			v[n++] = g;
	for (int i = first + 1; i < n; i++)
		for (int j = i; j > first; j--) {
			int a = v[j - 1], b = v[j];
			int swap = lib[b].recent > lib[a].recent ||
				   (lib[b].recent == lib[a].recent && strcmp(lib[b].title, lib[a].title) < 0);
			if (!swap)
				break;
			v[j - 1] = b, v[j] = a;
		}
	return n;
}

static int index_of(const int *v, int n, int g)
{
	for (int i = 0; i < n; i++)
		if (v[i] == g)
			return i;
	return 0;
}

#define GRID_COLS 5

// Covers layout: open games, a gap with a dashed line, then all other games.
#define COVER_SP 182
#define COVER_GAP 60

static float cover_pos(int i)
{
	float x = (float)(i * COVER_SP);
	if (nopen && i >= nopen)
		x += COVER_GAP;
	return x;
}

// Open the hub on its first item: the most recently played game.
void games_hub_focus_first(void)
{
	int v[MAXLIB + MAXRUN], n = hub_order(v);
	if (!n)
		return;
	hub_select(v[0]);
	G.cam = 0, G.camx = cover_pos(0);
	G.row = 0, G.btn = 0, G.changed = 0;
}

static void open_copy_of(int orig, int from_mine);

static void hub_activate(int act, int g, int ri)
{
	switch (act) {
	case BT_PLAY:
		if (ri >= 0)
			enter_game(ri, 100, 90); // that copy
		else if (find_running_copy(g, 0) >= 0)
			enter_game(find_running_copy(g, 0), 100, 90); // the main save, already open
		else
			open_game(g, -1, 100, 90);
		break;
	case BT_FAV:
		lib[g].fav = !lib[g].fav;
		sfx_play(lib[g].fav ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF, 0);
		break;
	case BT_CLOSE:
		snprintf(toast_buf, sizeof toast_buf, "%s CLOSED", inst_title(ri));
		close_running(ri);
		G.hri = -1;
		sfx_play(SFX_TOGGLE_OFF, 0);
		toast(toast_buf);
		G.btn = BT_PLAY;
		break;
	}
}

/* ---- PLAY WITH: choose which save a game starts with -------------------- */

// "TODAY 16:36", "YESTERDAY", "3 DAYS AGO", "12 NOV"
static const char *save_when(time_t t)
{
	static char buf[24];
	time_t now = time(NULL);
	struct tm a, b;
	localtime_r(&now, &a);
	localtime_r(&t, &b);
	long days = (long)(difftime(mktime(&(struct tm){.tm_year = a.tm_year, .tm_mon = a.tm_mon,
							.tm_mday = a.tm_mday, .tm_isdst = -1}),
				    mktime(&(struct tm){.tm_year = b.tm_year, .tm_mon = b.tm_mon,
							.tm_mday = b.tm_mday, .tm_isdst = -1})) /
			  86400 + 0.5);
	static const char *const mon[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
					  "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
	if (days <= 0)
		snprintf(buf, sizeof buf, "TODAY %02d:%02d", b.tm_hour, b.tm_min);
	else if (days == 1)
		snprintf(buf, sizeof buf, "YESTERDAY");
	else if (days < 7)
		snprintf(buf, sizeof buf, "%ld DAYS AGO", days);
	else
		snprintf(buf, sizeof buf, "%d %s", b.tm_mday, mon[b.tm_mon]);
	return buf;
}

// (Re)read game g's saves: the main one always, others while they have a save
// (deleted ones keep a backup the web app can restore).
static void pick_load(int g)
{
	coz_game_refresh((coz_game *)lib[g].cg);
	coz_save all[COZ_SAVE_SLOTS];
	int n = coz_saves_list(lib[g].cg, all), k = 0;
	for (int i = 0; i < n; i++)
		if (all[i].slot == 1 || all[i].has_save)
			G.pick_saves[k++] = all[i];
	G.pick_n = k;
	int f = coz_save_slot_free(lib[g].cg);
	G.pick_new = f > 0 ? f : 0;
	int rows = G.pick_n + (G.pick_new ? 1 : 0);
	if (G.pick_sel >= rows)
		G.pick_sel = rows - 1;
	G.pick_game = g;
}

#ifndef HOLD_TIME
#define HOLD_TIME 0.9f
#endif
#define RT_Y_PICK 12	// text offset inside a row (as ui_row)
static void scroll_arrow(canvas *c, int x, int y, int dir, uint32_t col);

static void pick_open(int g)
{
	G.pick_sel = 0, G.pick_hold = 0, G.pick_done = 1, G.pick_ingame = 0;
	G.pick_focus = 0, G.pick_act = 0, G.pick_scroll = 0;
	pick_load(g);
	G.pick = 1;
	ui_page_anim(0);
	sfx_play(SFX_OPEN, APP_GAMES);
}

static void ingame_header(canvas *c, const char *title, const char *status, uint32_t scol);

// What can be done with the selected row. Rows that would change a save
// that's open (being played, or paused) aren't offered: close it first.
enum { PA_PLAY, PA_COPY, PA_MAIN, PA_PREV, PA_DELETE };
static int pick_actions(int *a)
{
	int g = G.pick_game, n = 0;
	if (G.pick_sel >= G.pick_n) { // NEW SAVE
		a[n++] = PA_PLAY;
		return n;
	}
	const coz_save *sv = &G.pick_saves[G.pick_sel];
	int open = find_running_copy(g, sv->slot - 1) >= 0;
	a[n++] = PA_PLAY;
	if (sv->has_save && G.pick_new)
		a[n++] = PA_COPY;
	if (sv->slot > 1 && sv->has_save && !open && find_running_copy(g, 0) < 0)
		a[n++] = PA_MAIN;
	if (sv->has_backup && !open)
		a[n++] = PA_PREV;
	if (sv->has_save && !open)
		a[n++] = PA_DELETE;
	return n;
}

static int pick_needs_hold(int act)
{
	return act == PA_PREV || act == PA_DELETE;
}

static const char *pick_act_label(int act)
{
	int g = G.pick_game;
	if (act == PA_PLAY) {
		if (G.pick_sel >= G.pick_n)
			return "START IT";
		int ri = find_running_copy(g, G.pick_saves[G.pick_sel].slot - 1);
		return G.pick_ingame && ri >= 0 && ri == cur ? "RESUME" : ri >= 0 ? "SWITCH TO IT" : "PLAY";
	}
	return act == PA_COPY ? "COPY TO A NEW SAVE" : act == PA_MAIN ? "MAKE IT THE MAIN" : act == PA_PREV ? "RESTORE OLDER"
														: "DELETE";
}

static void pick_do(int act)
{
	int g = G.pick_game;
	const coz_save *sv = G.pick_sel < G.pick_n ? &G.pick_saves[G.pick_sel] : NULL;
	int save = sv ? sv->slot : G.pick_new, r = 0;
	switch (act) {
	case PA_PLAY:
		G.pick = 0;
		if (G.pick_ingame && cur >= 0 && run[cur].game == g && run[cur].copy == save - 1) {
			G.cc = 0;	// the save being played: just resume
			sfx_play(SFX_TOGGLE_OFF, 0);
			return;
		}
		G.cc = 0;
		if (find_running_copy(g, save - 1) >= 0)
			enter_game(find_running_copy(g, save - 1), W / 2, H / 2);
		else
			open_game_copy(g, save - 1, -1, W / 2, H / 2);
		return;
	case PA_COPY:
		r = coz_save_slot_copy(lib[g].cg, save, G.pick_new);
		snprintf(toast_buf, sizeof toast_buf, "COPIED INTO SAVE %d", G.pick_new);
		break;
	case PA_MAIN:
		r = coz_save_slot_swap(lib[g].cg, 1, save);
		snprintf(toast_buf, sizeof toast_buf, "IT'S THE MAIN SAVE NOW (THE OLD ONE IS SAVE %d)", save);
		break;
	case PA_PREV:
		r = coz_save_slot_restore_backup(lib[g].cg, save);
		snprintf(toast_buf, sizeof toast_buf, "OLDER VERSION RESTORED");
		break;
	case PA_DELETE:
		r = coz_save_slot_delete(lib[g].cg, save);
		snprintf(toast_buf, sizeof toast_buf, save == 1 ? "MAIN SAVE DELETED" : "SAVE %d DELETED", save);
		break;
	}
	if (r)
		snprintf(toast_buf, sizeof toast_buf, "THAT DIDN'T WORK");
	toast(toast_buf);
	sfx_play(r ? SFX_BUMP : act == PA_DELETE ? SFX_TOGGLE_OFF : SFX_TOGGLE_ON, 0);
	pick_load(g);
	if (act == PA_MAIN && !r)
		G.pick_sel = 0;	// follow it to the top
	G.pick_focus = 0, G.pick_act = 0;
}

static void pick_update(unsigned p, int g, float dt)
{
	(void)g;
	int rows = G.pick_n + (G.pick_new ? 1 : 0);
	int acts[8], na = pick_actions(acts);
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (G.pick_focus == 0) { // the list
		if (d) {
			if (G.pick_sel + d >= 0 && G.pick_sel + d < rows)
				G.pick_sel += d, G.pick_act = 0, sfx_play(SFX_TICK, 0);
			else
				sfx_play(SFX_BUMP, 0);
		}
		if (p & BTN_A) {
			// pick_done: the A that got us here must be released before
			// it can start a hold (no accidental delete)
			G.pick_focus = 1, G.pick_act = 0, G.pick_hold = 0, G.pick_done = 1;
			sfx_play(SFX_TICK, 0);
		} else if (p & BTN_B) {
			G.pick = 0;	// from the in-game menu: back to it
			sfx_play(SFX_BACK, 0);
		}
		return;
	}
	// the actions of the selected save
	if (G.pick_act >= na)
		G.pick_act = na - 1;
	if (d) {
		G.pick_hold = 0;
		if (ui_held() & BTN_A)
			G.pick_done = 1;
		if (G.pick_act + d >= 0 && G.pick_act + d < na)
			G.pick_act += d, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
		return;
	}
	if (p & BTN_B) {	// close the sheet
		G.pick_focus = 0, G.pick_hold = 0;
		sfx_play(SFX_BACK, 0);
		return;
	}
	int act = acts[G.pick_act];
	if (pick_needs_hold(act)) {
		if ((ui_held() & BTN_A) && !G.pick_done) {
			if ((G.pick_hold += dt) >= HOLD_TIME) {
				G.pick_hold = 0, G.pick_done = 1;
				pick_do(act);
			}
		} else if (!(ui_held() & BTN_A)) {
			G.pick_hold = 0, G.pick_done = 0;
		}
		return;
	}
	if (p & BTN_A)
		pick_do(act);
}

// Label of list row i ("MAIN SAVE", "SAVE 3", "+ NEW SAVE").
static void pick_label(int i, char *out, size_t n)
{
	if (i >= G.pick_n)
		snprintf(out, n, "+ NEW SAVE");
	else if (G.pick_saves[i].slot == 1)
		snprintf(out, n, "MAIN SAVE");
	else
		snprintf(out, n, "SAVE %d", G.pick_saves[i].slot);
}

// The list of saves full width; A opens a sheet with the selected one's actions.
static void pick_draw(canvas *c)
{
	int g = G.pick_game, rows = G.pick_n + (G.pick_new ? 1 : 0);
	uint32_t col = PAL[APP_GAMES];
	fill(c, 0, 0, W, H, 0x0e0e0e);
	ingame_header(c, "SAVES", lib[g].title, GREY);

	const int top = UI_TOP, view = (H - 52 - top) / UI_ROW;
	if (G.pick_sel < G.pick_scroll)
		G.pick_scroll = G.pick_sel;
	if (G.pick_sel >= G.pick_scroll + view)
		G.pick_scroll = G.pick_sel - view + 1;
	ui_column(UI_MX, W - 2 * UI_MX);
	for (int i = G.pick_scroll; i < rows && i < G.pick_scroll + view; i++) {
		int y = top + (i - G.pick_scroll) * UI_ROW, sel = i == G.pick_sel;
		char label[24];
		pick_label(i, label, sizeof label);
		ui_row(c, y, label, sel, col);
		const char *val = NULL;
		uint32_t vc = 0xbbbbbb;
		if (i < G.pick_n) {
			const coz_save *sv = &G.pick_saves[i];
			int ri = find_running_copy(g, sv->slot - 1);
			val = sv->has_save ? save_when(sv->time) : sv->slot == 1 ? "NOT SAVED YET" : "EMPTY";
			if (ri >= 0)
				val = G.pick_ingame && ri == cur ? "PLAYING" : "OPEN", vc = PAL[3];
		} else {
			val = "START FRESH", vc = GREY;
		}
		ui_row_value(c, y, val, 0, sel, col, vc);
	}
	ui_column(0, 0);
	if (G.pick_scroll > 0 || G.pick_scroll + view < rows) {
		int bob = (int)lrintf(sinf(ui_time() * 6) * 3);
		if (G.pick_scroll > 0)
			scroll_arrow(c, W - UI_MX - 20, top - 10 - bob, -1, WHITE);
		if (G.pick_scroll + view < rows)
			scroll_arrow(c, W - UI_MX - 20, top + view * UI_ROW + 2 + bob, 1, WHITE);
	}

	int acts[8], na = pick_actions(acts);
	if (G.pick_focus) { // the sheet: what to do with the selected save
		char title[24], info[64] = "";
		pick_label(G.pick_sel, title, sizeof title);
		if (G.pick_sel < G.pick_n) {
			const coz_save *sv = &G.pick_saves[G.pick_sel];
			int ri = find_running_copy(g, sv->slot - 1);
			if (ri >= 0)
				snprintf(info, sizeof info, G.pick_ingame && ri == cur ? "YOU'RE PLAYING IT" : "OPEN: CLOSE IT TO CHANGE IT");
			else if (sv->has_save)
				snprintf(info, sizeof info, "SAVED %s%s", save_when(sv->time), sv->has_backup ? " - OLDER KEPT" : "");
			else
				snprintf(info, sizeof info, "THE GAME HASN'T SAVED IN IT YET");
		} else {
			snprintf(info, sizeof info, "A NEW PLAYTHROUGH FROM THE START");
		}
		const int ph = 52 + na * UI_ROW + 8, py = H - 46 - ph;
		int ry = ui_panel(c, UI_MX - 8, py, W - 2 * UI_MX + 16, ph, title, col);
		text(c, W - UI_MX - 8 - text_width(info, 2), py + 20, info, 2, GREY);
		ui_column(UI_MX, W - 2 * UI_MX);
		for (int k = 0; k < na; k++) {
			int sel = k == G.pick_act, y = ry + k * UI_ROW;
			uint32_t ac = acts[k] == PA_DELETE ? PAL[0] : col;
			if (pick_needs_hold(acts[k]))
				ui_row_hold(c, y, pick_act_label(acts[k]), sel, ac, sel ? G.pick_hold / HOLD_TIME : 0);
			else
				ui_row(c, y, pick_act_label(acts[k]), sel, ac);
		}
		ui_column(0, 0);
	}

	// hints
	const char *b[3], *l[3];
	b[0] = "^V", l[0] = G.pick_focus ? "MOVE" : "CHOOSE";
	b[1] = "A", l[1] = !G.pick_focus ? "OPEN" : pick_needs_hold(acts[G.pick_act < na ? G.pick_act : 0]) ? "HOLD" : "OK";
	b[2] = "B", l[2] = G.pick_focus || G.pick_ingame ? "BACK" : "CLOSE";
	static const uint32_t hc[3] = {0xdddddd, 0x5ee05e, 0xff4a4a};
	int hx = 0;
	for (int k = 0; k < 3; k++)
		draw_hint(NULL, &hx, b[k], l[k], 0);
	hx = (W - (hx - 22)) / 2;
	for (int k = 0; k < 3; k++)
		draw_hint(c, &hx, b[k], l[k], hc[k]);
}

static void hub_update(unsigned p, float dt)
{
	int v[NGAMES + MAXRUN], n = hub_order(v);
	int idx = index_of(v, n, hub_sel_item());
	int g = G.game, ri = hub_sel_run();
	G.changed += dt;

	G.cam -= G.bump_off, G.camx -= G.bump_offx; // last frame's wiggle
	G.cam += (idx - G.cam) * (1 - expf(-dt * 14));
	G.camx += (cover_pos(idx) - G.camx) * (1 - expf(-dt * 14));
	// bumping into an end nudges the row that way and springs back (home screen does the same)
	G.bump += dt;
	float wig = G.bump < 0.3f ? G.bump_dir * 0.12f * sinf(G.bump / 0.3f * PI) : 0;
	G.bump_off = wig;
	G.bump_offx = wig * (n > 1 ? fabsf(cover_pos(1) - cover_pos(0)) : 100);
	G.cam += G.bump_off, G.camx += G.bump_offx;

	int d = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
	int ni = idx;
	if (G.hub_style == 2) { // grid: 2D movement
		if (d)
			ni = idx + d;
		if (p & BTN_DOWN)
			ni = idx + GRID_COLS;
		if (p & BTN_UP)
			ni = idx - GRID_COLS;
	} else {
		if (G.hub_style == 0) { // shelf has rows: tiles / buttons / save cards
			if (p & BTN_DOWN && G.row < HUB_CARDS)
				G.row++, sfx_play(SFX_TICK, 0);
			if (p & BTN_UP && G.row > HUB_TILES)
				G.row--, sfx_play(SFX_TICK, 0);
		} else {
			G.row = HUB_TILES;
		}
		if (G.row == HUB_TILES)
			ni = idx + (d ? d : (p & BTN_R) ? 1 : (p & BTN_L) ? -1 : 0);
		else if (G.row == HUB_BUTTONS) {
			int nb = ri >= 0 ? 3 : 2;
			if (d && G.btn + d >= 0 && G.btn + d < nb)
				G.btn += d, sfx_play(SFX_TICK, 0);
			if (G.btn >= nb)
				G.btn = nb - 1;
		} else if (d && G.card + d >= 0 && G.card + d < NSLOTS) {
			G.card += d, sfx_play(SFX_TICK, 0);
		}
	}
	if (ni != idx) {
		if (ni >= 0 && ni < n) {
			hub_select(v[ni]);
			G.changed = 0;
			G.btn = BT_PLAY;
			sfx_play(SFX_MOVE, ni);
		} else {
			sfx_play(SFX_BUMP, 0);
			G.bump = 0;
			G.bump_dir = ni > idx ? 1 : -1;
		}
		return;
	}

	if (p & BTN_A) {
		if (G.hub_style == 0 && G.row == HUB_BUTTONS) {
			hub_activate(G.btn, g, ri);
		} else if (G.hub_style == 0 && G.row == HUB_CARDS) {
			if (slots[g][G.card].valid) {
				open_game(g, G.card, 80 + G.card * 150, 360);
			} else {
				snprintf(toast_buf, sizeof toast_buf, "SLOT %d IS EMPTY", G.card + 1);
				sfx_play(SFX_BUMP, 0);
				toast(toast_buf);
			}
		} else {
			hub_activate(BT_PLAY, g, ri);
		}
	}
	if (p & BTN_Y)
		hub_activate(BT_FAV, g, ri);
	if ((p & BTN_X) && ri >= 0)
		hub_activate(BT_CLOSE, g, ri);
	else if ((p & BTN_X) && lib[g].cg)
		pick_open(g);
	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		go(SCR_HOME, BG0, W / 2, H / 2);
	}
}

// Variation 0: PS5 shelf
static void hub_draw_shelf(canvas *c, const int *v, int n)
{
	int g = G.game, ri = hub_sel_run(), si = hub_sel_item();
	draw_key_art(c, g, 0.25f);

	const int sp = 70, ts = 56, big = 76, x0 = 24, ty = 54;
	for (int i = 0; i < n; i++) {
		int x = x0 + (int)lrintf((i - G.cam) * sp) + (i > G.cam + 0.5f ? big - ts : 0);
		if (x < -big || x > W)
			continue;
		int sel = v[i] == si;
		int s = sel ? big : ts;
		int y = sel ? ty - 6 : ty + (big - ts) / 2 - 6;
		if (sel && G.row == HUB_TILES)
			wfocus(c, x, y, s, s, 6);
		draw_cover(c, item_game(v[i]), x, y, s);
		if (!sel)
			darken(c, x - 3, y - 3, s + 6, s + 6, 0.35f);
		if (item_run(v[i]) >= 0)
			running_badge(c, x + s - 16, y + s - 16);
	}

	text_arcade(c, 26, 158, lib[g].title, 4, lib[g].col);
	char buf[64];
	info_line(g, buf, sizeof buf);
	text(c, 28, 204, buf, 2, ri >= 0 ? PAL[3] : 0xcccccc);

	int bf = G.row == HUB_BUTTONS;
	pill(c, 28, 236, 136, ri >= 0 ? "RESUME" : "PLAY", bf && G.btn == BT_PLAY, 1);
	pill(c, 178, 236, 136, lib[g].fav ? "* FAVORITE" : "FAVORITE", bf && G.btn == BT_FAV, 0);
	if (ri >= 0)
		pill(c, 328, 236, 120, "CLOSE", bf && G.btn == BT_CLOSE, 0);

	text(c, 28, 300, "SNAPSHOTS", 2, 0xcccccc);
	for (int k = 0; k < NSLOTS; k++) {
		slot_t *sl = &slots[g][k];
		int x = 28 + k * 150, y = 324;
		int sel = G.row == HUB_CARDS && G.card == k;
		if (sel)
			y -= 4;
		if (sel)
			wfocus(c, x, y, 120, 80, 7);
		wframe(c, x, y, 120, 80, 0x444444);
		if (sl->valid) {
			blit_game(c, sl->thumb, TW, TH, x, y, 120, 80, 0);
		} else {
			fill(c, x, y, 120, 80, 0x1a1a1a);
			text(c, x + 60 - text_width("EMPTY", 2) / 2, y + 34, "EMPTY", 2, 0x666666);
		}
		snprintf(buf, sizeof buf, "%d", k + 1);
		rrect(c, x + 4, y + 4, 20, 20, 3, sel ? WHITE : 0x333333);
		text(c, x + 9, y + 7, buf, 2, sel ? BLACK : WHITE);
	}
}

// Variation 1: big cover carousel. Open games show their live screen with a
// green frame and sit in their own group, separated from the library.
// Square crop of a 240x160 frame as a tile: black ring, coloured border and a
// tag in the corner ("OPEN", "LAST PLAYED").
static void draw_frame_tile(canvas *c, const uint32_t *frame, int x, int y, int s,
			    uint32_t border, const char *tag, int play_icon)
{
	if (!skip_frame)
		wframe(c, x, y, s, s, border);
	for (int yy = 0; yy < s; yy++) {
		int py = y + yy;
		if (py < 0 || py >= c->h)
			continue;
		const uint32_t *in = frame + (size_t)(yy * GH / s) * GW + (GW - GH) / 2;
		uint32_t *out = c->px + (size_t)py * c->stride;
		for (int xx = 0; xx < s; xx++)
			if (x + xx >= 0 && x + xx < c->w)
				out[x + xx] = in[xx * GH / s];
	}
	int icon = play_icon ? 1 : 0;
	int tw = text_width(tag, 2) + 12 + icon * 12 <= s - 8 ? 2 : 1;
	int w = text_width(tag, tw) + 8 + icon * (4 + 4 * tw), h = 7 * tw + 8;
	rrect(c, x + 4, y + 4, w, h, 3, border);
	if (icon)
		for (int k = 0; k < 3; k++)
			fill(c, x + 8 + k * tw, y + 8 + k * tw, tw, 7 * tw - 2 * k * tw, BLACK);
	text(c, x + 8 + icon * (4 + 4 * tw), y + 8, tag, tw, BLACK);
}

#define LINK_COL PAL[4] // cyan: linked games

static int is_linked(int i)
{
	return i >= 0 && i < nrun && emu_linked(run[i].eg) > 1;
}

static void draw_open_cover(canvas *c, int ri, int x, int y, int s)
{
	if (is_linked(ri))
		draw_frame_tile(c, run[ri].frame, x, y, s, LINK_COL, "LINKED", 0);
	else
		draw_frame_tile(c, run[ri].frame, x, y, s, PAL[3], "OPEN", 1);
}

// Small "LINKED" chip (top-left at x, y).
// A small coloured label (top-left at x, y). Returns its width.
static int chip(canvas *c, int x, int y, int scale, const char *label, uint32_t col)
{
	int w = text_width(label, scale) + 6 * scale, h = 7 * scale + 4 * scale;
	rrect(c, x, y, w, h, scale + 1, col);
	text(c, x + 3 * scale, y + 2 * scale, label, scale, BLACK);
	return w;
}

static void link_chip(canvas *c, int x, int y, int scale)
{
	chip(c, x, y, scale, "LINKED", LINK_COL);
}

// Is another instance of open game i's game open? (then "#n" tells them apart)
static int has_twin(int i)
{
	for (int k = 0; k < nrun; k++)
		if (k != i && run[k].game == run[i].game)
			return 1;
	return 0;
}

/* Cover frame styles being compared: 0 current (widget frame + focus ring),
 * 1 clean, 2 thin, 3 shadow. Drawn before the image, around it. */
static int cover_style = 2; // thin solid border, chosen 2026-11-21

void games_set_cover_style(int st)
{
	cover_style = st;
}

static void ring(canvas *c, int x, int y, int w, int h, int t, uint32_t col)
{
	fill(c, x, y, w, t, col), fill(c, x, y + h - t, w, t, col);
	fill(c, x, y + t, t, h - 2 * t, col), fill(c, x + w - t, y + t, t, h - 2 * t, col);
}

static void cover_frame(canvas *c, int x, int y, int s, int sel, int open)
{
	uint32_t green = open == 2 ? PAL[4] : PAL[3]; // cyan when linked
	switch (cover_style) {
	case 1: // clean: dark edge; selection = one white ring with a gap
		ring(c, x - 1, y - 1, s + 2, s + 2, 1, BLACK);
		if (open)
			ring(c, x - 4, y - 4, s + 8, s + 8, 3, green);
		if (sel)
			ring(c, x - 9, y - 9, s + 18, s + 18, 4, WHITE);
		break;
	case 2: // thin: grey (or green) ring that turns white and thicker when selected
		if (sel)
			ring(c, x - 5, y - 5, s + 10, s + 10, 5, open ? green : WHITE);
		else
			ring(c, x - 2, y - 2, s + 4, s + 4, 2, open ? green : 0x8a8a8a);
		if (sel && open)
			ring(c, x - 8, y - 8, s + 16, s + 16, 3, WHITE);
		break;
	case 3: // shadow: no rings, a drop shadow; selection = white ring
		fill(c, x + 6, y + 8, s, s, 0x050505);
		if (open)
			ring(c, x - 3, y - 3, s + 6, s + 6, 3, green);
		if (sel)
			ring(c, x - 8, y - 8, s + 16, s + 16, 3, WHITE);
		break;
	}
}

static void hub_draw_covers(canvas *c, const int *v, int n)
{
	int g = G.game, ri = hub_sel_run(), si = hub_sel_item();
	draw_key_art(c, g, 0.45f);
	const int big = 216, small = 136, cy = 178;

	// group labels above each group's first cover
	int top = cy - big / 2 - 30;
	char lbl[24];
	if (nopen) {
		int x = W / 2 + (int)lrintf(cover_pos(0) - G.camx) - small / 2;
		snprintf(lbl, sizeof lbl, "OPEN (%d)", nopen);
		fill(c, x, top + 4, 10, 10, PAL[3]);
		text(c, x + 16, top + 2, lbl, 2, PAL[3]);
		// divider between the groups
		if (nopen < n) {
			int dx = W / 2 + (int)lrintf(cover_pos(nopen) - G.camx) - COVER_GAP / 2 - small / 2 - 30;
			for (int yy = top; yy < cy + big / 2; yy += 10)
				fill(c, dx, yy, 3, 6, 0x666666);
		}
	}
	if (nopen < n) {
		int x = W / 2 + (int)lrintf(cover_pos(nopen) - G.camx) - small / 2;
		text(c, x, top + 2, "ALL GAMES", 2, 0xdddddd);
	}

	for (int pass = 0; pass < 2; pass++) // neighbours first, selection on top
		for (int i = 0; i < n; i++) {
			int sel = v[i] == si;
			if (sel != pass)
				continue;
			int s = sel ? big : small;
			int x = W / 2 + (int)lrintf(cover_pos(i) - G.camx) - s / 2;
			if (x < -s || x > W)
				continue;
			int y = cy - s / 2 + (sel ? -2 * (int)lrintf(sinf(ui_time() * 4)) : 0);
			int r = item_run(v[i]);
			if (cover_style)
				cover_frame(c, x, y, s, sel, r < 0 ? 0 : is_linked(r) ? 2 : 1);
			else if (sel)
				wfocus(c, x, y, s, s, 9);
			skip_frame = cover_style != 0;
			if (r >= 0)
				draw_open_cover(c, r, x, y, s);
			else
				draw_cover(c, item_game(v[i]), x, y, s);
			skip_frame = 0;
			if (!sel)
				darken(c, x - 5, y - 5, s + 10, s + 10, r >= 0 ? 0.35f : 0.6f);
		}

	const char *title = ri >= 0 ? inst_title(ri) : lib[g].title;
	int ts = 4;
	while (ts > 2 && text_width(title, ts) > W - 30)
		ts--;
	int tw = text_width(title, ts);
	text_arcade(c, (W - tw) / 2, 316 + (4 - ts) * 7, title, ts, lib[g].col);
	char buf[64];
	int also = ri < 0 ? find_running(g) : -1; // in all games, but also open
	if (ri >= 0 || also >= 0) { // status pill: open
		games_open_pill(c, W / 2, 364, ri >= 0 ? ri : also);
	} else {
		if (lib[g].recent)
			snprintf(buf, sizeof buf, "LAST PLAYED %s", last_text(g));
		else
			snprintf(buf, sizeof buf, "NOT PLAYED YET");
		text(c, (W - text_width(buf, 3)) / 2, 366, buf, 3, 0xaaaaaa);
	}
}

// Variation 2: grid + details panel
static void hub_draw_grid(canvas *c, const int *v, int n)
{
	int g = G.game, ri = hub_sel_run(), si = hub_sel_item();
	draw_header(c, APP_GAMES, "GAMES");
	const int cs = 64, sp = 76, x0 = 28, y0 = 140;
	for (int i = 0; i < n; i++) {
		int x = x0 + (i % GRID_COLS) * sp, y = y0 + (i / GRID_COLS) * (sp + 8);
		int sel = v[i] == si;
		if (sel) {
			y -= 4;
			wfocus(c, x, y, cs, cs, 7);
		}
		draw_cover(c, item_game(v[i]), x, y, cs);
		if (!sel)
			darken(c, x - 3, y - 3, cs + 6, cs + 6, 0.3f);
		if (item_run(v[i]) >= 0)
			running_badge(c, x + cs - 14, y + cs - 14);
		if (lib[item_game(v[i])].fav)
			text(c, x + 4, y + 4, "*", 2, PAL[2]);
	}
	// details panel
	int px = 420, pw = 200;
	rrect(c, px - 4, 120, pw + 8, 318, 6, BLACK);
	rrect(c, px - 2, 122, pw + 4, 314, 5, 0x1c1c1c);
	const uint32_t *pic = ri >= 0 ? run[ri].frame : art[g];
	blit_game(c, pic, GW, GH, px, 122, pw, 133, 0);
	fill(c, px, 255, pw, 3, lib[g].col);
	text(c, px + 10, 268, lib[g].title, 2, WHITE);
	text(c, px + 10, 292, lib[g].genre, 2, lib[g].col);
	char buf[32];
	if (ri >= 0)
		text(c, px + 10, 316, "RUNNING", 2, PAL[3]);
	else if (lib[g].recent) {
		snprintf(buf, sizeof buf, "LAST PLAYED");
		text(c, px + 10, 316, buf, 2, GREY);
		snprintf(buf, sizeof buf, "%s", last_text(g));
		text(c, px + 10, 340, buf, 2, GREY);
	} else {
		text(c, px + 10, 316, "NEW", 2, GREY);
	}
	pill(c, px + 10, 388, pw - 20, ri >= 0 ? "RESUME" : "PLAY", 0, 1);
}

static void hub_draw(canvas *c)
{
	int v[NGAMES + MAXRUN], n = hub_order(v);
	if (G.hub_style == 1)
		hub_draw_covers(c, v, n);
	else if (G.hub_style == 2)
		hub_draw_grid(c, v, n);
	else
		hub_draw_shelf(c, v, n);
}

static int ref_game(int ref);

// The picker is an overlay over the game list or the home screen.
int games_pick_active(void) { return G.pick; }
void games_pick_update(unsigned p, float dt) { G.from_menu = 1; pick_update(p, G.pick_game, dt); }
void games_pick_draw(canvas *c)
{
	if (G.pick && !G.cc)	// in a game the in-game menu draws it on top of itself
		pick_draw(c);
}
// ref < 0: a game that isn't open. Has several saves to choose from (library game)?
int games_ref_has_saves(int ref) { return ref < 0 && lib[ref_game(ref)].cg != NULL; }
void games_ref_saves(int ref)
{
	if (games_ref_has_saves(ref))
		pick_open(ref_game(ref));
}

/* ---- game screen ----------------------------------------------------------- */

static void draw_game_view(canvas *c, inst_t *in)
{
	uint32_t col = lib[in->game].col;
	if (G.screen_mode == 1) { // full width, keeping the 3:2 shape
		fill(c, 0, 0, W, H, BLACK);
		blit_game(c, in->frame, GW, GH, 0, (H - 427) / 2, W, 427, G.filter);
		return;
	}
	if (G.screen_mode == 2) { // stretch to the whole screen
		blit_game(c, in->frame, GW, GH, 0, 0, W, H, G.filter);
		return;
	}
	for (int y = 0; y < H; y++)
		fill(c, 0, y, W, 1, mix(0x0c0c0c, mix(col, BLACK, 0.8f), (float)y / H));
	const int sx = 80, sy = 72;
	rrect(c, sx - 10, sy - 10, 500, 340, 8, BLACK);
	rrect(c, sx - 6, sy - 6, 492, 332, 6, WHITE);
	rrect(c, sx - 3, sy - 3, 486, 326, 4, BLACK);
	blit_game(c, in->frame, GW, GH, sx, sy, 480, 320, G.filter);
	text(c, sx, 40, inst_title(cur), 2, 0xbbbbbb);
	fill(c, sx + 480 - 30, 44, 8, 8, PAL[3]);
	static const char word[] = "COZISBOY";
	for (int i = 0; word[i]; i++) {
		char s1[2] = {word[i], 0};
		text(c, W / 2 - 48 + i * 12, 420, s1, 2, PAL[i]);
	}
}

// Controls popup: swap A/B, turbo on the otherwise unused X/Y.
static unsigned map_buttons(unsigned held)
{
	unsigned b = held & ~(BTN_MENU | BTN_X | BTN_Y | BTN_A | BTN_B);
	unsigned a = held & BTN_A, bb = held & BTN_B;
	if (input_items[IN_SWAP].val)
		a = held & BTN_B ? BTN_A : 0, bb = held & BTN_A ? BTN_B : 0;
	int pulse = (G.frame_no / 3) & 1; // turbo: 10 presses per second
	if (input_items[IN_TURBO_A].val && (held & BTN_X) && pulse)
		a = BTN_A;
	if (input_items[IN_TURBO_B].val && (held & BTN_Y) && pulse)
		bb = BTN_B;
	return b | a | bb;
}

static void game_update(unsigned p, unsigned held, float dt)
{
	G.frame_no++;
	if (G.from_menu) { // back in the game: what's held now is the menu's
		G.swallow = held;
		G.from_menu = 0;
	}
	G.swallow &= held;	// released: the game gets the button again
	held &= ~G.swallow;
	p &= ~G.swallow;
	if (cur < 0)
		return;
	inst_t *in = &run[cur];
	// paced by real time (and the sound buffer), not by screen refreshes
	int steps = emu_frames_due(dt) * (G.ff ? 3 : 1);
	if (is_emu(in->game)) {
		if (G.ff)	// fast-forward: only the first frame's sound
			emu_run(in->eg, map_buttons(held), steps);
		else	// every frame's sound
			for (int k = 0; k < steps; k++)
				emu_run(in->eg, map_buttons(held), 1);
		memcpy(in->frame, emu_frame(in->eg), sizeof in->frame);
		if (in->boot > 0) {
			// A starting game blanks its screen (white on a GBA), or shows flat
			// colour blocks: until a frame has some detail (4+ colours), show
			// black, then fade the first real picture in.
			uint32_t seen[4];
			int nseen = 0;
			for (int k = 0; k < GW * GH && nseen < 4; k += 3) {
				int known = 0;
				for (int q = 0; q < nseen && !known; q++)
					known = seen[q] == in->frame[k];
				if (!known)
					seen[nseen++] = in->frame[k];
			}
			if (nseen < 4) {
				memset(in->frame, 0, sizeof in->frame);
				in->boot -= steps, in->fade = 0;
			} else {
				in->boot = 0;
				if (ui_transitioning())
					in->fade = 1; // the transition reveals it already
			}
		} else if (in->fade < 1) {
			in->fade += dt * 4;
		}
		for (int k = 0; k < nrun; k++) // linked games ran too: keep their pictures live
			if (linked_with_cur(k))
				memcpy(run[k].frame, emu_frame(run[k].eg), sizeof run[k].frame);
		return;
	}
	for (int k = 0; k < steps; k++)
		game_step(&in->st, held, k ? 0 : p, dt);
	game_render(&in->st, in->game, in->frame, 1);
}

/* ---- control center ---------------------------------------------------------- */

// cards: HOME, running games (most recent first), ALL GAMES
static int cc_cards(int *v)
{
	int r[MAXRUN], n = running_order(r), k = 0;
	v[k++] = -1; // home
	for (int i = 0; i < n; i++)
		v[k++] = r[i];
	v[k++] = -2; // all games
	return k;
}

static int cc_has_actions(void)
{
	return ui_screen() == SCR_GAME && cur >= 0;
}

int games_cc_active(void)
{
	return G.cc;
}

void games_cc_close(void)
{
	G.cc = 0;
}

static void ingame_reset(void);

void games_menu_tap(void)
{
	if (G.cc) {
		G.cc = 0;
		sfx_play(SFX_TOGGLE_OFF, 0);
		return;
	}
	G.cc = 1;
	G.cc_sub = 0;
	G.cc_t = 0;
	ingame_reset();
	G.cc_row = 0;
	// start on the current game's card, else on the first running game
	int v[MAXRUN + 2], n = cc_cards(v);
	G.cc_card = n > 2 ? 1 : 0;
	for (int i = 0; i < n; i++)
		if (v[i] >= 0 && v[i] == cur && ui_screen() == SCR_GAME)
			G.cc_card = i;
	G.cc_item = G.cc_card;
	G.cc_cam = (float)G.cc_card;
	sfx_play(SFX_TOGGLE_ON, 0);
}

void games_menu_double(void)
{
	// most recently used running game that isn't the one on screen
	int r[MAXRUN], n = running_order(r);
	for (int i = 0; i < n; i++) {
		if (ui_screen() == SCR_GAME && r[i] == cur)
			continue;
		G.cc = 0;
		enter_game(r[i], W / 2, H / 2);
		return;
	}
	sfx_play(SFX_BUMP, 0);
}

static void quick_action(int a)
{
	inst_t *in = &run[cur];
	slot_t *sl = &slots[in->game][G.slot];
	switch (a) {
	case QA_SAVE:
		if (is_emu(in->game)) {
			size_t n = emu_state_size(in->eg);
			if (sl->n != n) {
				free(sl->blob);
				sl->blob = malloc(n);
				sl->n = sl->blob ? n : 0;
			}
			if (!sl->blob || emu_save_state(in->eg, sl->blob, sl->n)) {
				toast("SNAPSHOT FAILED");
				break;
			}
		}
		if (lib[in->game].cg) { // to disk: a new state (an overwritten one is replaced)
			coz_state out;
			if (coz_state_add(lib[in->game].cg, sl->blob, sl->n, in->frame, GW, GH, GW, &out)) {
				toast("SNAPSHOT FAILED");
				break;
			}
			if (sl->sid[0])
				coz_state_delete(lib[in->game].cg, sl->sid);
			snprintf(sl->sid, sizeof sl->sid, "%s", out.id);
		}
		sl->valid = 1;
		sl->t = (long)time(NULL);
		sl->st = in->st;
		for (int y = 0; y < TH; y++)
			for (int x = 0; x < TW; x++)
				sl->thumb[y * TW + x] = in->frame[(y * 2) * GW + x * 2];
		snprintf(toast_buf, sizeof toast_buf, "SNAPSHOT TAKEN");
		sfx_play(SFX_CONFIRM, 0);
		toast(toast_buf);
		break;
	case QA_LOAD:
		if (!sl->valid) {
			snprintf(toast_buf, sizeof toast_buf, "SLOT %d IS EMPTY", G.slot + 1);
			sfx_play(SFX_BUMP, 0);
		} else {
			if (is_emu(in->game)) {
				if (slot_data(in->game, sl) || emu_load_state(in->eg, sl->blob, sl->n)) {
					toast("COULD NOT LOAD THIS SNAPSHOT");
					sfx_play(SFX_BUMP, 0);
					break;
				}
			} else {
				in->st = sl->st;
				game_render(&in->st, in->game, in->frame, 1);
			}
			snprintf(toast_buf, sizeof toast_buf, "SNAPSHOT LOADED");
			sfx_play(SFX_CONFIRM, 0);
			G.cc = 0;
		}
		toast(toast_buf);
		break;
	case QA_SLOT:
		G.slot = (G.slot + 1) % NSLOTS;
		sfx_play(SFX_CHANGE, G.slot * 3);
		break;
	case QA_AV:
	case QA_INPUT: {
		list_t *l = a == QA_AV ? &av_list : &input_list;
		l->sel = 0, l->y = 0;
		av_items[AV_VOLUME].val = ui_volume();
		G.cc_sub = a;
		sfx_play(SFX_OPEN, APP_SETTINGS);
		break;
	}
	case QA_RESET:
		if (is_emu(in->game))
			emu_reset(in->eg), in->boot = 120, in->fade = 0;
		else
			game_reset(&in->st);
		G.cc = 0;
		sfx_play(SFX_CONFIRM, 0);
		toast("GAME RESET");
		break;
	case QA_CLOSE: {
		int g = in->game;
		close_running(cur);
		G.cc = 0;
		G.game = g, G.hri = -1;
		snprintf(toast_buf, sizeof toast_buf, "%s CLOSED", lib[g].title);
		toast(toast_buf);
		sfx_play(SFX_BACK, 0);
		go(SCR_LIBRARY, BG0, W / 2, H / 2);
		break;
	}
	}
}

static void activate_card(int sel, int x, int y)
{
	G.cc = 0;
	if (sel == -1) {
		sfx_play(SFX_BACK, 0);
		go(SCR_HOME, BG0, x, y);
	} else if (sel == -2) {
		games_hub_focus_first();
		sfx_play(SFX_OPEN, APP_GAMES);
		go(SCR_LIBRARY, PAL[APP_GAMES], x, y);
	} else if (sel == cur && ui_screen() == SCR_GAME) {
		sfx_play(SFX_TOGGLE_OFF, 0); // already playing it: just resume
	} else {
		enter_game(sel, x, y);
	}
}

/* ---- in-game menu: one vertical list with sections ----------------------------
 * GAME (resume, save, load, slot, reset, close), SWITCH (home, other open games,
 * all games), VIDEO, AUDIO, CONTROLS. A preview of the game sits on the right. */

enum { E_HEADER, E_ACTION, E_OPTION, E_CARD };
// (no RESUME row: B resumes)
enum { ACT_SAVES, ACT_STATES, ACT_SETTINGS, ACT_COPY, ACT_LINK, ACT_RESET, ACT_CLOSE, NACT };
static const char *const ACT_NAMES[NACT] = {"SAVES", "SNAPSHOTS", "SETTINGS", "ANOTHER COPY", "LINK CABLE",
					    "RESET", "CLOSE GAME"};

typedef struct {
	int kind, id;     // id: ACT_* for actions, card id for cards (-1 home, -2 all)
	const char *label;
	item_t *opt;      // E_OPTION
} entry_t;

#define MAXENT 32
static entry_t ents[MAXENT];
static int nents;
static struct {
	int col;        // 0: this game's actions, 1: open games / home / all games
	int sel[2];
	float sel_y[2];
	int page;       // 0: main, 1: settings, 2: link cable, 3: another copy
	int lsel;       // link page selection
	float lhold;    // hold-to-confirm progress on the link page
	int larmed;
	int ssel;       // settings page selection (index into ents)
	float ssel_y;
} M2;

static void ent(int kind, int id, const char *label, item_t *opt)
{
	if (nents < MAXENT)
		ents[nents++] = (entry_t){kind, id, label, opt};
}

// Settings page entries (the main view is built directly in update/draw)
static void build_ingame(void)
{
	nents = 0;
	ent(E_HEADER, 0, "VIDEO", NULL);
	ent(E_OPTION, 0, NULL, &av_items[AV_SCREEN]);
	ent(E_OPTION, 0, NULL, &av_items[AV_FILTER]);
	ent(E_OPTION, 0, NULL, &av_items[AV_COLOR]);
	ent(E_HEADER, 0, "AUDIO", NULL);
	ent(E_OPTION, 0, NULL, &av_items[AV_VOLUME]);
	ent(E_HEADER, 0, "CONTROLS", NULL);
	for (int i = 0; i < 3; i++)
		ent(E_OPTION, 0, NULL, &input_items[i]);
}

static void apply_options(void)
{
	G.screen_mode = av_items[AV_SCREEN].val;
	G.filter = av_items[AV_FILTER].val;
	if (av_items[AV_VOLUME].val != ui_volume())
		ui_set_volume(av_items[AV_VOLUME].val);
	static int color = -1;
	if (color != av_items[AV_COLOR].val) {
		color = av_items[AV_COLOR].val;
		emu_set_color_fix(color);
	}
}

// Left/Right/A on an option: same behaviour as the Settings lists.
static void change_option(item_t *it, unsigned p)
{
	int d = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
	if (it->type == IT_SLIDER && d) {
		int old = it->val;
		it->val = (int)clampf((float)(it->val + d), 0, (float)it->max);
		sfx_play(it->val != old ? SFX_CHANGE : SFX_BUMP, it->val);
	} else if (it->type == IT_CHOICE && (d || (p & BTN_A))) {
		it->val = (it->val + (d ? d : 1) + it->max) % it->max;
		sfx_play(SFX_CHANGE, it->val * 3);
	} else if (it->type == IT_TOGGLE && (d || (p & BTN_A))) {
		it->val = !it->val;
		sfx_play(it->val ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF, 0);
	}
}

/* ---- save states view: every save as a card, newest first ------------------- */

static struct {
	int on, sel, popup, psel;
	float scroll;
	float hold;  // how long A has been held on the chosen option
	int armed;   // A was released since the popup opened
} SV;
#define HOLD_TIME 0.9f
enum { SP_LOAD, SP_OVERWRITE, SP_DELETE };

// Saves of the current game, newest first. Returns how many.
static int states_order(int *v)
{
	int g = run[cur].game, n = 0;
	for (int k = 0; k < NSLOTS; k++)
		if (slots[g][k].valid)
			v[n++] = k;
	for (int i = 1; i < n; i++)
		for (int j = i; j > 0 && slots[g][v[j]].t > slots[g][v[j - 1]].t; j--) {
			int t = v[j];
			v[j] = v[j - 1], v[j - 1] = t;
		}
	return n;
}

// "JUST NOW", "12 MIN AGO", "TODAY 14:32", "YESTERDAY 09:10", "3 DAYS AGO"
static const char *when_text(long t)
{
	static char buf[32];
	time_t now = time(NULL), then = t;
	long ago = (long)difftime(now, then);
	if (ago < 60)
		return "JUST NOW";
	if (ago < 3600) {
		snprintf(buf, sizeof buf, "%ld MIN AGO", ago / 60);
		return buf;
	}
	struct tm a, b;
	localtime_r(&now, &a);
	localtime_r(&then, &b);
	if (a.tm_yday == b.tm_yday && a.tm_year == b.tm_year)
		snprintf(buf, sizeof buf, "TODAY %02d:%02d", b.tm_hour, b.tm_min);
	else if (ago < 2 * 86400)
		snprintf(buf, sizeof buf, "YESTERDAY %02d:%02d", b.tm_hour, b.tm_min);
	else
		snprintf(buf, sizeof buf, "%ld DAYS AGO", ago / 86400);
	return buf;
}

static void states_open(void)
{
	memset(&SV, 0, sizeof SV);
	SV.on = 1;
	ui_page_anim(0);
	sfx_play(SFX_OPEN, APP_GAMES);
}

static void states_update(unsigned p, float dt)
{
	int v[NSLOTS], n = states_order(v), cards = n + 1; // card 0: new save
	int g = run[cur].game;
	if (SV.sel >= cards)
		SV.sel = cards - 1;
	if (SV.popup) {
		// Load, overwrite and delete lose something: hold A to confirm
		int a_down = (ui_held() & BTN_A) != 0;
		if (!a_down)
			SV.armed = 1;
		if (p & (BTN_UP | BTN_DOWN)) {
			SV.psel = (SV.psel + ((p & BTN_DOWN) ? 1 : 2)) % 3;
			SV.hold = 0;
			sfx_play(SFX_TICK, 0);
		}
		if (p & BTN_B) {
			SV.popup = 0, SV.hold = 0;
			sfx_play(SFX_BACK, 0);
			return;
		}
		if (SV.armed && a_down) {
			if (SV.hold == 0)
				sfx_play(SFX_TICK, 0);
			SV.hold += dt;
		} else {
			SV.hold = 0;
		}
		if (SV.hold >= HOLD_TIME) {
			int k = v[SV.sel - 1];
			SV.popup = 0, SV.hold = 0;
			G.slot = k;
			if (SV.psel == SP_LOAD) {
				quick_action(QA_LOAD);
				SV.on = 0;
			} else if (SV.psel == SP_OVERWRITE) {
				quick_action(QA_SAVE);
				SV.sel = 1;
			} else {
				slot_delete(g, &slots[g][k]);
				sfx_play(SFX_TOGGLE_OFF, 0);
				toast("SNAPSHOT DELETED");
			}
		}
		return;
	}
	const int cols = 3;
	int d = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : (p & BTN_DOWN) ? cols : (p & BTN_UP) ? -cols : 0;
	if (d) {
		int s2 = SV.sel + d;
		if (s2 >= 0 && s2 < cards)
			SV.sel = s2, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (p & BTN_B) {
		SV.on = 0;
		ui_page_anim(1);
		sfx_play(SFX_BACK, 0);
		return;
	}
	if (!(p & BTN_A))
		return;
	if (SV.sel == 0) { // new save into a free spot
		int k = 0;
		while (k < NSLOTS && slots[g][k].valid)
			k++;
		if (k == NSLOTS) {
			toast("ALL 8 SAVES USED: OVERWRITE ONE");
			sfx_play(SFX_BUMP, 0);
			return;
		}
		G.slot = k;
		quick_action(QA_SAVE);
		SV.sel = 1; // the new save is now the newest card
	} else {
		SV.popup = 1, SV.psel = SP_LOAD;
		SV.hold = 0, SV.armed = 0;
		sfx_play(SFX_OPEN, APP_GAMES);
	}
}

static void ingame_header(canvas *c, const char *title, const char *status, uint32_t scol);

// A solid pixel triangle centred on x, its tip at y; dir -1 points up, 1 down.
// 2 px rows growing 4 px a row: 4 px wide at the tip, 24 at the base, 12 tall.
static void scroll_arrow(canvas *c, int x, int y, int dir, uint32_t col)
{
	for (int k = 0; k < 6; k++) {
		int yy = dir < 0 ? y + k * 2 : y - k * 2 - 1;
		fill(c, x - 2 - k * 2, yy, 4 + k * 4, 2, col);
	}
}

static void states_draw(canvas *c)
{
	uint32_t col = PAL[APP_GAMES];
	int v[NSLOTS], n = states_order(v), cards = n + 1, g = run[cur].game;
	fill(c, 0, 0, W, H, 0x0e0e0e);
	ingame_header(c, "SNAPSHOTS", inst_title(cur), GREY);
	const int cols = 3, gx = 16, cw = (W - 2 * UI_MX - 2 * gx) / 3, ch = cw * 2 / 3, rowh = ch + 38,
		  x0 = UI_MX, y0 = UI_TOP + 6;
	int srow = SV.sel / cols;
	if (srow - (int)SV.scroll > 1)
		SV.scroll = (float)(srow - 1);
	if (srow < SV.scroll)
		SV.scroll = (float)srow;
	for (int i = 0; i < cards; i++) {
		int row = i / cols - (int)SV.scroll;
		if (row < 0 || row > 2)
			continue;
		int x = x0 + (i % cols) * (cw + gx), y = y0 + row * rowh;
		if (y > H - 60)
			continue;
		int sel = i == SV.sel;
		if (sel)
			fill(c, x - 6, y - 6, cw + 12, ch + 12, WHITE), fill(c, x - 3, y - 3, cw + 6, ch + 6, 0x0e0e0e);
		if (i == 0) { // new save
			uint32_t fc = sel ? WHITE : 0x555555;
			for (int k = 0; k < cw; k += 12)
				fill(c, x + k, y, 6, 3, fc), fill(c, x + k, y + ch - 3, 6, 3, fc);
			for (int k = 0; k < ch; k += 12)
				fill(c, x, y + k, 3, 6, fc), fill(c, x + cw - 3, y + k, 3, 6, fc);
			if (n == NSLOTS)
				fc = mix(fc, BG0, 0.6f); // full: nothing to add
			text(c, x + cw / 2 - 14, y + 39, "+", 6, fc);
			continue;
		}
		slot_t *sl = &slots[g][v[i - 1]];
		fill(c, x - 2, y - 2, cw + 4, ch + 4, 0x8a8a8a);
		blit_game(c, sl->thumb, TW, TH, x, y, cw, ch, 0);
		if (!sel)
			darken(c, x, y, cw, ch, 0.35f);
		const char *wt = when_text(sl->t);
		text(c, x + (cw - text_width(wt, 2)) / 2, y + ch + 10, wt, 2, sel ? WHITE : GREY);
	}
	if (!n)
		text(c, x0 + 2, y0 + rowh + 10, "NO SNAPSHOTS YET: PRESS A", UI_TS, 0x666666);
	// more above / below: arrows that bob towards the hidden rows
	int rows_total = (cards + cols - 1) / cols, first = (int)SV.scroll, shown = 0;
	for (int r = 0; r < 3 && y0 + r * rowh <= H - 60; r++)
		shown++;
	if (!SV.popup) {
		// centred, in the gaps above the first row and below the last
		int bob = (int)lrintf(sinf(ui_time() * 6) * 2), ax = W / 2;
		if (first > 0)	// between the title line (ends ~70) and the cards' frame (84)
			scroll_arrow(c, ax, y0 - 17 - bob, -1, WHITE);
		if (first + shown < rows_total)
			scroll_arrow(c, ax, y0 + shown * rowh + 14 + bob, 1, WHITE);
	}
	if (SV.popup) { // sheet: the save's picture + what to do with it (hold A)
		slot_t *sl = &slots[g][v[SV.sel - 1]];
		static const char *const opts[3] = {"LOAD", "OVERWRITE", "DELETE"};
		char title[48];
		snprintf(title, sizeof title, "SAVED %s", when_text(sl->t));
		const int ph = 250, py = H - 48 - ph;
		int ry = ui_panel(c, UI_MX - 8, py, W - 2 * UI_MX + 16, ph, title, col);
		const int iw = 264, ih = 176;
		fill(c, UI_MX - 2, ry - 2, iw + 4, ih + 4, 0x8a8a8a);
		blit_game(c, sl->thumb, TW, TH, UI_MX, ry, iw, ih, 0);
		ui_column(UI_MX + iw + 14, W - UI_MX - (UI_MX + iw + 14));
		for (int k = 0; k < 3; k++)
			ui_row_hold(c, ry + 6 + k * UI_ROW, opts[k], SV.psel == k, k == SP_DELETE ? PAL[0] : col,
				    SV.hold / HOLD_TIME);
		ui_column(0, 0);
	}
	int hx = 0;
	const char *a_lbl = SV.popup ? "HOLD: CONFIRM" : SV.sel ? "OPTIONS" : "TAKE SNAPSHOT";
	draw_hint(NULL, &hx, SV.popup ? "^V" : "<>", "MOVE", 0);
	draw_hint(NULL, &hx, "A", a_lbl, 0);
	draw_hint(NULL, &hx, "B", "BACK", 0);
	hx = (W - (hx - 22)) / 2;
	draw_hint(c, &hx, SV.popup ? "^V" : "<>", "MOVE", 0xdddddd);
	draw_hint(c, &hx, "A", a_lbl, PAL[3]);
	draw_hint(c, &hx, "B", "BACK", PAL[0]);
}

// Right column: other open games (most recent first), then Home and All games.
static int switch_items(int *v)
{
	int r[MAXRUN], n = running_order(r), k = 0;
	for (int i = 0; i < n; i++)
		if (r[i] != cur)
			v[k++] = r[i];
	v[k++] = -1; // home
	v[k++] = -2; // all games
	return k;
}

static void settings_update(unsigned p, float dt)
{
	build_ingame();
	av_items[AV_VOLUME].val = ui_volume();
	if (M2.ssel <= 0 || M2.ssel >= nents || ents[M2.ssel].kind == E_HEADER)
		M2.ssel = 1;
	M2.ssel_y += (M2.ssel - M2.ssel_y) * (1 - expf(-dt * 22));
	if (p & BTN_B) {
		M2.page = 0;
		ui_page_anim(1);
		sfx_play(SFX_BACK, 0);
		return;
	}
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (d) {
		int s2 = M2.ssel + d;
		while (s2 > 0 && s2 < nents && ents[s2].kind == E_HEADER)
			s2 += d;
		if (s2 > 0 && s2 < nents)
			M2.ssel = s2, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	change_option(ents[M2.ssel].opt, p);
	apply_options();
}

/* ---- link cable page ---------------------------------------------------------- */

static void ingame_hints(canvas *c, const char *const *b, const char *const *l, int n);

enum { L_COPY_OWN, L_COPY_MINE, L_WITH, L_UNLINK };
typedef struct {
	int kind, target; // target: open game index for L_WITH
	char label[80];
} lrow_t;

// Next copy number not open for that game (1..COZ_SAVE_SLOTS-1), or 0 if none.
static int next_copy_of(int ri)
{
	int used[COZ_SAVE_SLOTS] = {0};
	for (int i = 0; i < nrun; i++)
		if (run[i].game == run[ri].game && run[i].copy < COZ_SAVE_SLOTS)
			used[run[i].copy] = 1;
	for (int n = 1; n < COZ_SAVE_SLOTS; n++)
		if (!used[n])
			return n;
	return 0;
}

static int next_copy(void)
{
	return next_copy_of(cur);
}

static int link_rows(lrow_t *r)
{
	int n = 0;
	if (M2.page == 3) { // another copy: which save it starts from
		r[n++] = (lrow_t){L_COPY_OWN, 0, "ITS OWN SAVE"};
		r[n++] = (lrow_t){L_COPY_MINE, 0, "A COPY OF MY SAVE"};
		return n;
	}
	for (int i = 0; i < nrun && n < 12; i++)
		if (i != cur && is_emu(run[i].game) && (!run[i].eg || emu_linked(run[i].eg) == 1) &&
		    emu_linked(run[cur].eg) < 4) {
			r[n] = (lrow_t){L_WITH, i, ""};
			snprintf(r[n].label, sizeof r[n].label, "%s", inst_title(i));
			n++;
		}
	if (emu_linked(run[cur].eg) > 1)
		r[n++] = (lrow_t){L_UNLINK, 0, "UNPLUG CABLE"};
	return n;
}

// Open a linked copy of the current game. from_mine: its save starts as a
// copy of the current one (replacing the copy's own save).
// Open another copy of open game orig, linked to it. from_mine: the copy's
// save starts as a copy of the game's own save (replacing the copy's).
static void open_copy_of(int orig, int from_mine)
{
	int g = run[orig].game, n = next_copy_of(orig);
	if (!n || nrun >= MAXRUN || emu_linked(run[orig].eg) >= 4 || wake(orig)) {
		toast("TOO MANY COPIES OPEN");
		sfx_play(SFX_BUMP, 0);
		return;
	}
	char path[600];
	copy_sav_path(g, n, path, sizeof path);
	if (from_mine)
		unlink(path);
	emu_game *eg = emu_open(lib[g].rom, from_mine ? lib[g].sav_out : NULL, path);
	if (!eg) {
		toast("COULD NOT OPEN A COPY");
		sfx_play(SFX_BUMP, 0);
		return;
	}
	int i = nrun++;
	memset(&run[i], 0, sizeof run[i]);
	run[i].game = g, run[i].copy = n, run[i].eg = eg, run[i].boot = 120, run[i].fade = 0;
	emu_link(run[orig].eg, eg);
	G.cc = 0;
	enter_game(i, W / 2, H / 2);
	toast("DOUBLE-TAP MENU TO SWITCH COPIES");
}

static void open_linked_copy(int from_mine)
{
	open_copy_of(cur, from_mine);
}

static void link_update(unsigned p, float dt)
{
	lrow_t r[16];
	int n = link_rows(r);
	if (M2.lsel >= n)
		M2.lsel = n ? n - 1 : 0;
	int a_down = (ui_held() & BTN_A) != 0;
	if (!a_down)
		M2.larmed = 1;
	if (p & BTN_B) {
		M2.page = 0;
		ui_page_anim(1);
		sfx_play(SFX_BACK, 0);
		return;
	}
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (d && n) {
		if (M2.lsel + d >= 0 && M2.lsel + d < n)
			M2.lsel += d, M2.lhold = 0, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (!n)
		return;
	lrow_t *row = &r[M2.lsel];
	// replacing a copy's existing save is irreversible: hold A
	char path[600];
	copy_sav_path(run[cur].game, next_copy(), path, sizeof path);
	int needs_hold = row->kind == L_COPY_MINE && access(path, F_OK) == 0;
	if (needs_hold) {
		M2.lhold = M2.larmed && a_down ? M2.lhold + dt : 0;
		if (M2.lhold < 0.9f)
			return;
		M2.lhold = 0;
	} else if (!(p & BTN_A)) {
		return;
	}
	switch (row->kind) {
	case L_COPY_OWN: open_linked_copy(0); break;
	case L_COPY_MINE: open_linked_copy(1); break;
	case L_WITH:
		if (wake(row->target) == 0 && emu_link(run[cur].eg, run[row->target].eg) == 0) {
			snprintf(toast_buf, sizeof toast_buf, "LINKED WITH %s", inst_title(row->target));
			sfx_play(SFX_CONFIRM, 0);
			toast(toast_buf);
		}
		break;
	case L_UNLINK:
		emu_unlink(run[cur].eg);
		sfx_play(SFX_TOGGLE_OFF, 0);
		toast("UNLINKED");
		break;
	}
}

static void link_draw(canvas *c)
{
	uint32_t col = PAL[APP_GAMES];
	int copy_page = M2.page == 3, y = UI_TOP;
	ingame_header(c, copy_page ? "ANOTHER COPY" : "LINK CABLE", inst_title(cur), GREY);
	ui_column(0, 0);
	if (copy_page) { // what a copy is
		text(c, UI_MX + 2, y, "A 2ND COPY OF THIS GAME, LINKED", UI_TS, WHITE);
		text(c, UI_MX + 2, y + 28, "TO IT: TRADE OR PLAY 2-PLAYER.", UI_TS, WHITE);
		text(c, UI_MX + 2, y + 58, "DOUBLE-TAP MENU TO SWITCH BETWEEN THEM.", 2, GREY);
		y += 82;
	} else { // status
		int linked = emu_linked(run[cur].eg) > 1;
		fill(c, UI_MX + 4, y + 6, 10, 10, linked ? PAL[4] : GREY);
		text(c, UI_MX + 24, y + 2, linked ? "CONNECTED TO" : "NOT CONNECTED", UI_TS, linked ? WHITE : GREY);
		y += 30;
		for (int i = 0; i < nrun && linked; i++)
			if (linked_with_cur(i)) {
				const char *t = inst_title(i);
				int ts = text_width(t, UI_TS) <= W - 2 * UI_MX - 24 ? UI_TS : 2;
				text(c, UI_MX + 24, y + 2, t, ts, PAL[4]);
				y += 28;
			}
		y += 6;
	}

	lrow_t r[16];
	int n = link_rows(r);
	char path[600];
	copy_sav_path(run[cur].game, next_copy(), path, sizeof path);
	int shown_with = 0;
	if (copy_page)
		y = ui_section(c, y, "START WITH");
	if (!n)
		text(c, UI_MX + 2, y + 12, "OPEN ANOTHER GAME TO CONNECT IT", 2, GREY);
	for (int i = 0; i < n; i++) {
		int sel = i == M2.lsel;
		if (r[i].kind == L_WITH && !shown_with)
			y = ui_section(c, y, "CONNECT TO"), shown_with = 1;
		if (r[i].kind == L_UNLINK && shown_with)
			y += 8;
		if (r[i].kind == L_COPY_MINE && access(path, F_OK) == 0)
			ui_row_hold(c, y, r[i].label, sel, col, M2.lhold / 0.9f);
		else
			ui_row(c, y, r[i].label, sel, r[i].kind == L_UNLINK ? PAL[0] : col);
		y += UI_ROW;
	}
	static const char *const b[] = {"^V", "A", "B"}, *const l[] = {"MOVE", "OK", "BACK"};
	ingame_hints(c, b, l, 3);
}

static void ingame_update(unsigned p, float dt)
{
	if (SV.on) {
		states_update(p, dt);
		return;
	}
	if (M2.page >= 2) {
		link_update(p, dt);
		return;
	}
	if (M2.page) {
		settings_update(p, dt);
		return;
	}
	int sv[MAXRUN + 2], ns = switch_items(sv);
	int counts[2] = {NACT, ns};
	for (int k = 0; k < 2; k++) {
		if (M2.sel[k] >= counts[k])
			M2.sel[k] = counts[k] - 1;
		M2.sel_y[k] += (M2.sel[k] - M2.sel_y[k]) * (1 - expf(-dt * 22));
	}
	if (p & BTN_B) {
		G.cc = 0;
		sfx_play(SFX_TOGGLE_OFF, 0);
		return;
	}
	if ((p & BTN_RIGHT) && M2.col == 0)
		M2.col = 1, sfx_play(SFX_TICK, 0);
	if ((p & BTN_LEFT) && M2.col == 1)
		M2.col = 0, sfx_play(SFX_TICK, 0);
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	int *sel = &M2.sel[M2.col];
	if (d) {
		if (*sel + d >= 0 && *sel + d < counts[M2.col])
			*sel += d, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (!(p & BTN_A))
		return;
	if (M2.col == 1) {
		activate_card(sv[*sel], 470, 120 + *sel * 90);
		return;
	}
	switch (*sel) {
	case ACT_STATES: states_open(); break;
	case ACT_SAVES:
		if (!lib[run[cur].game].cg) {
			toast("NOT AVAILABLE FOR THIS GAME");
			sfx_play(SFX_BUMP, 0);
			break;
		}
		pick_open(run[cur].game);
		G.pick_ingame = 1;
		G.pick_sel = 0;
		for (int i = 0; i < G.pick_n; i++)	// start on the save being played
			if (G.pick_saves[i].slot == run[cur].copy + 1)
				G.pick_sel = i;
		break;
	case ACT_SETTINGS:
		M2.page = 1, M2.ssel = 1, M2.ssel_y = 1;
		ui_page_anim(0);
		sfx_play(SFX_OPEN, APP_SETTINGS);
		break;
	case ACT_COPY:
	case ACT_LINK:
		if (!run[cur].eg) {
			toast("NOT AVAILABLE FOR THIS GAME");
			sfx_play(SFX_BUMP, 0);
			break;
		}
		if (*sel == ACT_COPY && (nrun >= MAXRUN || !next_copy() || emu_linked(run[cur].eg) >= 4)) {
			toast("TOO MANY COPIES OPEN");
			sfx_play(SFX_BUMP, 0);
			break;
		}
		M2.page = *sel == ACT_COPY ? 3 : 2, M2.lsel = 0, M2.lhold = 0, M2.larmed = 0;
		ui_page_anim(0);
		sfx_play(SFX_OPEN, APP_GAMES);
		break;
	case ACT_RESET: quick_action(QA_RESET); break;
	case ACT_CLOSE: quick_action(QA_CLOSE); break;
	}
}

static void ingame_hints(canvas *c, const char *const *b, const char *const *l, int n)
{
	static const uint32_t cols[4] = {0xdddddd, 0xdddddd, 0x5ee05e, 0xff4a4a};
	int hx = 0;
	for (int k = 0; k < n; k++)
		draw_hint(NULL, &hx, b[k], l[k], 0);
	hx = (W - (hx - 22)) / 2;
	for (int k = 0; k < n; k++)
		draw_hint(c, &hx, b[k], l[k], cols[4 - n + k]);
}

static void settings_draw(canvas *c)
{
	uint32_t col = PAL[APP_GAMES];
	ingame_header(c, "SETTINGS", inst_title(cur), GREY);
	// sections and rows; scrolls so the selected row stays in view
	int ys[32], y = 0;
	for (int i = 0; i < nents && i < 32; i++) {
		ys[i] = y;
		y += ents[i].kind == E_HEADER ? 30 : UI_ROW;
	}
	const int top = UI_TOP, bot = H - 44;
	static float scroll;
	int sy = ys[M2.ssel], sh = UI_ROW;
	if (M2.ssel > 0 && ents[M2.ssel - 1].kind == E_HEADER)
		sy -= 30, sh += 30; // keep a row's section label with it
	if (sy - scroll < 0)
		scroll = (float)sy;
	if (sy + sh - scroll > bot - top)
		scroll = (float)(sy + sh - (bot - top));
	for (int i = 0; i < nents && i < 32; i++) {
		entry_t *e = &ents[i];
		int ry = top + ys[i] - (int)scroll;
		if (ry < top - 4 || ry + (e->kind == E_HEADER ? 30 : UI_ROW) > bot + 4)
			continue;
		if (e->kind == E_HEADER) {
			ui_section(c, ry, e->label);
			continue;
		}
		int sel = i == M2.ssel;
		ui_row(c, ry, e->opt->label, sel, col);
		switch (e->opt->type) {
		case IT_CHOICE: ui_row_choice(c, ry, e->opt->choices[e->opt->val], sel, col); break;
		case IT_TOGGLE: ui_row_toggle(c, ry, e->opt->val, sel); break;
		case IT_SLIDER: ui_row_slider(c, ry, e->opt->val, e->opt->max, sel, col); break;
		}
	}
	if (y > bot - top) { // scroll indicator
		int lh = bot - top, bh = lh * lh / y, bt = top + (int)(lh * scroll / y);
		fill(c, W - 6, top, 2, lh, 0x2a2a2a);
		fill(c, W - 6, bt, 2, bh, GREY);
	}
	static const char *const b[] = {"^V", "<>", "B"}, *const l[] = {"MOVE", "CHANGE", "BACK"};
	ingame_hints(c, b, l, 3);
}

// Header of the in-game pages: a title as big as fits, and a status line.
static void ingame_header(canvas *c, const char *title, const char *status, uint32_t scol)
{
	int ts = 4;
	while (ts > 2 && text_width(title, ts) > W - 2 * UI_MX)
		ts--;
	char t[96];
	snprintf(t, sizeof t, "%s", title);
	int maxc = (W - 2 * UI_MX) / (6 * ts);
	if ((int)strlen(t) > maxc)
		snprintf(t, sizeof t, "%.*s..", maxc - 2, title);
	text_arcade(c, UI_MX, 14 + (4 - ts) * 4, t, ts, run[cur].game >= 0 ? lib[run[cur].game].col : WHITE);
	if (status) {
		fill(c, UI_MX + 2, 58, 8, 8, scol);
		text(c, UI_MX + 16, 55, status, 2, scol);
	}
}

static void ingame_draw(canvas *c, float a)
{
	if (SV.on) {
		states_draw(c);
		return;
	}
	darken(c, 0, 0, W, H, 0.85f * a); // the game stays visible: the menu belongs to it
	if (M2.page >= 2) {
		link_draw(c);
		return;
	}
	if (M2.page) {
		settings_draw(c);
		return;
	}
	uint32_t col = PAL[APP_GAMES];
	int slide = (int)((1 - a) * -30);
	inst_t *in = &run[cur];

	// header: the game, as big as fits, and its state
	ingame_header(c, inst_title(cur), emu_linked(in->eg) > 1 ? "PLAYING - LINKED" : "PLAYING", PAL[3]);

	// left: what to do with this game
	const int lx = UI_MX + slide, lw = 300, top = UI_TOP;
	ui_column(lx, lw);
	for (int i = 0; i < NACT; i++) {
		int y = top + i * UI_ROW, sel = M2.col == 0 && i == M2.sel[0];
		ui_row(c, y, ACT_NAMES[i], sel, col);
		if (i == ACT_STATES) {
			int sv2[NSLOTS];
			char nb[16];
			snprintf(nb, sizeof nb, "%d", states_order(sv2));
			ui_row_value(c, y, nb, 0, sel, col, GREY);
		} else if (i == ACT_SAVES) {
			char nb[24];
			if (run[cur].copy)
				snprintf(nb, sizeof nb, "SAVE %d", run[cur].copy + 1);
			else
				snprintf(nb, sizeof nb, "MAIN");
			ui_row_value(c, y, lib[in->game].cg ? nb : NULL, 1, sel, col, GREY);
		} else if (i == ACT_SETTINGS || i == ACT_COPY) {
			ui_row_value(c, y, NULL, 1, sel, col, GREY);
		} else if (i == ACT_LINK) {
			ui_row_value(c, y, emu_linked(in->eg) > 1 ? "ON" : NULL, 1, sel, col, PAL[4]);
		}
	}

	// right: other open games, then home / all games
	const int rx = lx + lw + 16, rw = W - UI_MX - rx - slide;
	int sv[MAXRUN + 2], ns = switch_items(sv), nother = ns - 2;
	ui_column(rx, rw);
	int ytop = top;
	if (!nother) {
		text(c, rx + 16, ytop + 14, "NO OTHER GAMES", 2, 0x666666);
		ytop += UI_ROW;
	}
	const int view = (H - 44 - ytop) / UI_ROW; // rows that fit
	static float rscroll;
	int ssel = M2.sel[1];
	if (ssel - rscroll > view - 1)
		rscroll = (float)(ssel - view + 1);
	if (ssel < rscroll)
		rscroll = (float)ssel;
	if (ns <= view)
		rscroll = 0;
	for (int i = (int)rscroll; i < ns && i < (int)rscroll + view; i++) {
		int y = ytop + (i - (int)rscroll) * UI_ROW, sel = M2.col == 1 && i == ssel;
		if (sv[i] >= 0) { // an open game: thumbnail, title, state
			uint32_t ink = ui_row(c, y, "", sel, col);
			int g = sv[i], tw = 60, th = 40, ty = y + (UI_ROW - 6 - th) / 2;
			fill(c, rx + 6, ty - 1, tw + 2, th + 2, is_linked(g) ? LINK_COL : PAL[3]);
			blit_game(c, run[g].frame, GW, GH, rx + 7, ty, tw, th, 0);
			char t[64];
			int maxc = (rw - tw - 30) / 12;
			const char *it = inst_title(g);
			if ((int)strlen(it) > maxc)
				snprintf(t, sizeof t, "%.*s..", maxc - 2, it);
			else
				snprintf(t, sizeof t, "%s", it);
			int lk = linked_with_cur(g);
			text(c, rx + tw + 20, y + 6, t, 2, sel ? ink : 0xdddddd);
			text(c, rx + tw + 20, y + 25, lk ? "LINKED" : "PAUSED", 2, sel ? ink : lk ? PAL[4] : PAL[3]);
		} else { // home / all games
			ui_row(c, y, sv[i] == -1 ? "HOME" : "ALL GAMES", sel, col);
			ui_row_value(c, y, NULL, 1, sel, col, GREY);
		}
	}
	if (ns > view) { // scroll indicator
		int lh = view * UI_ROW - 6, bh = lh * view / ns, bt = ytop + (int)(lh * rscroll / ns);
		fill(c, W - 6, ytop, 2, lh, 0x2a2a2a);
		fill(c, W - 6, bt, 2, bh, GREY);
	}
	ui_column(0, 0);
	if (G.pick)	// SAVES is open over the menu: its own hints
		return;
	static const char *const b[] = {"^V", "<>", "A", "B"}, *const l[] = {"MOVE", "SWITCH SIDE", "OK", "RESUME"};
	ingame_hints(c, b, l, 4);
}

static void ingame_reset(void)
{
	memset(&M2, 0, sizeof M2);
	SV.on = 0;
}

void games_cc_update(unsigned p, float dt)
{
	G.from_menu = 1;
	if (cc_has_actions()) { // opened from a game: the vertical in-game menu
		G.cc_t += dt;
		ingame_update(p, dt);
		return;
	}
	G.cc_t += dt;
	int v[MAXRUN + 2], n = cc_cards(v);
	int na = cc_has_actions() ? NQA : 0;
	if (G.cc_card >= n)
		G.cc_card = n - 1;
	G.cc_cam += (G.cc_card - G.cc_cam) * (1 - expf(-dt * 14));

	if (G.cc_sub) { // popup open: it gets the input
		list_t *l = G.cc_sub == QA_AV ? &av_list : &input_list;
		if (p & BTN_B) {
			G.cc_sub = 0;
			sfx_play(SFX_BACK, 0);
			return;
		}
		list_update(l, p, dt);
		G.screen_mode = av_items[AV_SCREEN].val;
		G.filter = av_items[AV_FILTER].val;
		if (av_items[AV_VOLUME].val != ui_volume())
			ui_set_volume(av_items[AV_VOLUME].val);
		static int color = -1;
		if (color != av_items[AV_COLOR].val) {
			color = av_items[AV_COLOR].val;
			emu_set_color_fix(color);
		}
		return;
	}
	if (p & BTN_B) {
		G.cc = 0;
		sfx_play(SFX_TOGGLE_OFF, 0);
		return;
	}

	if (G.cc_style == 1) { // side panel: one vertical list of cards then actions
		int total = n + na;
		if (G.cc_item >= total)
			G.cc_item = total - 1;
		if ((p & BTN_DOWN) && G.cc_item < total - 1)
			G.cc_item++, sfx_play(SFX_TICK, 0);
		if ((p & BTN_UP) && G.cc_item > 0)
			G.cc_item--, sfx_play(SFX_TICK, 0);
		if (p & BTN_A) {
			if (G.cc_item < n)
				activate_card(v[G.cc_item], W - 130, 100 + G.cc_item * 50);
			else
				quick_action(G.cc_item - n);
		}
		return;
	}

	int rows = na ? 2 : 1;
	if ((p & BTN_DOWN) && G.cc_row < rows - 1)
		G.cc_row++, sfx_play(SFX_TICK, 0);
	if ((p & BTN_UP) && G.cc_row > 0)
		G.cc_row--, sfx_play(SFX_TICK, 0);
	int d = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
	if (G.cc_row == 0) {
		if (d && G.cc_card + d >= 0 && G.cc_card + d < n)
			G.cc_card += d, sfx_play(SFX_MOVE, G.cc_card);
		else if (d)
			sfx_play(SFX_BUMP, 0);
	} else if (d && G.cc_act + d >= 0 && G.cc_act + d < NQA) {
		G.cc_act += d, sfx_play(SFX_TICK, 0);
	}
	if (!(p & BTN_A))
		return;
	if (G.cc_row == 1)
		quick_action(G.cc_act);
	else
		activate_card(v[G.cc_card], W / 2, 300);
}

static void cc_action_label(int a, char *buf, size_t n, int longform)
{
	static const char *const names[NQA] = {"SNAP", "LOAD", "", "A/V", "INPUT", "RESET", "CLOSE"};
	static const char *const longnames[NQA] = {"TAKE SNAPSHOT", "LOAD SNAPSHOT", "", "AUDIO & VIDEO",
						   "CONTROLS", "RESET GAME", "CLOSE GAME"};
	if (a == QA_SLOT)
		snprintf(buf, n, longform ? "SNAPSHOT SLOT  %d" : "SLOT%d", G.slot + 1);
	else
		snprintf(buf, n, "%s", longform ? longnames[a] : names[a]);
}

// One card: HOME, ALL GAMES or a running game's live frame.
static const char *draw_cc_card(canvas *c, int sel_id, int x, int y, int cw, int chh)
{
	if (sel_id == -1) {
		fill(c, x, y, cw, chh, 0x222222);
		int s = cw >= 200 ? 3 : cw >= 110 ? 2 : 1;
		int tw = 8 * 6 * s - s;
		for (int k = 0; k < 8; k++) {
			char s1[2] = {"COZISBOY"[k], 0};
			text(c, x + (cw - tw) / 2 + k * 6 * s, y + chh / 2 - 7 * s / 2, s1, s, PAL[k]);
		}
		return "HOME";
	}
	if (sel_id == -2) {
		fill(c, x, y, cw, chh, 0x222222);
		int ts = chh >= 120 ? 80 : chh >= 70 ? chh - 28 : chh - 10;
		draw_tile(c, APP_GAMES, x + cw / 2 - ts / 2, y + (chh - ts) / 2, ts,
			  ts / 16 > 1 ? ts / 16 : 1, 0);
		return "ALL GAMES";
	}
	inst_t *in = &run[sel_id];
	blit_game(c, in->frame, GW, GH, x, y, cw, chh, 0);
	if (sel_id == cur) {
		rrect(c, x + 4, y + 4, 40, 14, 2, PAL[3]);
		text(c, x + 8, y + 7, "NOW", 1, BLACK);
	}
	if (is_linked(sel_id))
		link_chip(c, x + 4, y + chh - 16, 1);
	return inst_title(sel_id);
}

static void action_chips(canvas *c, int y)
{
	const int aw = 78, asp = 84;
	int ax = (W - (NQA * asp - (asp - aw))) / 2;
	for (int i = 0; i < NQA; i++) {
		char buf[24];
		cc_action_label(i, buf, sizeof buf, 0);
		int sel = G.cc_row == 1 && G.cc_act == i;
		int on = 0;
		int x = ax + i * asp;
		uint32_t f = sel ? PAL[APP_GAMES] : on ? PAL[3] : 0x2a2a2a;
		int st = sel ? W_SEL : W_NORMAL;
		wbox(c, x, y, aw, 34, f, st);
		int tw = text_width(buf, 2) <= aw - 6 ? 2 : 1;
		text(c, x + (aw - text_width(buf, tw)) / 2,
		     y + (34 - 7 * tw) / 2 - (wstyle == 2 && sel ? 3 : 0), buf, tw, wink(f, st));
	}
}

// Variation 0: PS5 bottom bar
static void cc_draw_bottom(canvas *c, float a)
{
	int acts = cc_has_actions();
	int ph = acts ? 236 : 170;
	int py = H - (int)(ph * (1 - (1 - a) * (1 - a)));
	darken(c, 0, 0, W, py, 0.45f * a);
	fill(c, 0, py, W, H - py, 0x101010);
	for (int i = 0; i < 8; i++)
		fill(c, i * W / 8, py, W / 8 + 1, 3, PAL[i]);

	int v[MAXRUN + 2], n = cc_cards(v);
	const int cw = 120, chh = 80, sp = 132;
	int x0 = (W - (n * sp - (sp - cw))) / 2, y0 = py + 22;
	for (int i = 0; i < n; i++) {
		int x = x0 + i * sp, y = y0;
		int sel = G.cc_row == 0 && G.cc_card == i;
		if (sel)
			y -= 6, wfocus(c, x, y, cw, chh, 7);
		wframe(c, x, y, cw, chh, 0x333333);
		const char *label = draw_cc_card(c, v[i], x, y, cw, chh);
		if (!sel)
			darken(c, x, y, cw, chh, 0.35f);
		else
			text(c, x + (cw - text_width(label, 2)) / 2, y + chh + 12, label, 2, WHITE);
	}
	if (acts)
		action_chips(c, py + 148);
	const char *hint = "A SELECT   B CLOSE   HOLD MENU: HOME   DOUBLE-TAP: LAST GAME";
	text(c, (W - text_width(hint, 1)) / 2, H - 14, hint, 1, 0x888888);
}

// Variation 1: side panel (Steam Deck quick menu)
static void cc_draw_side(canvas *c, float a)
{
	const int pw = 280;
	int px = W - (int)(pw * (1 - (1 - a) * (1 - a)));
	darken(c, 0, 0, px, H, 0.5f * a);
	fill(c, px, 0, W - px, H, 0x101010);
	for (int i = 0; i < 8; i++)
		fill(c, px, i * H / 8, 3, H / 8 + 1, PAL[i]);

	int v[MAXRUN + 2], n = cc_cards(v);
	int na = cc_has_actions() ? NQA : 0;
	int total = n + na;
	// scroll so the selection stays visible (rows of 46 / 30 px)
	int y = 16;
	text_arcade(c, px + 18, y, "QUICK MENU", 3, PAL[APP_GAMES]);
	y += 40;
	int first = G.cc_item > 7 ? G.cc_item - 7 : 0;
	for (int i = first; i < total && y < H - 30; i++) {
		int sel = G.cc_item == i;
		if (i == n && na) { // section header
			fill(c, px + 18, y + 2, pw - 36, 2, 0x333333);
			y += 10;
		}
		if (i < n) {
			int rh = 46;
			if (sel)
				rrect(c, px + 10, y - 3, pw - 20, rh, 5, PAL[APP_GAMES]);
			rrect(c, px + 16, y + 1, 64, 38, 3, BLACK);
			const char *label = draw_cc_card(c, v[i], px + 18, y + 3, 60, 34);
			text(c, px + 92, y + 13, label, 2, sel ? BLACK : WHITE);
			y += rh + 2;
		} else {
			char buf[32];
			cc_action_label(i - n, buf, sizeof buf, 1);
			int rh = 30;
			if (sel)
				rrect(c, px + 10, y - 3, pw - 20, rh, 5, PAL[APP_GAMES]);
			text(c, px + 22, y + 5, buf, 2, sel ? BLACK : 0xdddddd);
			y += rh + 2;
		}
	}
	text(c, px + 18, H - 14, "A SELECT  B CLOSE  HOLD MENU: HOME", 1, 0x888888);
}

// Variation 2: full-screen card fan
static void cc_draw_fan(canvas *c, float a)
{
	darken(c, 0, 0, W, H, 0.85f * a);
	text_arcade(c, 24, 24, "SWITCH", 4, PAL[APP_GAMES]);
	int v[MAXRUN + 2], n = cc_cards(v);
	const int cw = GW, chh = GH, sp = 270;
	int lift = (int)((1 - a) * 40);
	for (int pass = 0; pass < 2; pass++)
		for (int i = 0; i < n; i++) {
			int sel = G.cc_row == 0 && G.cc_card == i;
			if ((G.cc_card == i) != pass)
				continue;
			int x = W / 2 + (int)lrintf((i - G.cc_cam) * sp) - cw / 2;
			if (x < -cw - 20 || x > W + 20)
				continue;
			int y = 110 + lift - (G.cc_card == i ? 8 : 0);
			if (sel)
				wfocus(c, x, y, cw, chh, 9);
			wframe(c, x, y, cw, chh, 0x333333);
			const char *label = draw_cc_card(c, v[i], x, y, cw, chh);
			if (G.cc_card != i)
				darken(c, x, y, cw, chh, 0.55f);
			else
				text_arcade(c, x + (cw - text_width(label, 3)) / 2, y + chh + 18, label, 3,
					    v[i] >= 0 ? lib[run[v[i]].game].col : WHITE);
		}
	if (cc_has_actions())
		action_chips(c, 384);
	const char *hint = "A SELECT   B CLOSE   HOLD MENU: HOME   DOUBLE-TAP: LAST GAME";
	text(c, (W - text_width(hint, 1)) / 2, H - 14, hint, 1, 0x888888);
}

static void cc_draw_popup(canvas *c)
{
	list_t *l = G.cc_sub == QA_AV ? &av_list : &input_list;
	const char *title = G.cc_sub == QA_AV ? "AUDIO & VIDEO" : "CONTROLS";
	uint32_t col = PAL[APP_GAMES];
	const int pw = 440, rh = 40, ph = 56 + l->n * rh + 12;
	int px = (W - pw) / 2, py = (H - ph) / 2 - 20;
	darken(c, 0, 0, W, H, 0.4f);
	rrect(c, px - 3, py - 3, pw + 6, ph + 6, 8, BLACK);
	rrect(c, px, py, pw, ph, 6, 0x161616);
	text_arcade(c, px + 20, py + 14, title, 3, col);
	list_draw(c, l, px + 14, py + 56, pw - 28, rh, col);
}

void games_cc_draw(canvas *c)
{
	if (!G.cc)
		return;
	float a = clampf(G.cc_t / 0.15f, 0, 1);
	if (cc_has_actions()) {
		ingame_draw(c, a);
		if (G.pick)
			pick_draw(c);	// a full page with its own hints
		return;
	}
	if (G.cc_style == 1)
		cc_draw_side(c, a);
	else if (G.cc_style == 2)
		cc_draw_fan(c, a);
	else
		cc_draw_bottom(c, a);
	if (G.cc_sub)
		cc_draw_popup(c);
}

/* ---- entry points ---------------------------------------------------------- */

#include <sys/stat.h>
static void mkdir_p(const char *dir)
{
	mkdir(dir, 0755);
}

static int by_title(const void *a, const void *b)
{
	return strcmp(((const game_t *)a)->title, ((const game_t *)b)->title);
}

// Fill lib[] with the GBA games of the game library ($COZ_LIBRARY, else
// /data/library). Each entry keeps its own copy of the library record (cgv),
// so a rescan (games_poll) can add, update and hide games while indices into
// lib[] (open games, snapshots, caches) stay valid.

static void add_game(const coz_game *cg)
{
	char rom[1024], sav[1024];
	if (nlib >= MAXLIB || cg->sys != COZ_SYS_GBA) // GB/GBC: not yet (the screen code assumes 240x160)
		return;
	if (coz_game_rom_path(cg, rom, sizeof rom) || coz_game_save_path(cg, sav, sizeof sav))
		return;
	game_t *gm = &lib[nlib++];
	memset(gm, 0, sizeof *gm);
	gm->cgv = *cg;
	gm->cg = &gm->cgv;
	char *title = strdup(cg->title);
	for (char *q = title; *q; q++)
		*q = (char)toupper((unsigned char)*q);
	gm->title = title;
	gm->genre = "GBA";
	unsigned h = 0;
	for (const char *q = title; *q; q++)
		h = h * 31 + (unsigned char)*q;
	gm->col = PAL[h % 8];
	gm->last = "NEVER";
	gm->rom = strdup(rom);
	gm->sav_out = strdup(sav);
	gm->thumb = strdup(cg->name); // also the play-history key
	// copies' saves used to live in <saves>/copies/<id>-copyN.sav: move
	// them into the library as save N (unless that save exists already)
	const char *sd = getenv("COZ_SAVES") ? getenv("COZ_SAVES") : "saves";
	for (int k = 2; k <= COZ_SAVE_SLOTS; k++) {
		char old[600], to[600], bak[600];
		snprintf(old, sizeof old, "%s/copies/%s-copy%d.sav", sd, cg->id, k);
		if (access(old, F_OK) || coz_save_slot_path(gm->cg, k, to, sizeof to) ||
		    coz_save_slot_backup_path(gm->cg, k, bak, sizeof bak) || !access(to, F_OK) || !access(bak, F_OK))
			continue;
		if (rename(old, to) == 0)
			coz_game_refresh(&gm->cgv);
	}
}

static int scan_roms(void)
{
	coz_lib library;
	if (coz_lib_scan(&library))
		return 0;
	for (int k = 0; k < library.n; k++)
		add_game(&library.games[k]);
	coz_lib_free(&library);
	qsort(lib, (size_t)nlib, sizeof *lib, by_title);
	for (int g = 0; g < nlib; g++) // cg points into the entry: fix after the move
		lib[g].cg = &lib[g].cgv;
	return nlib;
}

/* ---- live library: games added, changed or removed while running ---------- */

// Something changed in the library folder: the GBA folder's entries, or any
// game's folder (a new cover, save or snapshot). Cheap: one stat per game.
static unsigned long long library_signature(void)
{
	char d[1024];
	snprintf(d, sizeof d, "%s/%s", coz_lib_root(), coz_sys_key(COZ_SYS_GBA));
	struct stat st;
	if (stat(d, &st))
		return 0;
	unsigned long long sig = (unsigned long long)st.st_mtim.tv_sec * 1000003ULL + (unsigned long long)st.st_mtim.tv_nsec;
	DIR *dir = opendir(d);
	struct dirent *e;
	while (dir && (e = readdir(dir))) {
		if (e->d_name[0] == '.')
			continue;
		char p[1300];
		snprintf(p, sizeof p, "%s/%s", d, e->d_name);
		if (stat(p, &st) == 0)
			sig = sig * 31 + (unsigned long long)st.st_mtim.tv_sec * 1000003ULL + (unsigned long long)st.st_mtim.tv_nsec;
	}
	if (dir)
		closedir(dir);
	return sig;
}

static void library_sync(void)
{
	coz_lib fresh;
	if (coz_lib_scan(&fresh))
		return;
	int mock = nlib && !lib[0].rom;
	if (mock) {
		if (nrun || !fresh.n) { // keep the demo games until real ones can replace them
			coz_lib_free(&fresh);
			return;
		}
		nlib = 0;
		memset(cover_tried, 0, sizeof cover_tried), memset(screen_tried, 0, sizeof screen_tried);
		memset(has_cover, 0, sizeof has_cover);
	}
	// update or hide what we know
	for (int g = 0; g < nlib; g++) {
		if (!lib[g].cg)
			continue;
		const coz_game *f = coz_lib_find(&fresh, lib[g].cgv.id);
		if (!f) {
			if (find_running(g) < 0)	// an open game stays until closed
				lib[g].gone = 1;
			continue;
		}
		if (f->cover_time != lib[g].cgv.cover_time || f->has_cover != lib[g].cgv.has_cover)
			cover_tried[g] = 0, has_cover[g] = 0;	// reload the new box art when drawn
		lib[g].cgv = *f;
		lib[g].gone = 0;
	}
	// add the new ones
	int added = 0;
	for (int k = 0; k < fresh.n; k++) {
		int known = 0;
		for (int g = 0; g < nlib && !known; g++)
			known = lib[g].cg && !strcmp(lib[g].cgv.id, fresh.games[k].id);
		if (!known) {
			int before = nlib;
			add_game(&fresh.games[k]);
			added += nlib - before;
		}
	}
	coz_lib_free(&fresh);
	if (mock)
		history_load();
	if (added) {
		snprintf(toast_buf, sizeof toast_buf, added == 1 ? "1 NEW GAME" : "%d NEW GAMES", added);
		toast(toast_buf);
	}
}

// Called every frame: looks at the library every 2 s (not while a game runs on
// screen: a rescan reads every game's folder).
void games_poll(int screen, float dt)
{
	static float t;
	static unsigned long long sig;
	if (!emu_available() || screen == SCR_GAME || (t += dt) < 2.0f)
		return;
	t = 0;
	unsigned long long s2 = library_signature();
	if (!sig) {
		sig = s2;	// the first look: what games_init loaded
		return;
	}
	if (s2 != sig) {
		sig = s2;
		library_sync();
	}
}

void games_init(void)
{
	memset(&G, 0, sizeof G);
	G.screen_mode = av_items[AV_SCREEN].val; // FULL: fill the width
	nlib = 0;
	if (emu_init() == 0) {
		mkdir_p(getenv("COZ_SAVES") ? getenv("COZ_SAVES") : "saves");
		scan_roms();
		history_load();
		for (int g = 0; g < nlib; g++)
			if (lib[g].cg && lib[g].cg->nstates)
				states_load_disk(g);
	}
	if (!nlib) { // no core or no ROMs: built-in mock games
		nlib = (int)(sizeof mock_lib / sizeof *mock_lib);
		memcpy(lib, mock_lib, sizeof mock_lib);
		recent_counter = 10;
	}
	for (int g = 0; g < NGAMES; g++) {
		if (!lib[g].rom) {
			gstate_t s;
			game_reset(&s);
			s.px = 120 + g * 47;
			s.scroll = s.px - 90;
			s.t = g * 1.7f;
			game_render(&s, g, art[g], 0);
			lib[g].has_art = 1;
		}
		if (lib[g].img) { // optional real art (see mock/.gitignore)
			const char *dir = getenv("COZ_MOCK") ? getenv("COZ_MOCK") : "mock";
			char path[256];
			snprintf(path, sizeof path, "%s/%s-art.ppm", dir, lib[g].img);
			if (load_ppm(path, GW, GH, art[g]))
				lib[g].has_art = 1;
			snprintf(path, sizeof path, "%s/%s-cover.ppm", dir, lib[g].img);
			has_cover[g] = load_ppm(path, COVER_N, COVER_N, cover_img[g]);
		}
	}
}

int games_running(void)
{
	return nrun;
}

void games_update(int screen, unsigned p, unsigned held, float dt)
{
	if (screen == SCR_LIBRARY)
		hub_update(p, dt);
	else if (screen == SCR_GAME)
		game_update(p, held, dt);
}

void games_draw(canvas *c, int screen)
{
	if (screen == SCR_LIBRARY) {
		hub_draw(c);
	} else if (screen == SCR_GAME) {
		if (cur < 0) {
			fill(c, 0, 0, W, H, BLACK);
			return;
		}
		draw_game_view(c, &run[cur]);
		if (run[cur].fade < 1) // a game that just started fades in
			darken(c, 0, 0, W, H, 1 - clampf(run[cur].fade, 0, 1));
		if (is_linked(cur) && !G.cc) // in the black border, never over the game
		{
			// LINKED, plus which copy this is (#1 = the original) in yellow
			int x = W - 8 - text_width("LINKED", 2) - 12;
			chip(c, x, 6, 2, "LINKED", LINK_COL);
			if (has_twin(cur)) {
				char nb[16];
				snprintf(nb, sizeof nb, "#%d", run[cur].copy + 1);
				chip(c, x - 6 - text_width(nb, 2) - 12, 6, 2, nb, PAL[2]);
			}
		}

	}
}

void games_hints(canvas *c, int screen, int *x)
{
	if (screen != SCR_LIBRARY)
		return;
	int ri = hub_sel_run();
	draw_hint(c, x, "A", ri >= 0 || find_running_copy(G.game, 0) >= 0 ? "RESUME" : "START", PAL[3]);
	draw_hint(c, x, "Y", "FAV", PAL[2]);
	if (ri >= 0)
		draw_hint(c, x, "X", "CLOSE", PAL[4]);
	else if (lib[G.game].cg)
		draw_hint(c, x, "X", "SAVES", PAL[4]);
	draw_hint(c, x, "B", "BACK", PAL[0]);
}

/* ---- home carousel tiles --------------------------------------------------- */

static int last_played(void)
{
	int best = -1;
	for (int g = 0; g < NGAMES; g++)
		if (!lib[g].gone && lib[g].recent && (best < 0 || lib[g].recent > lib[best].recent))
			best = g;
	return best;
}

int games_home_refs(int *refs)
{
	if (nrun)
		return running_order(refs);
	int g = last_played();
	if (g < 0)
		return 0;
	refs[0] = -(g + 1);
	return 1;
}

static int ref_game(int ref)
{
	return ref >= 0 ? run[ref].game : -ref - 1;
}

static void ring(canvas *c, int x, int y, int w, int h, int t, uint32_t col);

void games_ref_tile(canvas *c, int ref, int x, int y, int size, int dim)
{
	// same thin border as the hub covers: green when open, grey otherwise
	ring(c, x - 2, y - 2, size + 4, size + 4, 2,
	     ref < 0 ? 0x8a8a8a : is_linked(ref) ? LINK_COL : PAL[3]);
	skip_frame = 1;
	if (ref >= 0) {
		draw_open_cover(c, ref, x, y, size);
	} else {
		int g = ref_game(ref);
		const uint32_t *pic = game_picture(g);
		if (pic)
			draw_frame_tile(c, pic, x, y, size, WHITE, "LAST", 0);
		else
			draw_cover(c, g, x, y, size);
	}
	skip_frame = 0;
	if (dim)
		darken(c, x - 2, y - 2, size + 4, size + 4, 0.45f);
}

// "OPEN - PAUSED" (green) / "OPEN - LINKED" (cyan) pill for open game ri,
// centred on cx, top at y.
void games_open_pill(canvas *c, int cx, int y, int ri)
{
	int lk = is_linked(ri);
	const char *st = lk ? "OPEN - LINKED" : "OPEN - PAUSED";
	uint32_t sc = lk ? LINK_COL : PAL[3];
	int w = text_width(st, 2) + 34, x = cx - w / 2;
	rrect(c, x, y, w, 26, 4, mix(sc, BLACK, 0.6f));
	fill(c, x + 10, y + 9, 8, 8, sc);
	text(c, x + 24, y + 6, st, 2, sc);
}

const char *games_ref_title(int ref)
{
	return ref >= 0 ? inst_title(ref) : lib[ref_game(ref)].title;
}

uint32_t games_ref_col(int ref)
{
	return lib[ref_game(ref)].col;
}

void games_ref_status(int ref, char *buf, int n)
{
	int g = ref_game(ref);
	if (ref >= 0)
		snprintf(buf, (size_t)n, emu_linked(run[ref].eg) > 1 ? "OPEN - LINKED" : "OPEN - PAUSED");
	else
		snprintf(buf, (size_t)n, "LAST PLAYED %s", last_text(g));
}

void games_ref_open(int ref, int x, int y)
{
	if (ref >= 0)
		enter_game(ref, x, y);
	else
		open_game(ref_game(ref), -1, x, y);
}

void games_ref_close(int ref)
{
	int g = run[ref].game;
	close_running(ref);
	snprintf(toast_buf, sizeof toast_buf, "%s CLOSED", lib[g].title);
	sfx_play(SFX_TOGGLE_OFF, 0);
	toast(toast_buf);
}

void games_ref_draw_bg(canvas *c, int ref)
{
	int g = ref_game(ref);
	const uint32_t *f = ref >= 0 ? run[ref].frame : game_picture(g);
	if (f)
		blit_game(c, f, GW, GH, 0, 0, W, H, 0);
	else
		fill(c, 0, 0, W, H, mix(lib[g].col, BLACK, 0.6f));
	darken(c, 0, 0, W, H, 0.7f);
}

void games_prefs_apply(void)
{
	G.screen_mode = av_items[AV_SCREEN].val;
	G.filter = av_items[AV_FILTER].val;
	emu_set_color_fix(av_items[AV_COLOR].val);
}

int games_starting(void)
{
	return cur >= 0 && cur < nrun && run[cur].boot > 0;
}

// Rumble strength 0..100 for the motor: the game on screen, while it runs
// (not paused in a menu or the control center).
int games_rumble(int screen)
{
	if (screen != SCR_GAME || cur < 0 || G.cc || G.pick || !run[cur].eg)
		return 0;
	return emu_rumble(run[cur].eg);
}
