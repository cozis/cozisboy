// PC LINK: the web app (apps/web) that manages games and saves from a browser.
// The page opens on a choice (two cards), USB or Wi-Fi; hold X there forgets
// every paired browser. The web app runs only while the
// USB or the Wi-Fi screen is open, and lets in only browsers coming that way
// (cable or Wi-Fi). Browsers paired before just connect; new ones pair there:
//   USB   waits for the cable to be plugged into a computer; then any browser
//         on the cable pairs by itself
//   Wi-Fi needs Wi-Fi to be connected; new browsers type the code shown
//         (new every time the screen opens)
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

// 24-cell icons for the two choices
static const char *const ICON_USB24[ICON_MAX] = {
	"........................",
	"........................",
	"........llllllll........",
	"........lddddddl........",
	"........lddddddl........",
	"........llllllll........",
	"........lldlldll........",
	"........lldlldll........",
	"........llllllll........",
	"......wwwwwwwwwwww......",
	"......wwwwwwwwwwww......",
	"......wwggggggggww......",
	"......wwggccccggww......",
	"......wwggccccggww......",
	"......wwggccccggww......",
	"......wwggggggggww......",
	"......wwwwwwwwwwww......",
	"......wwwwwwwwwwww......",
	"..........dddd..........",
	"..........dddd..........",
	"...........dd...........",
	"...........dd...........",
	"...........dd...........",
	"...........dd...........",
};
static const char *const ICON_WIFI24[ICON_MAX] = {
	"........................",
	"........................",
	"........................",
	".......wwwwwwwwww.......",
	".....wwwwwwwwwwwwww.....",
	"...wwww..........wwww...",
	"..www..............www..",
	".ww..................ww.",
	".w....................w.",
	"........wwwwwwww........",
	"......wwwwwwwwwwww......",
	".....www........www.....",
	"....ww............ww....",
	"....w..............w....",
	"........................",
	".........wwwwww.........",
	"........wwwwwwww........",
	".......ww......ww.......",
	".......w........w.......",
	"........................",
	"..........yyyy..........",
	"..........yyyy..........",
	"..........yyyy..........",
	"........................",
};

enum { V_CHOOSE, V_USB, V_WIFI };	// what the page shows
enum { CH_USB, CH_WIFI, NCH };

// Layout: content sits in a column with wide side margins, centred vertically
// between the page header and the hint bar.
#define PX0 56              // side margin
#define PW (W - 2 * PX0)    // column width
#define CONTENT_BOTTOM (H - 52)

static struct {
	int view, sel;
	float t, hold, paired_t; // time in the view; FORGET: A held; "PAIRED!" shown
	int held_done, pairs0;
} L;

static void set_view(int v)
{
	if (v == V_CHOOSE)
		sys_web_close();	// back to the choice: nobody gets in
	else
		sys_web_open();
	L.view = v, L.t = 0, L.paired_t = 0;
	L.pairs0 = sys_web_pairs();
}

void pclink_open(void)
{
	memset(&L, 0, sizeof L);
	set_view(V_CHOOSE);
}

static int wifi_ip(const char **ip)
{
	return sys_wifi_status(NULL, ip) == WIFI_CONNECTED;
}

// Returns 1 to leave the page (the web app stops).
int pclink_update(unsigned p, float dt)
{
	L.t += dt;
	if (L.paired_t > 0)
		L.paired_t -= dt;
	if (L.view != V_CHOOSE) {	// keeps the web app running, open to this way only
		int pairing = L.view == V_USB && sys_usb_connected() ? WEB_PAIR_USB
			    : L.view == V_WIFI && wifi_ip(NULL)        ? WEB_PAIR_WIFI
								   : WEB_PAIR_NONE;
		sys_web_tick(pairing);
	}

	if (L.view != V_CHOOSE) {
		if (sys_web_pairs() > L.pairs0) {	// a browser paired: say so, stay here
			L.pairs0 = sys_web_pairs();
			L.paired_t = 2.0f;
			sfx_play(SFX_CONFIRM, 0);
		}
		if (p & BTN_B) {
			sfx_play(SFX_BACK, 0);
			set_view(V_CHOOSE);
		}
		return 0;
	}

	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		return 1;
	}
	int d = (p & (BTN_DOWN | BTN_RIGHT)) ? 1 : (p & (BTN_UP | BTN_LEFT)) ? -1 : 0;
	if (d) {	// two cards: one is always selected
		int s = L.sel + d;
		if (s >= 0 && s < NCH)
			L.sel = s, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (p & BTN_A) {
		sfx_play(SFX_OPEN, APP_PCLINK);
		set_view(L.sel == CH_USB ? V_USB : V_WIFI);
	}
	// hold X: every browser has to pair again
	if ((ui_held() & BTN_X) && !L.held_done && sys_web_browsers()) {
		L.hold += dt;
		if (L.hold >= 1.0f) {
			sys_web_forget_browsers();
			sfx_play(SFX_TOGGLE_OFF, 0);
			toast("BROWSERS FORGOTTEN");
			L.held_done = 1, L.hold = 0;
		}
	} else if (!(ui_held() & BTN_X)) {
		L.hold = 0, L.held_done = 0;
	}
	return 0;
}

