// Internal interface shared by the UI's source files.
#ifndef UI_PRIV_H
#define UI_PRIV_H

#include "gfx.h"
#include "sfx.h"
#include "ui.h"

#include <math.h>

#define W UI_W
#define H UI_H
#define PI 3.14159265f

// The eight logo letter colours.
extern const uint32_t PAL[8];
#define BG0 0x111111u
#define BG1 0x1d1d1du
#define BAR 0x0b0b0bu
#define GREY 0x9a9a9au

enum { SCR_HOME, SCR_APP, SCR_LIBRARY, SCR_GAME };
// pages inside Settings
enum { PAGE_MAIN, PAGE_WIFI, PAGE_SSH, PAGE_LOOK, PAGE_ABOUT, PAGE_CLOCK };
enum { APP_BROWSER, APP_PCLINK, APP_TERM, APP_MUSIC, APP_GAMES, APP_WIFI,
       APP_SETTINGS, APP_POWER, NAPPS };

/* ---- widget styles: every box, image frame and focus ring goes through these
 * 0 ARCADE (white outline + black ring), 1 SOFT (rounded, no outline),
 * 2 BEVEL (physical button edges), 3 NEON (dark fill, coloured outline) ---- */
extern int wstyle;
enum { W_NORMAL, W_SEL, W_DIM };
void wbox(canvas *c, int x, int y, int w, int h, uint32_t col, int state);
void wframe(canvas *c, int x, int y, int w, int h, uint32_t accent); // around content
void wfocus(canvas *c, int x, int y, int w, int h, int gap);        // selection ring
uint32_t wink(uint32_t fill, int state); // text colour on a wbox of that colour

float clampf(float v, float lo, float hi);
float ease_out_bounce(float t);
float ui_time(void);
int ui_volume(void);         // 0..10, shared by UI sounds and games
void ui_set_volume(int v);
int ui_brightness(void);     // 0..10 (Settings slider; the device front end drives the backlight)
void ui_set_brightness(int v);
int ui_screen(void);
unsigned ui_held(void); // buttons held this frame (for hold-to-confirm)

// Switch screens with the pixel-square transition, starting from (x, y).
void go(int screen, uint32_t col, int x, int y);
void toast(const char *msg);

void draw_background(canvas *c);
void draw_tile(canvas *c, int app, int x, int y, int size, int scale, int dim);
void draw_header(canvas *c, int app, const char *title);
void ui_page_anim(int back); // animate a page change just made (Look > TRANSITION)
int ui_transitioning(void);  // a transition is running
int games_starting(void);    // the game on screen is still booting (blank screen)

/* ---- saved settings: <saves>/settings, "key=value" lines ---- */
typedef struct {
	const char *key;
	int *val;
	int n; // number of values (0..n-1)
} pref_t;
int games_prefs(pref_t *out);    // the in-game settings to save; returns how many
void games_prefs_apply(void);    // after loading them
void draw_hint(canvas *c, int *x, const char *btn, const char *label, uint32_t col);
void draw_hintbar(canvas *c); // background of the bottom bar; hints start at x=16

/* ---- vertical menus (settings, power, in-game menu) ---- */
enum { IT_TOGGLE, IT_SLIDER, IT_CHOICE, IT_ACTION };
typedef struct {
	const char *label;
	int type, val, max;
	const char *const *choices;
	const char *toast; // shown when an action is activated (optional)
	const char *note;  // IT_ACTION: status text shown before the arrow (optional)
} item_t;
typedef struct {
	item_t *it;
	int n, sel;
	float y, flash; // animated cursor row, activation flash
} list_t;
// Moves the cursor and changes values (with sounds). Returns the index of an
// IT_ACTION item activated with A, or -1.
int list_update(list_t *l, unsigned pressed, float dt);
void list_draw(canvas *c, const list_t *l, int x, int y, int w, int rh, uint32_t col);

