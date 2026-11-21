// PC LINK: the web app (apps/web) that manages games and saves from a browser.
// A home item; the page switches it on/off and shows how to connect: the
// address over Wi-Fi (needs the pairing code) and over USB (pairs by itself).
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

enum { ROW_SWITCH, ROW_NEWCODE, ROW_FORGET };

static struct {
	int sel;
	float hold; // FORGET BROWSERS: hold A
	int held_done;
} L;

void pclink_open(void)
{
	memset(&L, 0, sizeof L);
}

static int nrows(void)
{
	return sys_web_enabled() ? 3 : 1;
}

// Returns 1 to leave the page.
int pclink_update(unsigned p, float dt)
{
	if (L.sel >= nrows())
		L.sel = nrows() - 1;
	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		return 1;
	}
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (d) {
		int s = L.sel + d;
		if (s >= 0 && s < nrows())
			L.sel = s, L.hold = 0, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (L.sel == ROW_SWITCH && (p & (BTN_A | BTN_LEFT | BTN_RIGHT))) {
		int on = !sys_web_enabled();
		sys_web_enable(on);
		sfx_play(on ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF, 0);
		toast(on ? "PC LINK ON" : "PC LINK OFF");
	} else if (L.sel == ROW_NEWCODE && (p & BTN_A)) {
		sys_web_new_code();
		sfx_play(SFX_CONFIRM, 0);
		toast("NEW CODE: THE OLD ONE NO LONGER WORKS");
	} else if (L.sel == ROW_FORGET) {	// hold A: every browser has to pair again
		if ((ui_held() & BTN_A) && !L.held_done) {
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

void pclink_draw(canvas *c)
{
	uint32_t col = PAL[APP_PCLINK];
	int y = ui_page_header(c, APP_PCLINK, "PC LINK", NULL);
	int on = sys_web_enabled();

	ui_row(c, y, "PC LINK", L.sel == ROW_SWITCH, col);
	ui_row_toggle(c, y, on, L.sel == ROW_SWITCH);
	y += UI_ROW;
	if (!on) {
		y = ui_section(c, y + 6, "WHAT IT DOES");
		text(c, UI_MX + 16, y + 8, "MANAGE GAMES AND SAVES", UI_TS, GREY);
		text(c, UI_MX + 16, y + 40, "FROM A BROWSER ON YOUR PC", UI_TS, GREY);
		text(c, UI_MX + 16, y + 72, "OR PHONE.", UI_TS, GREY);
		return;
	}

	// where to go: one box per way in, the same style for both
	y = ui_section(c, y + 6, "OPEN IN A BROWSER");
	const char *ip;
	int wifi = sys_wifi_status(NULL, &ip) == WIFI_CONNECTED;
	char wurl[64], uurl[64];
	snprintf(wurl, sizeof wurl, "http://%s", ip);
	snprintf(uurl, sizeof uurl, "http://%s", sys_usb_ip()[0] ? sys_usb_ip() : "10.55.0.2");
	const struct { const char *url, *tag; int ok; } way[2] = {
		{wifi ? wurl : "NOT ON WI-FI", "WI-FI", wifi},
		{uurl, "USB - NO CODE", 1},
	};
	for (int i = 0; i < 2; i++) {
		rrect(c, UI_MX, y + 2, W - 2 * UI_MX, 42, 6, 0x0c0c0c);
		text(c, UI_MX + 16, y + 13, way[i].url, UI_TS, way[i].ok ? PAL[3] : GREY);
		text(c, W - UI_MX - 16 - text_width(way[i].tag, 2), y + 16, way[i].tag, 2, GREY);
		y += 48;
	}
	y += 2;

	// pairing
	y = ui_section(c, y + 6, "PAIRING");
	const char *code = sys_web_code();
	char shown[16];
	if (strlen(code) == 6)
		snprintf(shown, sizeof shown, "%.3s %.3s", code, code + 3);
	else
		snprintf(shown, sizeof shown, "...");
	ui_info(c, y, wifi ? "CODE (WI-FI)" : "CODE", shown);
	y += UI_INFO;
	ui_row(c, y, "NEW CODE", L.sel == ROW_NEWCODE, col);
	y += UI_ROW;
	int n = sys_web_browsers();
	char lbl[40];
	snprintf(lbl, sizeof lbl, n ? "FORGET %d BROWSER%s" : "NO PAIRED BROWSERS", n, n == 1 ? "" : "S");
	ui_row_hold(c, y, lbl, L.sel == ROW_FORGET, PAL[0], L.hold);
}

void pclink_hints(canvas *c, int *x)
{
	draw_hint(c, x, "^V", "MOVE", 0xdddddd);
	draw_hint(c, x, "A", L.sel == ROW_FORGET ? "HOLD" : "SELECT", PAL[3]);
	draw_hint(c, x, "B", "BACK", PAL[0]);
}
