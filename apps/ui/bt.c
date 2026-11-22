// Settings > BLUETOOTH: on/off, make visible (pair a phone from its side),
// paired devices; a device's sheet: use its internet (phone tethering), forget.
// The work happens in cozisboy-bt (sys_bt_*), in the background.

#include "ui_priv.h"
#include "sys.h"

#include <stdio.h>
#include <string.h>

#define HOLD 0.9f

static struct {
	int sel;
	int sheet, act;	// a device's actions are open
	char mac[18];	// that device
	float hold;
	int hold_done;
	int ndev_seen;	// to notice a newly paired device
} BT;

enum { R_POWER, R_VISIBLE, R_DEV0 };	// R_DEV0.. one row per paired device
enum { A_NET, A_FORGET };

const char *bt_note(void)
{
	if (!sys_bt_known())
		return NULL;
	if (!sys_bt_powered())
		return "OFF";
	btdev_t d[16];
	int n = sys_bt_devices(d, 16);
	for (int i = 0; i < n; i++)
		if (d[i].internet)
			return "INTERNET";
	return "ON";
}

void bt_open(void)
{
	memset(&BT, 0, sizeof BT);
	BT.ndev_seen = -1;
}

static int rows(int ndev)
{
	return sys_bt_powered() ? R_DEV0 + ndev : 1;
}

static int sheet_actions(const btdev_t *d, int *a)
{
	int n = 0;
	if (d->nap || d->internet)
		a[n++] = A_NET;
	a[n++] = A_FORGET;
	return n;
}

static const btdev_t *find_dev(const btdev_t *d, int n, const char *mac)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(d[i].mac, mac))
			return &d[i];
	return NULL;
}

int bt_update(unsigned p, float dt)
{
	sys_bt_tick(dt, 1);
	const char *r = sys_bt_result();
	if (r)
		toast(r);
	btdev_t d[16];
	int n = sys_bt_devices(d, 16);
	// a phone just paired while visible
	if (BT.ndev_seen >= 0 && n > BT.ndev_seen && sys_bt_visible()) {
		static char msg[80];
		snprintf(msg, sizeof msg, "PAIRED WITH %s", d[n - 1].name);
		toast(msg);
		sfx_play(SFX_CONFIRM, 0);
	}
	if (sys_bt_known())
		BT.ndev_seen = n;

	if (BT.sheet) {
		const btdev_t *dev = find_dev(d, n, BT.mac);
		if (!dev) { // forgotten meanwhile
			BT.sheet = 0;
			return 0;
		}
		int a[4], na = sheet_actions(dev, a);
		if (BT.act >= na)
			BT.act = na - 1;
		int dd = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
		if (dd) {
			BT.hold = 0;
			if (ui_held() & BTN_A)
				BT.hold_done = 1;
			if (BT.act + dd >= 0 && BT.act + dd < na)
				BT.act += dd, sfx_play(SFX_TICK, 0);
			else
				sfx_play(SFX_BUMP, 0);
		}
		if (p & BTN_B) {
			BT.sheet = 0;
			sfx_play(SFX_BACK, 0);
			return 0;
		}
		if (a[BT.act] == A_FORGET) { // hold A: it loses the pairing
			if ((ui_held() & BTN_A) && !BT.hold_done) {
				if ((BT.hold += dt) >= HOLD) {
					sys_bt_forget(dev->mac);
					sfx_play(SFX_TOGGLE_OFF, 0);
					BT.sheet = 0, BT.hold = 0, BT.hold_done = 1;
				}
			} else if (!(ui_held() & BTN_A)) {
				BT.hold = 0, BT.hold_done = 0;
			}
			return 0;
		}
		if (p & BTN_A) {
			if (sys_bt_busy()) {
				toast("WAIT A MOMENT");
				return 0;
			}
			sys_bt_internet(dev->mac, !dev->internet);
			toast(dev->internet ? "STOPPING..." : "CONNECTING TO THE PHONE...");
			sfx_play(SFX_CONFIRM, 0);
			BT.sheet = 0;
		}
		return 0;
	}

	int nr = rows(n);
	if (BT.sel >= nr)
		BT.sel = nr - 1;
	int dd = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (dd) {
		if (BT.sel + dd >= 0 && BT.sel + dd < nr)
			BT.sel += dd, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		return 1;
	}
	if (BT.sel == R_POWER && (p & (BTN_A | BTN_LEFT | BTN_RIGHT))) {
		if (sys_bt_busy())
			return 0;
		int on = !sys_bt_powered();
		sys_bt_power(on);
		sfx_play(on ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF, 0);
		return 0;
	}
	if (!(p & BTN_A))
		return 0;
	if (BT.sel == R_VISIBLE) {
		if (!sys_bt_visible())
			sys_bt_make_visible();
		sfx_play(SFX_OPEN, APP_SETTINGS);
	} else if (BT.sel >= R_DEV0 && BT.sel - R_DEV0 < n) {
		snprintf(BT.mac, sizeof BT.mac, "%s", d[BT.sel - R_DEV0].mac);
		BT.sheet = 1, BT.act = 0, BT.hold = 0, BT.hold_done = 1;
		sfx_play(SFX_OPEN, APP_SETTINGS);
	}
	return 0;
}