// One address in a box: the URL on the left, what it is on the right.
static int address_box(canvas *c, int y, const char *url, const char *tag)
{
	int sc = 3;
	rrect(c, PX0, y, PW, 48, 6, 0x0c0c0c);
	text(c, PX0 + 18, y + 24 - 7 * sc / 2, url, sc, PAL[3]);
	if (tag)
		text(c, PX0 + PW - 18 - text_width(tag, 2), y + 17, tag, 2, GREY);
	return y + 48;
}

static void center(canvas *c, int y, const char *s, int scale, uint32_t col)
{
	text(c, (W - text_width(s, scale)) / 2, y, s, scale, col);
}

// A numbered step label: a coloured number chip, then the text.
static void step(canvas *c, int y, int n, const char *s)
{
	char num[4];
	snprintf(num, sizeof num, "%d", n);
	rrect(c, PX0, y, 24, 24, 4, PAL[APP_PCLINK]);
	text(c, PX0 + 12 - text_width(num, 2) / 2, y + 5, num, 2, BLACK);
	text(c, PX0 + 36, y + 5, s, 2, WHITE);
}

// "WAITING..." with animated dots
static void waiting(canvas *c, int y, const char *what)
{
	char s[48];
	int dots = (int)(L.t * 2.5f) % 4;
	snprintf(s, sizeof s, "%s%.*s%*s", what, dots, "...", 3 - dots, "");
	center(c, y, s, 2, GREY);
}

// Bottom status: "PAIRED!" for a moment, else waiting + how many are paired.
static void status_line(canvas *c, int y)
{
	if (L.paired_t > 0) {
		text_arcade(c, (W - text_width("PAIRED!", 3)) / 2, y - 4, "PAIRED!", 3, PAL[3]);
		return;
	}
	int n = sys_web_browsers();
	char s[48];
	snprintf(s, sizeof s, n ? "WAITING FOR A BROWSER - %d PAIRED" : "WAITING FOR A BROWSER", n);
	waiting(c, y, s);
}

static const char *usb_url(void)
{
	static char u[40];
	snprintf(u, sizeof u, "http://%s", sys_usb_ip()[0] ? sys_usb_ip() : "10.55.0.2");
	return u;
}

static void big_icon(canvas *c, const char *const *rows, int x, int y, int scale, uint32_t detail)
{
	icon_t ic;
	icon_from_rows(&ic, rows, 24);
	draw_icon(c, &ic, x, y, scale, WHITE, detail, BLACK);
}

// The choice widget: a big card with an icon, a title and a line under it.
// Exactly one card is selected (filled with the page colour).
static void choice_card(canvas *c, int y, int h, const char *const *icon, const char *title,
			const char *line, const char *status, uint32_t scol, int sel)
{
	uint32_t col = PAL[APP_PCLINK];
	uint32_t fillc = sel ? col : 0x222222;
	wbox(c, PX0, y, PW, h, fillc, sel ? W_SEL : W_NORMAL);
	uint32_t ink = sel ? wink(fillc, W_SEL) : WHITE;
	int is = 3, isz = 24 * is;
	big_icon(c, icon, PX0 + 24, y + (h - isz) / 2, is, sel ? mix(col, BLACK, 0.45f) : 0x444444);
	int tx = PX0 + 24 + isz + 24, ty = y + (h - 76) / 2;
	text_arcade(c, tx, ty, title, 3, sel ? WHITE : col);
	text(c, tx, ty + 40, line, 2, sel ? ink : GREY);
	if (status)
		text(c, tx, ty + 62, status, 2, sel ? ink : scol);
}

