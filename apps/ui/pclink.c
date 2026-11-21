// PC LINK: the web app (apps/web) that manages games and saves from a browser.
// It runs only while this page is open: entering starts it, leaving stops it.
// Browsers paired before just connect; a new one pairs from here:
//   USB   wait for the cable to be plugged into a computer, then the browser
//         at the USB address pairs by itself
//   Wi-Fi needs Wi-Fi to be connected, then shows a code (new every time)
// Backed by sys.h (sys_web_*); mocked on the PC.
#include "ui_priv.h"

#include "sys.h"

#include <stdio.h>
#include <string.h>

// 24-cell icon: a laptop and the console joined by a cable (chosen 2026-11-21)
const char *const ICON_PCLINK24[ICON_MAX] = {
	"........................",
	"........................",
	"........................",
	"........................",
	"...............llllllll.",
	"...............lddddddl.",
	"wwwwwwwwwww....ldccccdl.",
	"wccccccccccw...ldccccdl.",
	"wccccccccccw...ldccccdl.",
	"wccccccccccw...lddddddl.",
	"wccccccccccw...llllllll.",
	"wccccccccccw...llllllll.",
	"wccccccccccw...llllllrl.",
	"wwwwwwwwwwww...lddllrll.",
	"llllllllllll...llllllll.",
	"llllllllllll...llllllll.",
	"......y........llllllll.",
	"......y....y.y.llllllll.",
	"......y........llllllll.",
	"......y...........y.....",
	"......yyyyyyyyyyyyy.....",
	"........................",
	"........................",
	"........................",
};

enum { V_MAIN, V_USB, V_WIFI, V_DONE };	// what the page shows
enum { ROW_USB, ROW_WIFI, ROW_FORGET, NROWS };

static struct {
	int view, sel;
	float t, hold;    // time in the view; FORGET: A held
	int held_done, pairs0;
} L;

static void set_view(int v)
{
	L.view = v, L.t = 0;
	L.pairs0 = sys_web_pairs();
}

void pclink_open(void)
{
	memset(&L, 0, sizeof L);
	sys_web_open();
	set_view(V_MAIN);
}

static int wifi_ip(const char **ip)
{
	return sys_wifi_status(NULL, ip) == WIFI_CONNECTED;
}

// Returns 1 to leave the page (the web app stops).
int pclink_update(unsigned p, float dt)
{
	L.t += dt;
	int pairing = L.view == V_USB && sys_usb_connected() ? WEB_PAIR_USB
		    : L.view == V_WIFI && wifi_ip(NULL)        ? WEB_PAIR_WIFI
							   : WEB_PAIR_NONE;
	sys_web_tick(pairing);

	if (L.view == V_DONE) {	// "PAIRED": back to the page after a moment
		if (L.t > 1.6f || (p & (BTN_A | BTN_B)))
			set_view(V_MAIN);
		return 0;
	}
	if (L.view != V_MAIN) {
		if (pairing && sys_web_pairs() > L.pairs0) {	// a browser paired
			sfx_play(SFX_CONFIRM, 0);
			set_view(V_DONE);
		} else if (p & BTN_B) {
			sfx_play(SFX_BACK, 0);
			set_view(V_MAIN);
		}
		return 0;
	}

	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		sys_web_close();
		return 1;
	}
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (d) {
		int s = L.sel + d;
		if (s >= 0 && s < NROWS)
			L.sel = s, L.hold = 0, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (L.sel == ROW_USB && (p & BTN_A)) {
		sfx_play(SFX_OPEN, APP_PCLINK);
		set_view(V_USB);
	} else if (L.sel == ROW_WIFI && (p & BTN_A)) {
		sfx_play(SFX_OPEN, APP_PCLINK);
		set_view(V_WIFI);
	} else if (L.sel == ROW_FORGET) {	// hold A: every browser has to pair again
		if ((ui_held() & BTN_A) && !L.held_done && sys_web_browsers()) {
			L.hold += dt;
			if (L.hold >= 1.0f) {
				sys_web_forget_browsers();
				sfx_play(SFX_TOGGLE_OFF, 0);
				toast("BROWSERS FORGOTTEN");
				L.held_done = 1, L.hold = 0;
			}
		} else if (!(ui_held() & BTN_A)) {
			L.hold = 0, L.held_done = 0;
		}
	}
	return 0;
}

// One address in a box: the URL on the left, what it is on the right.
static int address_box(canvas *c, int y, const char *url, const char *tag, int ok)
{
	rrect(c, UI_MX, y + 2, W - 2 * UI_MX, 42, 6, 0x0c0c0c);
	text(c, UI_MX + 16, y + 13, url, UI_TS, ok ? PAL[3] : GREY);
	if (tag)
		text(c, W - UI_MX - 16 - text_width(tag, 2), y + 16, tag, 2, GREY);
	return y + 48;
}

