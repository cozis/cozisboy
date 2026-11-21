// cozisboy UI: home carousel, settings/power lists, shared widgets, transitions.
// Game screens live in games.c. Everything is drawn in software into a
// 640x480 XRGB8888 buffer.
#include "ui_priv.h"

#include "emu.h"
#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const uint32_t PAL[8] = {
	0xff4a4a, 0xff9a2e, 0xffd93d, 0x5ee05e, 0x3fc8ff, 0x6a7dff, 0xc56bff, 0xff6bc1,
};

/* ---- widget styles --------------------------------------------------------- */

int wstyle;

static int lum(uint32_t c)
{
	return (int)(((c >> 16) & 255) * 3 + ((c >> 8) & 255) * 6 + (c & 255)) / 10;
}

uint32_t wink(uint32_t f, int state)
{
	if (wstyle == 3)
		return state == W_SEL ? WHITE : mix(f, WHITE, 0.3f);
	return lum(f) > 110 ? BLACK : WHITE;
}

void wbox(canvas *c, int x, int y, int w, int h, uint32_t col, int state)
{
	int big = h >= 30;
	if (state == W_DIM)
		col = mix(col, BG0, 0.55f);
	switch (wstyle) {
	case 1: { // soft
		int r = h >= 60 ? 12 : big ? 8 : 5;
		if (state == W_SEL) {
			rrect(c, x - 6, y - 6, w + 12, h + 12, r + 4, WHITE);
			rrect(c, x - 3, y - 3, w + 6, h + 6, r + 2, BG0);
		}
		rrect(c, x, y, w, h, r, col);
		break;
	}
	case 2: { // bevel
		int t = big ? 2 : 1, hi = big ? 4 : 2, lo = big ? 6 : 3;
		if (state == W_SEL)
			y -= 3;
		rrect(c, x + 3, y + h + t, w - 2, 3 + (state == W_SEL ? 3 : 0), 1, 0x050505); // drop
		rrect(c, x - t, y - t, w + 2 * t, h + 2 * t, 3, state == W_SEL ? WHITE : BLACK);
		fill(c, x, y, w, h, col);
		uint32_t light = mix(col, WHITE, 0.4f), dark = mix(col, BLACK, 0.45f);
		fill(c, x, y, w, hi, light), fill(c, x, y, hi, h, light);
		fill(c, x, y + h - lo, w, lo, dark), fill(c, x + w - lo, y, lo, h, dark);
		break;
	}
	case 3: { // neon
		int r = h >= 60 ? 6 : 4;
		uint32_t border = state == W_SEL ? WHITE : state == W_DIM ? col : mix(col, WHITE, 0.1f);
		if (state == W_SEL)
			rrect(c, x - 6, y - 6, w + 12, h + 12, r + 3, mix(col, BG0, 0.45f));
		rrect(c, x - 3, y - 3, w + 6, h + 6, r + 2, border);
		rrect(c, x, y, w, h, r, mix(col, BG0, 0.82f));
		break;
	}
	default: { // arcade
		int r = h >= 60 ? 6 : big ? 4 : 3, t = big ? 2 : 1;
		if (state == W_SEL)
			rrect(c, x - 3 * t - 2, y - 3 * t - 2, w + 6 * t + 4, h + 6 * t + 4, r + 3, WHITE);
		rrect(c, x - 2 * t, y - 2 * t, w + 4 * t, h + 4 * t, r + 2, BLACK);
		rrect(c, x - t, y - t, w + 2 * t, h + 2 * t, r + 1, state == W_DIM ? 0x6a6a6a : WHITE);
		rrect(c, x, y, w, h, r, col);
		if (h >= 24) {
			fill(c, x + r, y, w - 2 * r, 3, mix(col, WHITE, 0.3f));
			fill(c, x + r, y + h - 3, w - 2 * r, 3, mix(col, BLACK, 0.25f));
		}
	}
	}
}

void wframe(canvas *c, int x, int y, int w, int h, uint32_t accent)
{
	switch (wstyle) {
	case 1:
		rrect(c, x - 3, y - 3, w + 6, h + 6, 8, accent);
		break;
	case 2:
		rrect(c, x - 5, y - 5, w + 10, h + 10, 3, mix(accent, BLACK, 0.5f));
		fill(c, x - 5, y - 5, w + 8, 3, accent), fill(c, x - 5, y - 5, 3, h + 8, accent);
		rrect(c, x - 1, y - 1, w + 2, h + 2, 1, BLACK);
		break;
	case 3:
		rrect(c, x - 2, y - 2, w + 4, h + 4, 3, accent);
		break;
	default:
		rrect(c, x - 5, y - 5, w + 10, h + 10, 6, BLACK);
		rrect(c, x - 3, y - 3, w + 6, h + 6, 5, accent);
		rrect(c, x - 1, y - 1, w + 2, h + 2, 3, BLACK);
	}
}

void wfocus(canvas *c, int x, int y, int w, int h, int g)
{
	switch (wstyle) {
	case 1:
		rrect(c, x - g - 2, y - g - 2, w + 2 * g + 4, h + 2 * g + 4, g + 6, WHITE);
		rrect(c, x - g + 1, y - g + 1, w + 2 * g - 2, h + 2 * g - 2, g + 4, BG0);
		break;
	case 2:
		rrect(c, x - g, y - g, w + 2 * g, h + 2 * g, 2, WHITE);
		rrect(c, x - g + 3, y - g + 3, w + 2 * g - 6, h + 2 * g - 6, 1, BLACK);
		break;
	case 3:
		rrect(c, x - g, y - g, w + 2 * g, h + 2 * g, g, WHITE);
		rrect(c, x - g + 2, y - g + 2, w + 2 * g - 4, h + 2 * g - 4, g - 1, BG0);
		break;
	default:
		rrect(c, x - g, y - g, w + 2 * g, h + 2 * g, g, WHITE);
		rrect(c, x - g + 2, y - g + 2, w + 2 * g - 4, h + 2 * g - 4, g - 1, BLACK);
	}
}

/* ---- apps ------------------------------------------------------------- */

static const char *const APP_NAME[NAPPS] = {
	"BROWSER", "FILES", "TERMINAL", "MUSIC", "GAMES", "WI-FI", "SETTINGS", "POWER",
};
static const char *const APP_DESC[NAPPS] = {
	"SURF THE WEB", "BROWSE YOUR STUFF", "COMMAND LINE", "PLAY SOME TUNES",
	"PLAY YOUR GBA GAMES", "CONNECT TO A NETWORK", "TWEAK YOUR COZISBOY",
	"SLEEP OR SHUT DOWN",
};
static icon_t icons[NAPPS];