void pclink_draw(canvas *c)
{
	const char *ip, *ssid;
	int wifi = wifi_ip(&ip);
	sys_wifi_status(&ssid, NULL);

	if (L.view == V_USB) {
		int top = ui_page_header(c, APP_PCLINK, "USB CABLE", NULL);
		int y = top + (CONTENT_BOTTOM - top - (sys_usb_connected() ? 292 : 210)) / 2 - 16;
		if (!sys_usb_connected()) {	// waiting for the cable: the plug, big
			big_icon(c, ICON_USB24, (W - 24 * 4) / 2, y + 30, 4, 0x444444);
			center(c, y + 150, "PLUG THE USB CABLE INTO YOUR COMPUTER", 2, WHITE);
			waiting(c, y + 190, "WAITING FOR THE CABLE");
			return;
		}
		step(c, y + 16, 1, "ON YOUR COMPUTER, OPEN");
		address_box(c, y + 50, usb_url(), "USB");
		step(c, y + 124, 2, "NO CODE NEEDED: THE CABLE IS THE KEY");
		big_icon(c, ICON_PCLINK24, (W - 24 * 3) / 2, y + 170, 3, 0x444444);
		status_line(c, y + 270);
		return;
	}
	if (L.view == V_WIFI) {
		int top = ui_page_header(c, APP_PCLINK, "WI-FI", NULL);
		int y = top + (CONTENT_BOTTOM - top - (wifi ? 292 : 210)) / 2 - 16;
		if (!wifi) {
			big_icon(c, ICON_WIFI24, (W - 24 * 4) / 2, y + 30, 4, 0x444444);
			center(c, y + 150, "WI-FI IS NOT CONNECTED", 2, WHITE);
			center(c, y + 180, "CONNECT IN SETTINGS > WI-FI, THEN COME BACK", 2, GREY);
			return;
		}
		char wurl[40];
		snprintf(wurl, sizeof wurl, "http://%s", ip);
		step(c, y + 16, 1, "ON THE SAME WI-FI, OPEN");
		address_box(c, y + 50, wurl, ssid && *ssid ? ssid : NULL);
		step(c, y + 124, 2, "ENTER THIS CODE");
		const char *code = sys_web_code();
		char shown[16];
		if (strlen(code) == 6)
			snprintf(shown, sizeof shown, "%.3s %.3s", code, code + 3);
		else
			snprintf(shown, sizeof shown, "...");
		text_arcade(c, (W - text_width(shown, 7)) / 2, y + 170, shown, 7, WHITE);
		status_line(c, y + 270);
		return;
	}

	// the choice: two cards, one selected
	int top = ui_page_header(c, APP_PCLINK, "PC LINK", NULL), h = 112, gap = 18;
	int y = top + (CONTENT_BOTTOM - top - (2 * h + gap)) / 2;
	choice_card(c, y, h, ICON_USB24, "USB CABLE", "PLUG INTO A COMPUTER", "NO CODE NEEDED", GREY,
		    L.sel == CH_USB);
	choice_card(c, y + h + gap, h, ICON_WIFI24, "WI-FI", "COMPUTER OR PHONE",
		    wifi ? (ssid && *ssid ? ssid : "CONNECTED") : "NOT CONNECTED", wifi ? PAL[3] : GREY,
		    L.sel == CH_WIFI);
}

void pclink_hints(canvas *c, int *x)
{
	if (L.view != V_CHOOSE) {
		draw_hint(c, x, "B", "BACK", PAL[0]);
		return;
	}
	draw_hint(c, x, "^V", "CHOOSE", 0xdddddd);
	draw_hint(c, x, "A", "OPEN", PAL[3]);
	int n = sys_web_browsers();
	if (n) {
		char s[32];
		snprintf(s, sizeof s, L.hold > 0 ? "FORGETTING..." : "HOLD: FORGET %d", n);
		draw_hint(c, x, "X", s, PAL[4]);
	}
	draw_hint(c, x, "B", "CLOSE", PAL[0]);
}