static void center(canvas *c, int y, const char *s, int scale, uint32_t col)
{
	text(c, (W - text_width(s, scale)) / 2, y, s, scale, col);
}

// "WAITING..." with animated dots
static void waiting(canvas *c, int y, const char *what)
{
	char s[48];
	int dots = (int)(L.t * 2.5f) % 4;
	snprintf(s, sizeof s, "%s%.*s%*s", what, dots, "...", 3 - dots, "");
	center(c, y, s, 2, GREY);
}

static const char *usb_url(void)
{
	static char u[40];
	snprintf(u, sizeof u, "http://%s", sys_usb_ip()[0] ? sys_usb_ip() : "10.55.0.2");
	return u;
}

void pclink_draw(canvas *c)
{
	uint32_t col = PAL[APP_PCLINK];
	const char *ip;
	int wifi = wifi_ip(&ip);
	char wurl[40];
	snprintf(wurl, sizeof wurl, "http://%s", wifi ? ip : "");

	if (L.view == V_USB) {
		int y = ui_page_header(c, APP_PCLINK, "PAIR OVER USB", NULL);
		if (!sys_usb_connected()) {
			center(c, y + 40, "PLUG THE USB CABLE", UI_TS, WHITE);
			center(c, y + 76, "INTO YOUR COMPUTER", UI_TS, WHITE);
			waiting(c, y + 140, "WAITING FOR THE CABLE");
		} else {
			center(c, y + 20, "ON YOUR COMPUTER, OPEN", UI_TS, WHITE);
			address_box(c, y + 60, usb_url(), "USB", 1);
			center(c, y + 132, "IN A BROWSER. NO CODE NEEDED.", 2, GREY);
			waiting(c, y + 190, "WAITING FOR THE BROWSER");
		}
		return;
	}
	if (L.view == V_WIFI) {
		int y = ui_page_header(c, APP_PCLINK, "PAIR OVER WI-FI", NULL);
		if (!wifi) {
			center(c, y + 40, "WI-FI IS NOT CONNECTED", UI_TS, WHITE);
			center(c, y + 90, "CONNECT IN SETTINGS > WI-FI,", 2, GREY);
			center(c, y + 118, "THEN COME BACK HERE.", 2, GREY);
			return;
		}
		center(c, y + 10, "ON YOUR COMPUTER OR PHONE, OPEN", 2, WHITE);
		address_box(c, y + 36, wurl, "WI-FI", 1);
		center(c, y + 104, "AND ENTER THIS CODE", 2, WHITE);
		const char *code = sys_web_code();
		char shown[16];
		if (strlen(code) == 6)
			snprintf(shown, sizeof shown, "%.3s %.3s", code, code + 3);
		else
			snprintf(shown, sizeof shown, "...");
		text_arcade(c, (W - text_width(shown, 7)) / 2, y + 134, shown, 7, WHITE);
		waiting(c, y + 210, "WAITING FOR THE BROWSER");
		return;
	}
	if (L.view == V_DONE) {
		int y = ui_page_header(c, APP_PCLINK, "PC LINK", NULL);
		text_arcade(c, (W - text_width("PAIRED!", 6)) / 2, y + 90, "PAIRED!", 6, PAL[3]);
		return;
	}

	// main: the web app is on while this page is open
	int y = ui_page_header(c, APP_PCLINK, "PC LINK", NULL);
	y = ui_section(c, y, sys_web_running() ? "ON WHILE THIS PAGE IS OPEN" : "STARTING...");
	y = address_box(c, y, wifi ? wurl : "NOT ON WI-FI", "WI-FI", wifi);
	y = address_box(c, y, usb_url(), "USB", 1);
	y = ui_section(c, y + 4, "NEW BROWSER");
	ui_row(c, y, "PAIR OVER USB", L.sel == ROW_USB, col);
	ui_row_value(c, y, "", 1, L.sel == ROW_USB, col, GREY);
	y += UI_ROW;
	ui_row(c, y, "PAIR OVER WI-FI", L.sel == ROW_WIFI, col);
	ui_row_value(c, y, "", 1, L.sel == ROW_WIFI, col, GREY);
	y += UI_ROW;
	int n = sys_web_browsers();
	char lbl[40];
	snprintf(lbl, sizeof lbl, n ? "FORGET %d BROWSER%s" : "NO PAIRED BROWSERS", n, n == 1 ? "" : "S");
	ui_row_hold(c, y, lbl, L.sel == ROW_FORGET, PAL[0], L.hold);
}

void pclink_hints(canvas *c, int *x)
{
	if (L.view != V_MAIN) {
		draw_hint(c, x, "B", "BACK", PAL[0]);
		return;
	}
	draw_hint(c, x, "^V", "MOVE", 0xdddddd);
	draw_hint(c, x, "A", L.sel == ROW_FORGET ? "HOLD" : "SELECT", PAL[3]);
	draw_hint(c, x, "B", "CLOSE", PAL[0]);
}