static const char *const ICON_GLOBE[ICON_N] = {
	".....######.....",
	"...####kk####...",
	"..##k##kk##k##..",
	".##k###kk###k##.",
	".kkkkkkkkkkkkkk.",
	"#k#####kk#####k#",
	"#k#####kk#####k#",
	"kkkkkkkkkkkkkkkk",
	"#k#####kk#####k#",
	"#k#####kk#####k#",
	".kkkkkkkkkkkkkk.",
	".##k###kk###k##.",
	"..##k##kk##k##..",
	"...####kk####...",
	".....######.....",
	NULL,
};
static const char *const ICON_FOLDER[ICON_N] = {
	"",
	"",
	".#####..........",
	"#######.........",
	"################",
	"################",
	"#kkkkkkkkkkkkkk#",
	"################",
	"################",
	"################",
	"################",
	"################",
	"################",
	"################",
	NULL,
};
static const char *const ICON_TERM[ICON_N] = {
	"",
	"################",
	"#k#k#k##########",
	"################",
	"#kkkkkkkkkkkkkk#",
	"#kk#kkkkkkkkkkk#",
	"#kkk#kkkkkkkkkk#",
	"#kkkk#kkkkkkkkk#",
	"#kkk#kkkkkkkkkk#",
	"#kk#kkk#####kkk#",
	"#kkkkkkkkkkkkkk#",
	"#kkkkkkkkkkkkkk#",
	"################",
	NULL,
};
static const char *const ICON_NOTE[ICON_N] = {
	"",
	".....###########",
	".....###########",
	".....##.......##",
	".....##.......##",
	".....##.......##",
	".....##.......##",
	".....##.......##",
	".....##....#####",
	"..#####...######",
	".######...######",
	".######....####.",
	"..####..........",
	NULL,
};
static const char *const ICON_PAD[ICON_N] = {
	"",
	"",
	"",
	"",
	"..############..",
	".##############.",
	"###k########k###",
	"##kkk######k#k##",
	"###k########k###",
	"################",
	"######....######",
	".####......####.",
	"..##........##..",
	NULL,
};

static void gen_icons(void)
{
	icon_from_rows(&icons[APP_BROWSER], ICON_GLOBE, ICON_N);
	icon_from_rows(&icons[APP_FILES], ICON_FOLDER, ICON_N);
	icon_from_rows(&icons[APP_TERM], ICON_TERM, ICON_N);
	icon_from_rows(&icons[APP_MUSIC], ICON_NOTE, ICON_N);
	icon_from_rows(&icons[APP_GAMES], ICON_PAD, ICON_N);

	// Procedural ones: wifi arcs, gear, power symbol
	icon_t *wf = &icons[APP_WIFI], *gr = &icons[APP_SETTINGS], *pw = &icons[APP_POWER];
	wf->n = gr->n = pw->n = ICON_N;
	for (int y = 0; y < ICON_N; y++)
		for (int x = 0; x < ICON_N; x++) {
			float cx = x + 0.5f, cy = y + 0.5f;

			float dx = cx - 8, dy = cy - 14;
			float r = sqrtf(dx * dx + dy * dy);
			float a = fabsf(atan2f(dx, -dy));
			int on = r < 1.8f ||
				 (a < 0.8f && ((r > 4.2f && r < 6.2f) || (r > 8.2f && r < 10.2f) ||
					       (r > 12.2f && r < 14.2f)));
			wf->cell[y][x] = on ? '#' : '.';

			dx = cx - 8, dy = cy - 8;
			r = sqrtf(dx * dx + dy * dy);
			a = atan2f(dy, dx);
			float outer = cosf(8 * a) > 0.1f ? 7.9f : 5.9f;
			gr->cell[y][x] = r < outer ? (r < 2.6f ? '.' : '#') : '.';

			dx = cx - 8, dy = cy - 8.5f;
			r = sqrtf(dx * dx + dy * dy);
			a = fabsf(atan2f(dx, -dy));
			on = (r > 4.6f && r < 6.9f && a > 0.75f) ||
			     (fabsf(dx) < 1.1f && cy > 1 && cy < 9);
			pw->cell[y][x] = on ? '#' : '.';
		}
}

/* ---- icon variations being compared (Games and Settings) ---------------- */

static const char *const ICON_G_PAD24[ICON_MAX] = {
	"........................",
	"........................",
	"........................",
	"........................",
	"........................",
	"........................",
	"....wwwwww....wwwwww....",
	"..wwwwwwwwwwwwwwwwwwww..",
	"..wwwwddwwwwwwwwyywwww..",
	"..wwwwddwwwwwwwwyywwww..",
	"..wwddddddggggccwwrrww..",
	"..wwddddddwwwwccwwrrww..",
	"..wwwwddwwwwwwwwnnwwww..",
	"..llllddllllllllnnllll..",
	"..llllllllllllllllllll..",
	"..lllllll......lllllll..",
	"..lllllll......lllllll..",
	"...lllll........lllll...",
	"........................",
	"........................",
	"........................",
	"........................",
	"........................",
	"........................",
};
static const char *const ICON_G_CART24[ICON_MAX] = {
	"........................",
	"........................",
	"........................",
	".....ggggg....ggggg.....",
	"....glgggg....gggggg....",
	"....glggggggggggggdg....",
	"....glaaaaaaaaaaaadg....",
	"....glaaaaaaaaaaaadg....",
	"....glwwwwwwwwwwwwdg....",
	"....glwwkkkyykkkwwdg....",
	"....glwwkkyyyykkwwdg....",
	"....glwwkkyyyykkwwdg....",
	"....glwwkkkyykkkwwdg....",
	"....glwwkkkkkkkkwwdg....",
	"....glwggggggwwwwwdg....",
	"....glwwwwwwwwwwwwdg....",
	"....glggggggggggggdg....",
	"....gldgdgdgdgdgdgdg....",
	"....gldgdgdgdgdgdgdg....",
	"....gldgdgdgdgdgdgdg....",
	"....glggggggggggggdg....",
	".....gggggggggggggg.....",
	"........................",
	"........................",
};
static const char *const ICON_G_STICK24[ICON_MAX] = {
	"........................",
	"..........rrrr..........",
	".........rwrrrr.........",
	"........rwwwrrrr........",
	"........rrwwrrrr........",
	"........rrrrrrrr........",
	"........rrrrrrrr........",
	".........rrrrrr.........",
	"..........rrrr..........",
	"...........dd...........",
	"...........dd...........",
	"...........dd...........",
	".......c...dd...y.......",
	"....llcccllddllyyyll....",
	"...llllllllddllllllll...",
	"...llllllllllllllllll...",
	"...gggggggggggggggggg...",
	"...gggggggggggggggggg...",
	"...gggggggggggggggggg...",
	"...gggggggggggggggggg...",
	"...dddddddddddddddddd...",
	"........................",
	"........................",
	"........................",
};
static const char *const ICON_S_GEAR24[ICON_MAX] = {
	"........................",
	"..........wwww..........",
	"..........wwww..........",
	"....ww....wwww....ll....",
	"...wwww.wwwwwwww.llll...",
	"...wwwwwwwwwwwwwlllll...",
	"....wwwwwgggggglllll....",
	".....wwwgwwwwwlglll.....",
	"....wwwgwww..lllglll....",
	"....wwgww......llglg....",
	".wwwwwgww......llgggggg.",
	".wwwwwgw........ldggggg.",
	".wwwwwgw........gdggggg.",
	".wwwwwgwl......ggdggggg.",
	"....wwgll......ggdgg....",
	"....wwlglll..gggdggg....",
	".....lllglllgggdggg.....",
	"....lllllggddddggggg....",
	"...lllllllggggggggggg...",
	"...llll.lggggggg.gggg...",
	"....ll....gggg....gg....",
	"..........gggg..........",
	"..........gggg..........",
	"........................",
};
static const char *const ICON_S_SLIDERS24[ICON_MAX] = {
	"........................",
	"........................",
	"........................",
	"..............wwww......",
	"..............wwww......",
	"...aaaaaaaaaaawwwwddd...",
	"...aaaaaaaaaaawwwwddd...",
	"..............llll......",
	"..............llll......",
	".......wwww.............",
	".......wwww.............",
	"...aaaawwwwdddddddddd...",
	"...aaaawwwwdddddddddd...",
	".......llll.............",
	".......llll.............",
	"................wwww....",
	"................wwww....",
	"...aaaaaaaaaaaaawwwwd...",
	"...aaaaaaaaaaaaawwwwd...",
	"................llll....",
	"................llll....",
	"........................",
	"........................",
	"........................",
};
static const char *const ICON_S_TOGGLES24[ICON_MAX] = {
	"........................",
	"........................",
	"........................",
	"....nnnnnnnnnnnnnnnn....",
	"...nnnnnnnnnnwwwwwwwn...",
	"...nnnnnnnnnnwwwwwwwn...",
	"...nnnnnnnnnnwwwwwwwn...",
	"...nnnnnnnnnnllllllln...",
	"...nnnnnnnnnnllllllln...",
	"....nnnnnnnnnnnnnnnn....",
	"........................",
	"........................",
	"........................",
	"........................",
	"....gggggggggggggggg....",
	"...gwwwwwwwgggggggggg...",
	"...gwwwwwwwgggggggggg...",
	"...gwwwwwwwgggggggggg...",
	"...glllllllgggggggggg...",
	"...glllllllgggggggggg...",
	"....gggggggggggggggg....",
	"........................",
	"........................",
	"........................",
};