void bt_draw(canvas *c)
{
	uint32_t col = PAL[APP_SETTINGS];
	int y = ui_page_header(c, APP_SETTINGS, "BLUETOOTH", NULL);
	btdev_t d[16];
	int n = sys_bt_devices(d, 16), on = sys_bt_powered(), vis = sys_bt_visible();

	ui_row(c, y, "BLUETOOTH", BT.sel == R_POWER, col);
	ui_row_toggle(c, y, on, BT.sel == R_POWER);
	y += UI_ROW;
	if (!on) {
		text(c, UI_MX + 16, y + 10, "TURN IT ON TO PAIR A PHONE.", 2, GREY);
		return;
	}
	char v[24] = "";
	if (vis)
		snprintf(v, sizeof v, "%d:%02d", vis / 60, vis % 60);
	ui_row(c, y, vis ? "VISIBLE" : "MAKE VISIBLE", BT.sel == R_VISIBLE, col);
	ui_row_value(c, y, vis ? v : NULL, !vis, BT.sel == R_VISIBLE, col, PAL[3]);
	y += UI_ROW;

	y = ui_section(c, y + 4, "PAIRED DEVICES");
	if (!n)
		text(c, UI_MX + 16, y + 10, sys_bt_known() ? "NONE YET" : "...", UI_TS, 0x666666);
	for (int i = 0; i < n; i++) {
		int sel = BT.sel == R_DEV0 + i, ry = y + i * UI_ROW;
		ui_row(c, ry, d[i].name, sel, col);
		ui_row_value(c, ry, d[i].internet ? "INTERNET" : d[i].connected ? "CONNECTED" : NULL, 1, sel, col,
			     d[i].internet || d[i].connected ? PAL[3] : GREY);
	}
	y += (n ? n : 1) * UI_ROW + 6;

	// what to do next, or where it is
	const char *ip = sys_bt_ip();
	char line[96];
	if (vis)
		snprintf(line, sizeof line, "ON THE PHONE: BLUETOOTH > COZISBOY, THEN PAIR");
	else if (ip[0])
		snprintf(line, sizeof line, "ON THE PHONE'S NETWORK: %s", ip);
	else
		snprintf(line, sizeof line, "OTHERS SEE IT AS COZISBOY");
	text(c, UI_MX + 16, y, line, 2, vis ? WHITE : GREY);

	if (BT.sheet) { // a device's actions
		const btdev_t *dev = find_dev(d, n, BT.mac);
		if (!dev)
			return;
		int a[4], na = sheet_actions(dev, a);
		const int ph = 52 + na * UI_ROW + 8, py = H - 46 - ph;
		int ry = ui_panel(c, UI_MX - 8, py, W - 2 * UI_MX + 16, ph, dev->name, col);
		for (int k = 0; k < na; k++) {
			int sel = k == BT.act, yy = ry + k * UI_ROW;
			if (a[k] == A_FORGET)
				ui_row_hold(c, yy, "FORGET", sel, PAL[0], sel ? BT.hold / HOLD : 0);
			else
				ui_row(c, yy, dev->internet ? "STOP USING ITS INTERNET" : "USE ITS INTERNET", sel, col);
		}
	}
}

void bt_hints(canvas *c, int *x)
{
	if (BT.sheet) {
		draw_hint(c, x, "^V", "MOVE", 0xdddddd);
		draw_hint(c, x, "A", "OK", PAL[3]);
		draw_hint(c, x, "B", "BACK", PAL[0]);
		return;
	}
	draw_hint(c, x, "^V", "MOVE", 0xdddddd);
	draw_hint(c, x, "A", BT.sel == R_POWER ? "TOGGLE" : BT.sel == R_VISIBLE ? "MAKE VISIBLE" : "OPEN", PAL[3]);
	draw_hint(c, x, "B", "BACK", PAL[0]);
}