/* ---- widget kit (ui.c): the building blocks of every page ---- */
#define UI_MX 16   // side margin
#define UI_TOP 84  // content top under a page header
#define UI_ROW 50  // selectable row pitch
#define UI_INFO 34 // info row pitch
#define UI_TS 3    // text scale of rows and info
int ui_page_header(canvas *c, int app, const char *title, const char *sub); // returns content top
int ui_section(canvas *c, int y, const char *title);                       // returns y below it
void ui_column(int x, int w); // rows/info drawn in this column (w <= 0: full width)
uint32_t ui_row(canvas *c, int y, const char *label, int sel, uint32_t col);
void ui_row_value(canvas *c, int y, const char *value, int arrow, int sel, uint32_t col, uint32_t vcol);
void ui_row_toggle(canvas *c, int y, int on, int sel);
void ui_row_hold(canvas *c, int y, const char *label, int sel, uint32_t col, float progress);
void ui_row_slider(canvas *c, int y, int val, int max, int sel, uint32_t col);
void ui_row_choice(canvas *c, int y, const char *value, int sel, uint32_t col);
void ui_info(canvas *c, int y, const char *label, const char *value);
int ui_panel(canvas *c, int x, int y, int w, int h, const char *title, uint32_t col);
void ui_scroll_arrow(canvas *c, int x, int y, int dir, uint32_t col); // more rows that way (-1 up, 1 down)

/* ---- pclink.c: PC LINK (the web app) ---- */
extern const char *const ICON_PCLINK24[ICON_MAX];
void pclink_open(void);
int pclink_update(unsigned pressed, float dt); // 1: leave the page
void pclink_draw(canvas *c);
void pclink_hints(canvas *c, int *x);

/* ---- games.c: games hub, running games, Control Center ---- */
void games_init(void);
int games_running(void);
void games_update(int screen, unsigned pressed, unsigned held, float dt);
void games_poll(int screen, float dt);
int games_rumble(int screen); // 0..100: the motor strength the game on screen wants // picks up games added/changed/removed in the library
void games_draw(canvas *c, int screen);
void games_hints(canvas *c, int screen, int *x);
void games_set_style(int hub, int cc); // layout variations (see games.c)
void games_hub_focus_first(void);       // select the most recently played game

/* ---- net.c: Wi-Fi and SSH settings pages ---- */
void net_open(int page);
int net_update(int page, unsigned pressed, float dt); // returns 1 to go back
void net_draw(canvas *c, int page);
void net_hints(canvas *c, int page, int *x);
/* ---- clock.c: Settings > CLOCK (automatic / date / time / time zone) ---- */
const char *clock_note(void); // "19:48"
void clock_open(void);
int clock_update(unsigned pressed, float dt); // 1: leave the page
void clock_draw(canvas *c);
void clock_hints(canvas *c, int *x);

const char *net_wifi_note(void);
const char *net_ssh_note(void);
// Menu button gestures and the Control Center overlay
void games_menu_tap(void);
void games_menu_double(void);
int games_cc_active(void);
void games_cc_close(void);
void games_cc_update(unsigned pressed, float dt);
void games_cc_draw(canvas *c);
// Home carousel game tiles. A reference >= 0 is an open game; < 0 is the last
// played game (shown when nothing is open).
int games_home_refs(int *refs);
void games_ref_tile(canvas *c, int ref, int x, int y, int size, int dim);
const char *games_ref_title(int ref);
uint32_t games_ref_col(int ref);
void games_ref_status(int ref, char *buf, int n);
void games_open_pill(canvas *c, int cx, int y, int ri);
void games_ref_open(int ref, int x, int y);
void games_ref_close(int ref);
// SAVES (choose, copy, delete a game's saves): an overlay from the game list,
// home, or the in-game menu
int games_pick_active(void);
void games_pick_update(unsigned pressed, float dt);
void games_pick_draw(canvas *c);
int games_ref_has_saves(int ref);
void games_ref_saves(int ref);
void games_ref_draw_bg(canvas *c, int ref); // full-screen dimmed game frame

#endif