#define ICON_VARIANTS 4
static const char *const ICON_VAR_NAME[2][ICON_VARIANTS] = {
	{"PAD", "PAD 24", "CARTRIDGE", "JOYSTICK"},
	{"GEAR", "GEAR 24", "SLIDERS", "TOGGLES"},
};
static icon_t icon_var[2][ICON_VARIANTS]; // [0] games, [1] settings
static int icon_pick[2] = {1, 2}; // chosen 2026-11-21: shaded pad, sliders

static void gen_icon_variants(void)
{
	icon_var[0][0] = icons[APP_GAMES];
	icon_from_rows(&icon_var[0][1], ICON_G_PAD24, 24);
	icon_from_rows(&icon_var[0][2], ICON_G_CART24, 24);
	icon_from_rows(&icon_var[0][3], ICON_G_STICK24, 24);
	icon_var[1][0] = icons[APP_SETTINGS];
	icon_from_rows(&icon_var[1][1], ICON_S_GEAR24, 24);
	icon_from_rows(&icon_var[1][2], ICON_S_SLIDERS24, 24);
	icon_from_rows(&icon_var[1][3], ICON_S_TOGGLES24, 24);
}

static const icon_t *app_icon(int app)
{
	if (app == APP_GAMES)
		return &icon_var[0][icon_pick[0]];
	if (app == APP_SETTINGS)
		return &icon_var[1][icon_pick[1]];
	return &icons[app];
}

void ui_set_icons(int games, int settings)
{
	icon_pick[0] = games, icon_pick[1] = settings;
}

/* ---- settings / power lists -------------------------------------------- */

static const char *const SLEEPS[] = {"1 MIN", "5 MIN", "NEVER"};
static const char *const HUB_STYLES[] = {"SHELF", "COVERS", "GRID"};
static const char *const CC_STYLES[] = {"BOTTOM BAR", "SIDE PANEL", "CARD FAN"};
static const char *const W_STYLES[] = {"ARCADE", "SOFT", "BEVEL", "NEON"};
enum { SET_WIFI, SET_SSH, SET_BRIGHTNESS, SET_VOLUME, SET_SLEEP, SET_LOOK, SET_ABOUT };
static item_t settings_items[] = {
	{"WI-FI", IT_ACTION, 0, 0, NULL, NULL, NULL},
	{"SSH", IT_ACTION, 0, 0, NULL, NULL, NULL},
	{"BRIGHTNESS", IT_SLIDER, 7, 10, NULL, NULL, NULL},
	{"VOLUME", IT_SLIDER, 5, 10, NULL, NULL, NULL},
	{"SLEEP AFTER", IT_CHOICE, 1, 3, SLEEPS, NULL, NULL},
	{"LOOK", IT_ACTION, 0, 0, NULL, NULL, NULL},
	{"ABOUT", IT_ACTION, 0, 0, NULL, "COZISBOY 0.1 - MAINLINE LINUX", NULL},
};
// "Look": the design variations still being compared
enum { SET_HUB, SET_CC, SET_WIDGETS };
static item_t look_items[] = {
	{"GAMES LAYOUT", IT_CHOICE, 1, 3, HUB_STYLES, NULL, NULL}, // COVERS, picked 2026-11-21
	{"CONTROL CENTER", IT_CHOICE, 0, 3, CC_STYLES, NULL, NULL},
	{"WIDGET STYLE", IT_CHOICE, 0, 4, W_STYLES, NULL, NULL},
};
static item_t power_items[] = {
	{"SLEEP", IT_ACTION, 0, 0, NULL, "DEMO: WOULD SLEEP NOW", NULL},
	{"REBOOT", IT_ACTION, 0, 0, NULL, "DEMO: WOULD REBOOT NOW", NULL},
	{"SHUT DOWN", IT_ACTION, 0, 0, NULL, "DEMO: WOULD POWER OFF", NULL},
};
static list_t settings_list = {settings_items, 7, 0, 0, 0};
static list_t look_list = {look_items, 3, 0, 0, 0};
static list_t power_list = {power_items, 3, 0, 0, 0};

static list_t *app_list(int app)
{
	return app == APP_SETTINGS ? &settings_list : app == APP_POWER ? &power_list : NULL;
}

/* ---- state ------------------------------------------------------------- */

static struct {
	float t;
	int screen;
	// home
	int sel;
	float cam, hop, bump;
	int bump_dir;
	// app screen (settings, power, placeholders)
	int app, page;
	// transition: phase 1 covers the screen, then switch, phase 2 reveals
	int trans, trans_to;
	float trans_t;
	uint32_t trans_col;
	int trans_x, trans_y;
	// toast message
	const char *toast;
	float toast_t;
	// input
	unsigned prev;
	float rep[4];
	// Menu button gestures: tap / hold / double-tap
	int menu_state; // 0 up, 1 down, 2 down and already used as a hold
	int menu_pending;
	float menu_down_t, menu_since;
	// chrome variations (background, top bar, bottom bar) being compared
	int bg_style, top_style, bot_style;
	uint32_t tint; // background glow colour, follows the selection
} S;

void ui_init(void)
{
	memset(&S, 0, sizeof S);
	// chosen 2026-11-21: plain background, floating status, centred hints
	S.bg_style = 1, S.top_style = 1, S.bot_style = 2;
	gen_icons();
	gen_icon_variants();
	games_init();
	S.hop = 1;
	sfx_set_volume(settings_items[SET_VOLUME].val / 10.0f);
	emu_set_volume(settings_items[SET_VOLUME].val / 10.0f);
	games_set_style(look_items[SET_HUB].val, look_items[SET_CC].val);
}

float ui_time(void)
{
	return S.t;
}

void ui_set_widgets(int style)
{
	wstyle = look_items[SET_WIDGETS].val = style;
}

void ui_set_chrome(int bg, int top, int bot)
{
	S.bg_style = bg, S.top_style = top, S.bot_style = bot;
}

void ui_set_layout(int hub, int cc)
{
	look_items[SET_HUB].val = hub;
	look_items[SET_CC].val = cc;
	games_set_style(hub, cc);
}

int ui_volume(void)
{
	return settings_items[SET_VOLUME].val;
}

void ui_set_volume(int v)
{
	settings_items[SET_VOLUME].val = v;
	sfx_set_volume(v / 10.0f);
	emu_set_volume(v / 10.0f);
}

int ui_screen(void)
{
	return S.screen;
}

/* ---- helpers ------------------------------------------------------------ */

float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

float ease_out_bounce(float t)
{
	const float n = 7.5625f, d = 2.75f;
	if (t < 1 / d)
		return n * t * t;
	if (t < 2 / d) {
		t -= 1.5f / d;
		return n * t * t + 0.75f;
	}
	if (t < 2.5f / d) {
		t -= 2.25f / d;
		return n * t * t + 0.9375f;
	}
	t -= 2.625f / d;
	return n * t * t + 0.984375f;
}

// Buttons that went down this frame, plus auto-repeat for held directions.
static unsigned pressed(unsigned held, float dt)
{
	unsigned p = held & ~S.prev;
	for (int b = 0; b < 4; b++) {
		unsigned m = 1u << b;
		if (!(held & m)) {
			S.rep[b] = 0;
			continue;
		}
		float before = S.rep[b];
		S.rep[b] += dt;
		const float delay = 0.32f, every = 0.085f;
		if (before >= delay &&
		    (int)((S.rep[b] - delay) / every) != (int)((before - delay) / every))
			p |= m;
		else if (before < delay && S.rep[b] >= delay)
			p |= m;
	}
	S.prev = held;
	return p;
}

void go(int to, uint32_t col, int x, int y)
{
	S.trans = 1;
	S.trans_t = 0;
	S.trans_to = to;
	S.trans_col = col;
	S.trans_x = x;
	S.trans_y = y;
	sfx_play(SFX_WHOOSH, 0);
}

void toast(const char *msg)
{
	S.toast = msg;
	S.toast_t = 0;
	sfx_play(SFX_POP, 0);
}

/* ---- lists -------------------------------------------------------------- */

int list_update(list_t *l, unsigned p, float dt)
{
	l->flash = l->flash > 0 ? l->flash - dt : 0;
	l->y += (l->sel - l->y) * (1 - expf(-dt * 22));
	if (!l->n)
		return -1;
	if (p & (BTN_DOWN | BTN_UP))
		sfx_play(SFX_TICK, 0);
	if (p & BTN_DOWN)
		l->sel = (l->sel + 1) % l->n;
	if (p & BTN_UP)
		l->sel = (l->sel + l->n - 1) % l->n;
	item_t *i = &l->it[l->sel];
	int d = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
	if (i->type == IT_SLIDER && d) {
		int old = i->val;
		i->val = (int)clampf((float)(i->val + d), 0, (float)i->max);
		sfx_play(i->val != old ? SFX_CHANGE : SFX_BUMP, i->val);
	}
	if (i->type == IT_CHOICE && d) {
		i->val = (i->val + d + i->max) % i->max;
		sfx_play(SFX_CHANGE, i->val * 3);
	}
	if (i->type == IT_TOGGLE && (d || (p & BTN_A))) {
		i->val = !i->val;
		sfx_play(i->val ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF, 0);
	}
	if (i->type == IT_ACTION && (p & BTN_A)) {
		l->flash = 0.15f;
		return l->sel;
	}
	return -1;
}

void list_draw(canvas *c, const list_t *l, int x, int y, int w, int rh, uint32_t col)
{
	int ts = rh >= 44 ? 3 : 2;          // text scale
	int ty = (rh - 6 - 7 * ts) / 2;     // text offset inside the bar
	int by = y + (int)lrintf(l->y * rh);
	wbox(c, x, by, w, rh - 6, l->flash > 0 ? WHITE : col, W_NORMAL);
	uint32_t ink = wink(col, W_NORMAL);

	for (int i = 0; i < l->n; i++) {
		const item_t *it = &l->it[i];
		int ry = y + i * rh;
		int sel = i == l->sel;
		uint32_t tc = sel ? ink : 0xdddddd;
		if (sel && fmodf(S.t, 0.6f) < 0.4f)
			text(c, x + 12, ry + ty, ">", ts, ink);
		text(c, x + 12 + 8 * ts, ry + ty, it->label, ts, tc);
		int xr = x + w - 16;
		int mid = ry + (rh - 6) / 2;
		switch (it->type) {
		case IT_TOGGLE: {
			int on = it->val;
			if (sel)
				rrect(c, xr - 62, mid - 14, 64, 28, 5, BLACK);
			rrect(c, xr - 60, mid - 12, 60, 24, 4, on ? PAL[3] : 0x444444);
			fill(c, on ? xr - 26 : xr - 56, mid - 8, 22, 16, WHITE);
			if (on)
				text(c, xr - 54, mid - 7, "ON", 2, BLACK);
			break;
		}
		case IT_SLIDER: {
			int bw = 10, gap = 3, n = it->max;
			int sx = xr - n * (bw + gap) + gap;
			for (int k = 0; k < n; k++) {
				uint32_t bc = k < it->val ? (sel ? WHITE : col)
							  : (sel ? mix(col, BLACK, 0.45f) : 0x333333);
				fill(c, sx + k * (bw + gap), mid - 10, bw, 20, bc);
			}
			break;
		}
		case IT_CHOICE: {
			char buf[32];
			snprintf(buf, sizeof buf, sel ? "< %s >" : "%s", it->choices[it->val]);
			text(c, xr - text_width(buf, ts), ry + ty, buf, ts, sel ? ink : col);
			break;
		}
		case IT_ACTION:
			text(c, xr - text_width(">", ts), ry + ty, ">", ts, sel ? ink : GREY);
			if (it->note)
				text(c, xr - text_width(">", ts) - 12 - text_width(it->note, 2),
				     ry + (rh - 6 - 14) / 2, it->note, 2, sel ? ink : col);
			break;
		}
	}
}

/* ---- input / logic ----------------------------------------------------- */

/* Home carousel items: open games (most recent first) or, when none are open,
 * the last played game; then the apps. */
// Browser, Files, Terminal, Music, Wi-Fi and Power exist but are hidden from
// home for now (decided 2026-11-21): Wi-Fi/Power will move to Settings and the
// Control Center.
static const int HOME_APPS[] = {APP_GAMES, APP_SETTINGS};
#define NHOME_APPS ((int)(sizeof HOME_APPS / sizeof *HOME_APPS))
typedef struct {
	int app;  // app id, or -1 for a game
	int game; // games.c reference when app == -1
} hitem_t;
#define MAX_HOME 16

static int home_items(hitem_t *it)
{
	int refs[8], n = games_home_refs(refs), k = 0;
	for (int i = 0; i < n; i++)
		it[k].app = -1, it[k].game = refs[i], k++;
	for (int i = 0; i < NHOME_APPS; i++)
		it[k].app = HOME_APPS[i], it[k].game = 0, k++;
	return k;
}

// Menu works like the PS button: tap = Control Center, hold = Home,
// double-tap = previous game. The Control Center opens on the first press with
// no delay; a quick second press turns it into a game switch instead.
#define MENU_HOLD 0.45f
#define MENU_DOUBLE 0.28f
static void menu_gesture(unsigned held, float dt)
{
	int down = (held & BTN_MENU) != 0;
	if (down && S.menu_state == 0) { // pressed
		S.menu_state = 1;
		S.menu_down_t = 0;
		if (S.menu_pending && S.menu_since < MENU_DOUBLE) {
			S.menu_pending = 0;
			S.menu_state = 2; // consumed: no hold, no tap on release
			games_cc_close();
			games_menu_double();
			return;
		}
		int was_open = games_cc_active();
		games_menu_tap(); // toggle the Control Center right away
		S.menu_pending = !was_open; // only an opening tap can start a double-tap
		S.menu_since = 0;
		return;
	}
	if (down) {
		S.menu_down_t += dt;
		if (S.menu_state == 1 && S.menu_down_t >= MENU_HOLD) {
			S.menu_state = 2;
			S.menu_pending = 0;
			games_cc_close();
			if (S.screen != SCR_HOME) {
				sfx_play(SFX_BACK, 0);
				go(SCR_HOME, BG0, W / 2, H / 2);
			}
		}
		return;
	}
	if (S.menu_state == 1) // released after a short press: start the double-tap window
		S.menu_since = 0;
	S.menu_state = 0;
	if (S.menu_pending) {
		S.menu_since += dt;
		if (S.menu_since >= MENU_DOUBLE)
			S.menu_pending = 0;
	}
}

// Colour of what's selected (drives the background glow).
static uint32_t selected_colour(void)
{
	if (S.screen == SCR_APP)
		return PAL[S.app];
	if (S.screen != SCR_HOME)
		return PAL[APP_GAMES];
	hitem_t it[MAX_HOME];
	int n = home_items(it);
	hitem_t *h = &it[S.sel < n ? S.sel : n - 1];
	return h->app >= 0 ? PAL[h->app] : games_ref_col(h->game);
}

static unsigned held_now;

unsigned ui_held(void)
{
	return held_now;
}

static void update(unsigned p, unsigned held, float dt)
{
	held_now = held;
	S.t += dt;
	sys_tick(dt); // Wi-Fi connects etc. carry on in the background
	S.hop += dt;
	S.bump += dt;
	S.cam += (S.sel - S.cam) * (1 - expf(-dt * 14));
	S.tint = mix(S.tint, selected_colour(), 1 - expf(-dt * 8));
	if (S.toast)
		S.toast_t += dt;
	if (S.toast && S.toast_t > 2.2f)
		S.toast = NULL;

	if (S.trans) {
		S.trans_t += dt;
		if (S.trans == 1 && S.trans_t >= 0.32f) {
			if (S.trans_to == SCR_HOME && S.screen == SCR_GAME)
				S.sel = 0, S.cam = 0; // back from a game: focus its tile
			S.screen = S.trans_to;
			S.trans = 2;
			S.trans_t = 0;
			S.hop = 0;
		} else if (S.trans == 2 && S.trans_t >= 0.32f) {
			S.trans = 0;
		}
		return; // no input during transitions
	}

	if (S.screen != SCR_LIBRARY) // the game list doesn't use Menu
		menu_gesture(held, dt);
	p &= ~BTN_MENU;
	if (games_cc_active()) {
		games_cc_update(p, dt);
		return;
	}

	switch (S.screen) {
	case SCR_HOME: {
		hitem_t it[MAX_HOME];
		int n = home_items(it);
		if (S.sel >= n)
			S.sel = n - 1;
		int d = (p & (BTN_RIGHT | BTN_R)) ? 1 : (p & (BTN_LEFT | BTN_L)) ? -1 : 0;
		if (d) {
			if (S.sel + d < 0 || S.sel + d >= n) {
				S.bump = 0;
				S.bump_dir = d;
				sfx_play(SFX_BUMP, 0);
			} else {
				S.sel += d;
				S.hop = 0;
				sfx_play(SFX_MOVE, S.sel);
			}
		}
		hitem_t *h = &it[S.sel];
		if ((p & BTN_A) && h->app < 0) {
			games_ref_open(h->game, W / 2, 190);
		} else if (p & BTN_A) {
			sfx_play(SFX_OPEN, h->app);
			if (h->app == APP_GAMES) {
				games_hub_focus_first();
				go(SCR_LIBRARY, PAL[h->app], W / 2, 190);
			} else {
				S.app = h->app;
				S.page = PAGE_MAIN;
				list_t *l = app_list(S.app);
				if (l)
					l->sel = 0, l->y = 0;
				go(SCR_APP, PAL[h->app], W / 2, 190);
			}
		}
		if ((p & BTN_X) && h->app < 0 && h->game >= 0)
			games_ref_close(h->game);
		break;
	}
	case SCR_APP: {
		if (S.app == APP_SETTINGS && (S.page == PAGE_WIFI || S.page == PAGE_SSH)) {
			if (net_update(S.page, p, dt))
				S.page = PAGE_MAIN;
			break;
		}
		list_t *l = S.app == APP_SETTINGS && S.page == PAGE_LOOK ? &look_list : app_list(S.app);
		if (p & BTN_B) {
			sfx_play(SFX_BACK, 0);
			if (S.page != PAGE_MAIN)
				S.page = PAGE_MAIN;
			else
				go(SCR_HOME, BG0, W / 2, H / 2);
			return;
		}
		if (!l)
			return;
		if (l == &settings_list) {
			settings_items[SET_WIFI].note = net_wifi_note();
			settings_items[SET_SSH].note = net_ssh_note();
		}
		int a = list_update(l, p, dt);
		if (l == &settings_list && (a == SET_WIFI || a == SET_SSH || a == SET_LOOK)) {
			sfx_play(SFX_OPEN, APP_SETTINGS);
			S.page = a == SET_WIFI ? PAGE_WIFI : a == SET_SSH ? PAGE_SSH : PAGE_LOOK;
			if (S.page == PAGE_LOOK)
				look_list.sel = 0, look_list.y = 0;
			else
				net_open(S.page);
		} else if (a >= 0) {
			sfx_play(SFX_CONFIRM, 0);
			if (l->it[a].toast)
				toast(l->it[a].toast);
		}
		sfx_set_volume(settings_items[SET_VOLUME].val / 10.0f);
		emu_set_volume(settings_items[SET_VOLUME].val / 10.0f);
		games_set_style(look_items[SET_HUB].val, look_items[SET_CC].val);
		wstyle = look_items[SET_WIDGETS].val;
		break;
	}
	default:
		games_update(S.screen, p, held, dt);
	}
}

/* ---- drawing ------------------------------------------------------------ */

static void bg_stars(canvas *c)
{
	for (int y = 0; y < H; y++)
		fill(c, 0, y, W, 1, mix(BG0, BG1, (float)y / H));
	// two layers of drifting stars; the near layer follows the carousel
	uint32_t seed = 777;
	for (int s = 0; s < 48; s++) {
		seed = seed * 1103515245u + 12345u;
		float x = (float)((seed >> 8) % W);
		seed = seed * 1103515245u + 12345u;
		int y = (int)((seed >> 8) % H);
		int near = s & 1;
		x -= S.t * (near ? 6 : 2) + S.cam * (near ? 24 : 8);
		x = fmodf(x, W);
		if (x < 0)
			x += W;
		float b = 0.2f + 0.45f * (0.5f + 0.5f * sinf(S.t * (1 + (s % 4) * 0.5f) + s));
		int sz = near ? 4 : 2;
		fill(c, (int)x, y, sz, sz, mix(BG0, WHITE, b));
	}
}

static void bg_plain(canvas *c)
{
	for (int y = 0; y < H; y++)
		fill(c, 0, y, W, 1, mix(0x161616, 0x0c0c0c, (float)y / H));
}

// soft glow in the selected colour, drawn in chunky 8px blocks with banding
static void bg_glow(canvas *c)
{
	const int b = 8;
	for (int y = 0; y < H; y += b)
		for (int x = 0; x < W; x += b) {
			float dx = (x + b / 2 - W / 2) / 330.0f, dy = (y + b / 2 - 180) / 220.0f;
			float k = 1 - sqrtf(dx * dx + dy * dy);
			k = k < 0 ? 0 : k * k;
			k = floorf(k * 6) / 6 * 0.45f; // 6 bands
			fill(c, x, y, b, b, mix(0x0e0e0e, S.tint, k));
		}
}

static void bg_checker(canvas *c)
{
	const int b = 32;
	int off = (int)(S.t * 8) % (2 * b);
	for (int y = -2 * b; y < H; y += b)
		for (int x = -2 * b; x < W; x += b) {
			int k = ((x + 2 * b) / b + (y + 2 * b) / b) & 1;
			fill(c, x + off, y + off, b, b, k ? 0x161616 : 0x1d1d1d);
		}
}

static void bg_keyart(canvas *c)
{
	if (S.screen == SCR_HOME) {
		hitem_t it[MAX_HOME];
		int n = home_items(it);
		hitem_t *h = &it[S.sel < n ? S.sel : n - 1];
		if (h->app < 0) {
			games_ref_draw_bg(c, h->game);
			return;
		}
	}
	for (int y = 0; y < H; y++)
		fill(c, 0, y, W, 1, mix(mix(S.tint, BLACK, 0.55f), 0x0c0c0c, (float)y / H));
}

static void bg_synthwave(canvas *c)
{
	const int hy = 300;
	for (int y = 0; y < hy; y += 4)
		fill(c, 0, y, W, 4, mix(0x0c0420, 0x4a1048, (float)y / hy));
	// striped pixel sun
	int cx = W / 2, cy = hy - 20, r = 84;
	for (int dy = -r; dy < 0 && cy + dy < hy; dy += 2) {
		float fy = dy + 1;
		int hw = (int)sqrtf((float)(r * r) - fy * fy);
		int yy = cy + dy;
		float k = (float)(dy + r) / r;
		int gap = dy > -r / 2 && ((-dy) % 12) < (int)(k * 6);
		if (!gap)
			fill(c, cx - hw, yy, 2 * hw, 2, mix(0xffd93d, 0xff4f9a, k));
	}
	fill(c, 0, hy, W, H - hy, 0x12041e);
	// perspective grid floor
	float ph = fmodf(S.t * 0.6f, 1);
	for (int i = 0; i < 14; i++) {
		float z = (i + ph) / 14;
		int y = hy + (int)(z * z * (H - hy));
		fill(c, 0, y, W, 2, mix(0x12041e, 0xff3fd2, 0.3f + 0.5f * z));
	}
	for (int k = -12; k <= 12; k++)
		for (int y = hy; y < H; y += 2) {
			float t = (float)(y - hy) / (H - hy);
			int x = cx + (int)(k * (10 + t * 70));
			fill(c, x, y, 2, 2, mix(0x12041e, 0x3fc8ff, 0.25f + 0.5f * t));
		}
}

void draw_background(canvas *c)
{
	switch (S.bg_style) {
	case 1: bg_plain(c); break;
	case 2: bg_glow(c); break;
	case 3: bg_checker(c); break;
	case 4: bg_keyart(c); break;
	case 5: bg_synthwave(c); break;
	default: bg_stars(c);
	}
}

static void draw_rainbow(canvas *c, int y, int h)
{
	for (int i = 0; i < 8; i++)
		fill(c, i * W / 8, y, W / 8 + 1, h, PAL[i]);
}

static void clock_text(char *buf, size_t n)
{
	const char *fixed = getenv("COZ_CLOCK"); // freeze frames use a fixed time
	if (fixed) {
		snprintf(buf, n, "%s", fixed);
		return;
	}
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(buf, n, "%02d:%02d", tm.tm_hour, tm.tm_min);
}

// Clock, battery and wifi, right-aligned at xr. Returns the left edge.
#define ST_NOCLOCK 1
#define ST_SHADOW 2
static int draw_status(canvas *c, int xr, int y, int flags)
{
	int sh = flags & ST_SHADOW;
	uint32_t wh = sh ? BLACK : WHITE;
	int x = xr;
	if (!(flags & ST_NOCLOCK)) {
		char buf[8];
		clock_text(buf, sizeof buf);
		x -= text_width(buf, 2);
		text(c, x, y + 1, buf, 2, wh);
		x -= 14;
	}
	// battery (placeholder level until the PMIC fuel gauge is wired up)
	float level = 0.8f;
	x -= 30;
	fill(c, x, y, 26, 14, wh);
	fill(c, x + 2, y + 2, 22, 10, sh ? BLACK : BAR);
	fill(c, x + 26, y + 4, 3, 6, wh);
	for (int k = 0; k < 4; k++)
		if (level > k / 4.0f)
			fill(c, x + 4 + k * 5, y + 4, 4, 6, sh ? BLACK : PAL[3]);
	// wifi bars
	x -= 30;
	for (int k = 0; k < 4; k++)
		fill(c, x + k * 6, y + 12 - k * 4, 4, 4 + k * 4, sh ? BLACK : k < 3 ? WHITE : 0x555555);
	return x;
}

static int status_width(int flags)
{
	char buf[8];
	clock_text(buf, sizeof buf);
	return (flags & ST_NOCLOCK ? 0 : text_width(buf, 2) + 14) + 60;
}

static void wordmark(canvas *c, int x, int y)
{
	static const char word[] = "COZISBOY";
	for (int i = 0; word[i]; i++) {
		char s1[2] = {word[i], 0};
		text(c, x + i * 12 + 2, y + 2, s1, 2, BLACK);
		text(c, x + i * 12, y, s1, 2, PAL[i]);
	}
}

static void draw_topbar(canvas *c)
{
	switch (S.top_style) {
	case 1: // floating: status only, with drop shadows
		draw_status(c, W - 12, 12, ST_SHADOW);
		draw_status(c, W - 14, 10, 0);
		break;
	case 2: { // pills
		wordmark(c, 16, 12);
		int l = W - 20 - status_width(0);
		rrect(c, l - 14, 4, W - 6 - (l - 14), 28, 6, BLACK);
		rrect(c, l - 12, 6, W - 10 - (l - 12), 24, 5, 0x262626);
		draw_status(c, W - 20, 11, 0);
		break;
	}
	case 3: { // big centred clock
		char buf[8];
		clock_text(buf, sizeof buf);
		int tw = text_width(buf, 3);
		text(c, (W - tw) / 2 + 2, 10, buf, 3, BLACK);
		text(c, (W - tw) / 2, 8, buf, 3, WHITE);
		draw_status(c, W - 12, 12, ST_SHADOW | ST_NOCLOCK);
		draw_status(c, W - 14, 10, ST_NOCLOCK);
		wordmark(c, 16, 12);
		break;
	}
	default: // bar with rainbow stripe
		fill(c, 0, 0, W, 34, BAR);
		draw_rainbow(c, 34, 3);
		wordmark(c, 14, 11);
		draw_status(c, W - 14, 10, 0);
	}
}

// Draws one button hint at *x and advances it. With c == NULL it only measures.
void draw_hint(canvas *c, int *x, const char *btn, const char *label, uint32_t col)
{
	int bw = text_width(btn, 2) + 10;
	int total = bw + 8 + text_width(label, 2);
	if (c) {
		if (S.bot_style && S.screen != SCR_GAME) // no bar: dark backing for legibility
			rrect(c, *x - 6, 447, total + 12, 30, 5, 0x0a0a0a);
		wbox(c, *x, 452, bw, 20, col, W_NORMAL);
		text(c, *x + 5, 455, btn, 2, wink(col, W_NORMAL));
		text(c, *x + bw + 8, 455, label, 2, 0xcccccc);
	}
	*x += total + 22;
}

void draw_hintbar(canvas *c)
{
	fill(c, 0, 444, W, 36, BAR);
	fill(c, 0, 444, W, 2, 0x2a2a2a);
}

static void bottom_hints(canvas *c, int *x);

static void bottom_hints(canvas *c, int *xp)
{
	int x = *xp;
	switch (S.screen) {
	case SCR_HOME: {
		hitem_t it[MAX_HOME];
		int n = home_items(it);
		hitem_t *h = &it[S.sel < n ? S.sel : n - 1];
		draw_hint(c, &x, "<>", "MOVE", 0xdddddd);
		if (h->app >= 0) {
			draw_hint(c, &x, "A", "OPEN", PAL[3]);
		} else if (h->game >= 0) {
			draw_hint(c, &x, "A", "RESUME", PAL[3]);
			draw_hint(c, &x, "X", "CLOSE", PAL[4]);
		} else {
			draw_hint(c, &x, "A", "PLAY", PAL[3]);
		}
		if (games_running())
			draw_hint(c, &x, "MENU", "CONTROL", PAL[APP_GAMES]);
		break;
	}
	case SCR_APP:
		if (S.app == APP_SETTINGS && (S.page == PAGE_WIFI || S.page == PAGE_SSH)) {
			net_hints(c, S.page, &x);
			break;
		}
		if (app_list(S.app)) {
			draw_hint(c, &x, "^V", "MOVE", 0xdddddd);
			draw_hint(c, &x, "<>", "CHANGE", 0xdddddd);
			draw_hint(c, &x, "A", "OK", PAL[3]);
		}
		draw_hint(c, &x, "B", "BACK", PAL[0]);
		break;
	default:
		games_hints(c, S.screen, &x);
	}
	*xp = x;
}

static void draw_bottombar(canvas *c)
{
	if (S.bot_style == 3)
		return; // no hints
	if (S.bot_style == 0) {
		draw_hintbar(c);
		int x = 16;
		bottom_hints(c, &x);
		return;
	}
	int w = 0;
	bottom_hints(NULL, &w); // measure
	w -= 22;
	int x = S.bot_style == 1 ? W - 16 - w : (W - w) / 2;
	bottom_hints(c, &x);
}

// One app tile: black ring, white outline, coloured body, bevel, icon.
void draw_tile(canvas *c, int app, int x, int y, int size, int scale, int dim)
{
	uint32_t col = PAL[app];
	wbox(c, x, y, size, size, col, dim ? W_DIM : W_NORMAL);
	uint32_t f = dim ? mix(col, BG0, 0.55f) : col;
	uint32_t main = wstyle == 3 ? (dim ? mix(col, BG0, 0.4f) : col) : dim ? 0xbdbdbd : WHITE;
	uint32_t detail = wstyle == 3 ? mix(col, BG0, 0.75f) : mix(f, BLACK, 0.45f);
	uint32_t shadow = wstyle == 3 ? mix(col, BG0, 0.7f) : mix(f, BLACK, 0.4f);
	const icon_t *ic = app_icon(app);
	// callers size icons for a 16-cell grid; bigger grids keep the same footprint
	int sc = (scale * ICON_N + ic->n - 1) / ic->n;
	if (sc < 1)
		sc = 1;
	int isz = ic->n * sc;
	draw_icon_dim(c, ic, x + (size - isz) / 2, y + (size - isz) / 2, sc, main, detail, shadow,
		      dim ? 0.5f : 0, f);
}

void draw_header(canvas *c, int app, const char *title)
{
	draw_tile(c, app, 24, 54, 60, 3, 0);
	text_arcade(c, 104, 64, title, 4, PAL[app]);
}

static void brackets(canvas *c, int x, int y, int w, int h, int g)
{
	if (fmodf(S.t, 0.8f) >= 0.6f)
		return;
	g += (int)lrintf(sinf(S.t * 6) * 1.5f);
	int L = 18, T = 4, x0 = x - g, y0 = y - g, x1 = x + w + g, y1 = y + h + g;
	fill(c, x0, y0, L, T, WHITE), fill(c, x0, y0, T, L, WHITE);
	fill(c, x1 - L, y0, L, T, WHITE), fill(c, x1 - T, y0, T, L, WHITE);
	fill(c, x0, y1 - T, L, T, WHITE), fill(c, x0, y1 - L, T, L, WHITE);
	fill(c, x1 - L, y1 - T, L, T, WHITE), fill(c, x1 - T, y1 - L, T, L, WHITE);
}

static void draw_home(canvas *c)
{
	hitem_t it[MAX_HOME];
	int n = home_items(it);
	if (S.sel >= n)
		S.sel = n - 1;
	const int spacing = 140, size = 112, base_y = 92;
	float bump = S.bump < 0.3f ? S.bump_dir * 0.12f * sinf(S.bump / 0.3f * PI) : 0;
	float cam = S.cam + bump;
	int ngames = 0;
	while (ngames < n && it[ngames].app < 0)
		ngames++;

	for (int i = 0; i < n; i++) {
		int x = W / 2 + (int)lrintf((i - cam) * spacing) - size / 2;
		if (x < -size - 10 || x > W + 10)
			continue;
		int y = base_y + 24, sel = i == S.sel;
		if (sel) {
			float hop = S.hop < 0.25f ? sinf(S.hop / 0.25f * PI) * 12 : 0;
			int bob = 2 * (int)lrintf(sinf(S.t * 4) * 1.0f);
			rrect(c, x + 8, y + size + 6, size - 16, 8, 4, 0x080808); // ground shadow
			y -= 14 + bob + (int)hop;
		}
		if (it[i].app >= 0)
			draw_tile(c, it[i].app, x, y, size, 5, !sel);
		else
			games_ref_tile(c, it[i].game, x, y, size, !sel);
		if (sel)
			brackets(c, x, y, size, size, 12);
		// dotted divider between the games and the apps
		if (ngames && i == ngames - 1) {
			int dx = x + size + (spacing - size) / 2 - 1;
			for (int yy = base_y + 10; yy < base_y + 24 + size; yy += 10)
				fill(c, dx, yy, 3, 6, 0x555555);
		}
	}

	// page dots: games green, apps in their colour
	int dx = W / 2 - (n * 16 - 8 + 12) / 2;
	for (int i = 0; i < n; i++) {
		int w = i == S.sel ? 20 : 8;
		uint32_t dc = it[i].app >= 0 ? PAL[it[i].app] : games_ref_col(it[i].game);
		fill(c, dx, 272, w, 8, i == S.sel ? dc : 0x444444);
		dx += w + 8;
	}

	// name (letters drop in) and description
	hitem_t *h = &it[S.sel];
	char desc[48];
	const char *name;
	uint32_t col;
	if (h->app >= 0) {
		name = APP_NAME[h->app], col = PAL[h->app];
		snprintf(desc, sizeof desc, "%s", APP_DESC[h->app]);
	} else {
		name = games_ref_title(h->game), col = games_ref_col(h->game);
		games_ref_status(h->game, desc, sizeof desc);
	}
	int scale = 5; // long game titles shrink to fit
	while (scale > 2 && text_width(name, scale) > W - 30)
		scale--;
	int nx = (W - text_width(name, scale)) / 2;
	for (int i = 0; name[i]; i++) {
		float lt = clampf((S.hop - i * 0.03f) / 0.3f, 0, 1);
		int off = -(int)((1 - ease_out_bounce(lt)) * 18);
		char s1[2] = {name[i], 0};
		text_arcade(c, nx + i * 6 * scale, 306 + off, s1, scale, col);
	}
	if (h->app < 0 && h->game >= 0) // an open game: same pill as the games view
		games_open_pill(c, W / 2, 362, h->game);
	else
		text(c, (W - text_width(desc, 2)) / 2, 368, desc, 2, GREY);
}

static void draw_app(canvas *c)
{
	if (S.app == APP_SETTINGS && (S.page == PAGE_WIFI || S.page == PAGE_SSH)) {
		net_draw(c, S.page);
		return;
	}
	if (S.app == APP_SETTINGS && S.page == PAGE_LOOK) {
		draw_header(c, APP_SETTINGS, "LOOK");
		list_draw(c, &look_list, 20, 136, W - 40, 46, PAL[APP_SETTINGS]);
		return;
	}
	draw_header(c, S.app, APP_NAME[S.app]);
	list_t *l = app_list(S.app);
	if (l) {
		list_draw(c, l, 20, 136, W - 40, l->n > 6 ? 38 : 46, PAL[S.app]);
		return;
	}
	// placeholder for apps that don't exist yet
	int bob = 2 * (int)lrintf(sinf(S.t * 3) * 2);
	draw_tile(c, S.app, W / 2 - 64, 160 + bob, 128, 6, 0);
	const char *msg = "COMING SOON";
	text_arcade(c, (W - text_width(msg, 4)) / 2, 324, msg, 4, PAL[S.app]);
	const char *sub = "THIS APP IS NOT BUILT YET";
	text(c, (W - text_width(sub, 2)) / 2, 380, sub, 2, GREY);
}

static void draw_toast(canvas *c)
{
	if (!S.toast)
		return;
	float a = clampf(S.toast_t / 0.15f, 0, 1) * clampf((2.2f - S.toast_t) / 0.2f, 0, 1);
	int w = text_width(S.toast, 2) + 32;
	int x = (W - w) / 2, y = 400 + (int)((1 - a) * 20);
	wbox(c, x, y, w, 30, WHITE, W_NORMAL);
	text(c, x + 16, y + 8, S.toast, 2, wink(WHITE, W_NORMAL));
}

// Squares grow outward from the origin to cover the screen, then shrink away.
static void draw_transition(canvas *c)
{
	if (!S.trans)
		return;
	const int cell = 40;
	float p = clampf(S.trans_t / 0.32f, 0, 1);
	float maxd = sqrtf((float)(W * W + H * H));
	for (int gy = 0; gy < H; gy += cell)
		for (int gx = 0; gx < W; gx += cell) {
			float dx = gx + cell / 2.0f - S.trans_x, dy = gy + cell / 2.0f - S.trans_y;
			float d = sqrtf(dx * dx + dy * dy) / maxd * 0.6f;
			float l = clampf((p - d) / 0.4f, 0, 1);
			if (S.trans == 2)
				l = 1 - l;
			int s = (int)(l * cell + 0.5f) & ~1;
			if (s > 0)
				fill(c, gx + (cell - s) / 2, gy + (cell - s) / 2, s, s, S.trans_col);
		}
}

void ui_frame(uint32_t *buf, int stride, float dt, unsigned held)
{
	canvas cv = {buf, W, H, stride}, *c = &cv;
	update(pressed(held, dt), held, dt);

	if (S.screen == SCR_GAME) {
		games_draw(c, SCR_GAME); // full screen, draws its own overlay
	} else {
		draw_background(c);
		if (S.screen == SCR_HOME)
			draw_home(c);
		else if (S.screen == SCR_APP)
			draw_app(c);
		else
			games_draw(c, S.screen);
		draw_topbar(c);
		draw_bottombar(c);
	}
	games_cc_draw(c);
	draw_toast(c);
	draw_transition(c);
}

/* ---- icon sheet (design review) ---------------------------------------------- */

void ui_icon_sheet(uint32_t *buf, int stride)
{
	canvas cv = {buf, W, H, stride}, *c = &cv;
	fill(c, 0, 0, W, H, 0x111111);
	int save[2] = {icon_pick[0], icon_pick[1]};
	static const int apps[2] = {APP_GAMES, APP_SETTINGS};
	for (int row = 0; row < 2; row++)
		for (int v = 0; v < ICON_VARIANTS; v++) {
			icon_pick[row] = v;
			int x = 16 + v * 156, y = 4 + row * 232;
			draw_tile(c, apps[row], x + 14, y + 14, 112, 5, 0);
			draw_tile(c, apps[row], x + 8, y + 146, 60, 3, 0);
			draw_tile(c, apps[row], x + 76, y + 146, 60, 3, 1);
			char lbl[24];
			snprintf(lbl, sizeof lbl, "%c%d %s", row ? 'S' : 'G', v, ICON_VAR_NAME[row][v]);
			text(c, x + 8, y + 212, lbl, 2, 0xcccccc);
		}
	icon_pick[0] = save[0], icon_pick[1] = save[1];
}
