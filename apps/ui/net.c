// Settings pages for Wi-Fi and SSH, plus the on-screen keyboard used to type
// Wi-Fi passwords. Backed by sys.h (mocked on the PC).
#include "ui_priv.h"

#include "sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- rows ------------------------------------------------------------------- */

enum { R_TOGGLE, R_INFO, R_NET, R_ACTION };
enum { A_WIFI, A_SSH, A_SSHPW, A_SCAN, A_ROOTPW };

typedef struct {
	int kind, id; // id: A_* for toggles/actions, network index for R_NET
	const char *label;
	char value[64];
	int on;
} row_t;

#define MAXROWS 20
#define MAXNETS 12
static net_t nets[MAXNETS];
static int nnets;
static row_t rows[MAXROWS];
static int nrows;

static struct {
	int sel;
	float sel_y, scroll;
} N;

// on-screen keyboard: a Wi-Fi password, or the root password for SSH
enum { OSK_WIFI, OSK_ROOTPW };
static struct {
	int on, purpose;
	char ssid[33], buf[64];
	int len, r, c, shift, show;
	float t;
} K;

static char msg[96];

static row_t *add(int kind, int id, const char *label)
{
	row_t *r = &rows[nrows++];
	memset(r, 0, sizeof *r);
	r->kind = kind, r->id = id, r->label = label;
	return r;
}

static void build_rows(int page)
{
	nrows = 0;
	if (page == PAGE_WIFI) {
		// row 0 is the switch in the status card; the list holds only networks
		row_t *r = add(R_TOGGLE, A_WIFI, "WI-FI");
		r->on = sys_wifi_enabled();
		nnets = sys_wifi_enabled() && !sys_wifi_scanning() ? sys_wifi_scan(nets, MAXNETS) : 0;
		for (int i = 0; i < nnets && nrows < MAXROWS; i++)
			add(R_NET, i, nets[i].ssid);
	} else {
		// two rows; how to connect is drawn as info. Password login is on
		// exactly when a password is set (keys always work).
		row_t *r = add(R_TOGGLE, A_SSH, "SSH");
		r->on = sys_ssh_enabled();
		r = add(R_ACTION, A_ROOTPW, "PASSWORD");
		snprintf(r->value, sizeof r->value, "%s", sys_ssh_has_password() ? "SET" : "NOT SET");
	}
}

static int selectable(int i)
{
	return i >= 0 && i < nrows && rows[i].kind != R_INFO;
}

static void fix_sel(int dir)
{
	if (N.sel >= nrows)
		N.sel = nrows - 1;
	while (N.sel >= 0 && N.sel < nrows && !selectable(N.sel))
		N.sel += dir;
	if (N.sel < 0 || N.sel >= nrows) {
		N.sel = 0;
		while (N.sel < nrows - 1 && !selectable(N.sel))
			N.sel++;
	}
}

void net_open(int page)
{
	memset(&N, 0, sizeof N);
	K.on = 0;
	if (page == PAGE_WIFI)
		sys_wifi_rescan();
	build_rows(page);
	fix_sel(1);
	N.sel_y = (float)N.sel;
}

/* ---- keyboard ------------------------------------------------------------------- */

static const char *const KEYS[4] = {"1234567890", "qwertyuiop", "asdfghjkl-", "zxcvbnm_.@"};
static const char *const KEYS_SHIFT[4] = {"!@#$%^&*()", "QWERTYUIOP", "ASDFGHJKL+", "ZXCVBNM=,?"};
enum { K_SHIFT, K_SPACE, K_DEL, K_DONE };

static int special_at(int col)
{
	return col < 2 ? K_SHIFT : col < 6 ? K_SPACE : col < 8 ? K_DEL : K_DONE;
}

static void osk_open(int purpose, const char *ssid)
{
	memset(&K, 0, sizeof K);
	K.on = 1;
	K.purpose = purpose;
	K.r = 1;
	snprintf(K.ssid, sizeof K.ssid, "%s", ssid ? ssid : "");
}

/* ---- root password: choice popup and "write it down" popup ------------------- */
static struct {
	int choose, sel; // popup: type one / generate one
	int show;        // popup: the generated password
	char pw[20];
} P;

// 12 random characters, no look-alikes (0/O, 1/l/I), as xxxx-xxxx-xxxx.
// Returns -1 if the system's random source can't be read (no weak fallback).
static int random_password(char *out)
{
	static const char set[] = "abcdefghijkmnpqrstuvwxyz23456789"; // 32: no modulo bias
	unsigned char rnd[12];
	FILE *f = fopen("/dev/urandom", "rb");
	int ok = f && fread(rnd, 1, sizeof rnd, f) == sizeof rnd;
	if (f)
		fclose(f);
	if (!ok)
		return -1;
	int k = 0;
	for (int i = 0; i < 12; i++) {
		if (i && i % 4 == 0)
			out[k++] = '-';
		out[k++] = set[rnd[i] % (sizeof set - 1)];
	}
	out[k] = 0;
	for (size_t i = 0; i < sizeof rnd; i++)
		((volatile unsigned char *)rnd)[i] = 0;
	return 0;
}

// Overwrite a secret so it doesn't linger in memory (not optimised away).
static void wipe(void *p, size_t n)
{
	volatile unsigned char *v = p;
	while (n--)
		*v++ = 0;
}

static void set_root_password(const char *pw, int generated)
{
	if (sys_ssh_set_password(pw)) {
		toast("AT LEAST 8 CHARACTERS");
		sfx_play(SFX_BUMP, 0);
		return;
	}
	sys_ssh_set_password_login(1); // a password is for logging in with
	if (generated) {
		snprintf(P.pw, sizeof P.pw, "%s", pw);
		P.show = 1;
	} else {
		toast("ROOT PASSWORD SET");
	}
	sfx_play(SFX_CONFIRM, 0);
}

static void osk_type(char ch)
{
	if (K.len < (int)sizeof K.buf - 1) {
		K.buf[K.len++] = ch;
		K.buf[K.len] = 0;
		sfx_play(SFX_TICK, 0);
	} else {
		sfx_play(SFX_BUMP, 0);
	}
}

static void osk_done(void)
{
	K.on = 0;
	if (K.purpose == OSK_ROOTPW) {
		set_root_password(K.buf, 0);
		wipe(K.buf, sizeof K.buf), K.len = 0;
		return;
	}
	sys_wifi_connect(K.ssid, K.buf);
	wipe(K.buf, sizeof K.buf), K.len = 0;
	snprintf(msg, sizeof msg, "CONNECTING TO %s", K.ssid);
	sfx_play(SFX_CONFIRM, 0);
	toast(msg);
}

static void osk_update(unsigned p, float dt)
{
	K.t += dt;
	int dr = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	int dc = (p & BTN_RIGHT) ? 1 : (p & BTN_LEFT) ? -1 : 0;
	if (dr || dc)
		sfx_play(SFX_TICK, 0);
	K.r = (K.r + dr + 5) % 5;
	if (K.r == 4 && dc) { // special row moves key by key
		int k = (special_at(K.c) + dc + 4) % 4;
		static const int first[4] = {0, 2, 6, 8};
		K.c = first[k];
	} else {
		K.c = (K.c + dc + 10) % 10;
	}
	if (p & BTN_Y)
		K.shift = !K.shift, sfx_play(SFX_CHANGE, K.shift * 6);
	if (p & BTN_X)
		K.show = !K.show, sfx_play(SFX_CHANGE, K.show * 6);
	if (p & BTN_B) {
		if (K.len)
			K.buf[--K.len] = 0, sfx_play(SFX_TOGGLE_OFF, 0);
		else
			K.on = 0, sfx_play(SFX_BACK, 0); // B on an empty field cancels
	}
	if (p & BTN_SELECT) {
		K.on = 0, sfx_play(SFX_BACK, 0);
		wipe(K.buf, sizeof K.buf), K.len = 0;
	}
	if (p & BTN_START)
		osk_done();
	if (p & BTN_A) {
		if (K.r < 4) {
			osk_type((K.shift ? KEYS_SHIFT : KEYS)[K.r][K.c]);
		} else {
			switch (special_at(K.c)) {
			case K_SHIFT: K.shift = !K.shift, sfx_play(SFX_CHANGE, K.shift * 6); break;
			case K_SPACE: osk_type(' '); break;
			case K_DEL:
				if (K.len)
					K.buf[--K.len] = 0;
				sfx_play(SFX_TOGGLE_OFF, 0);
				break;
			case K_DONE: osk_done(); break;
			}
		}
	}
}

/* Key styles being compared: 0 boxes (widget style), 1 flat, 2 no keys, 3 plate */
static int osk_style = 1; // flat, chosen 2026-11-21
#define KEY_BASE 0x262626u

void net_set_osk_style(int st)
{
	osk_style = st;
}

static void osk_key(canvas *c, int x, int y, int w, int h, const char *label, int sel,
		    uint32_t base, uint32_t col)
{
	uint32_t f = sel ? col : base;
	int lift = 0;
	switch (osk_style) {
	case 0:
		wbox(c, x, y, w, h, f, sel ? W_SEL : W_NORMAL);
		lift = wstyle == 2 && sel ? 3 : 0;
		break;
	case 1: // flat rounded keys, no outlines
		if (sel)
			rrect(c, x - 3, y - 3, w + 6, h + 6, 6, WHITE);
		rrect(c, x, y, w, h, 4, f);
		break;
	case 2: // just characters; cursor block behind the selection
		if (sel || base != KEY_BASE)
			rrect(c, x, y, w, h, 4, f);
		break;
	case 3: // plate (drawn by the caller); selection fills the cell
		if (sel || base != KEY_BASE)
			rrect(c, x + 1, y + 1, w - 2, h - 2, 3, f);
		break;
	}
	int light = ((f >> 16 & 255) * 3 + (f >> 8 & 255) * 6 + (f & 255)) / 10 > 110;
	uint32_t ink = osk_style == 0 ? wink(f, sel ? W_SEL : W_NORMAL) : light ? BLACK : 0xdddddd;
	text(c, x + (w - text_width(label, 2)) / 2, y + (h - 14) / 2 - lift, label, 2, ink);
}

static void osk_draw(canvas *c)
{
	uint32_t col = PAL[APP_WIFI];
	darken(c, 0, 0, W, H, 0.6f);
	fill(c, 0, 150, W, H - 150, 0x101010);
	for (int i = 0; i < 8; i++)
		fill(c, i * W / 8, 150, W / 8 + 1, 3, PAL[i]);
	char title[64];
	if (K.purpose == OSK_ROOTPW)
		snprintf(title, sizeof title, "NEW ROOT PASSWORD (8+ CHARACTERS)");
	else
		snprintf(title, sizeof title, "PASSWORD FOR %s", K.ssid);
	text(c, 30, 164, title, 2, 0xcccccc);
	// field
	wframe(c, 30, 186, W - 60, 34, 0x555555);
	fill(c, 30, 186, W - 60, 34, 0x1a1a1a);
	char shown[64];
	for (int i = 0; i <= K.len; i++)
		shown[i] = i == K.len ? 0 : K.show ? K.buf[i] : '*';
	int tw = text_width(shown, 2);
	text(c, 42, 196, shown, 2, WHITE);
	if (fmodf(K.t, 0.8f) < 0.5f)
		fill(c, 44 + tw, 194, 3, 18, col);
	// keys
	const int kw = 50, kh = 32, sx = 54, sy = 38, x0 = (W - 10 * sx + 4) / 2, y0 = 234;
	if (osk_style == 3) { // one plate with thin dividers
		int pw = 10 * sx - (sx - kw), ph = 5 * sy - (sy - kh);
		rrect(c, x0 - 4, y0 - 4, pw + 8, ph + 8, 6, 0x1c1c1c);
		for (int r = 1; r < 5; r++)
			fill(c, x0, y0 + r * sy - (sy - kh) / 2 - 1, pw, 1, 0x303030);
		for (int r = 0; r < 4; r++)
			for (int k = 1; k < 10; k++)
				fill(c, x0 + k * sx - (sx - kw) / 2 - 1, y0 + r * sy, 1, kh, 0x303030);
		static const int cuts[3] = {2, 6, 8};
		for (int k = 0; k < 3; k++)
			fill(c, x0 + cuts[k] * sx - (sx - kw) / 2 - 1, y0 + 4 * sy, 1, kh, 0x303030);
	}
	for (int r = 0; r < 4; r++)
		for (int k = 0; k < 10; k++) {
			char s1[2] = {(K.shift ? KEYS_SHIFT : KEYS)[r][k], 0};
			osk_key(c, x0 + k * sx, y0 + r * sy, kw, kh, s1, K.r == r && K.c == k, KEY_BASE,
				col);
		}
	static const char *const names[4] = {"SHIFT", "SPACE", "DEL", "DONE"};
	static const int span[4][2] = {{0, 2}, {2, 4}, {6, 2}, {8, 2}};
	for (int k = 0; k < 4; k++) {
		int sel = K.r == 4 && special_at(K.c) == k;
		int x = x0 + span[k][0] * sx, w = span[k][1] * sx - (sx - kw), y = y0 + 4 * sy;
		uint32_t base = k == K_DONE ? mix(PAL[3], BG0, 0.3f)
			      : k == K_SHIFT && K.shift ? mix(col, BG0, 0.4f) : KEY_BASE;
		osk_key(c, x, y, w, kh, names[k], sel, base, col);
	}
}

/* ---- pages ------------------------------------------------------------------------ */

static void activate(int page, row_t *r)
{
	(void)page;
	switch (r->kind) {
	case R_TOGGLE:
		if (r->id == A_WIFI)
			sys_wifi_enable(!sys_wifi_enabled());
		else if (r->id == A_SSH)
			sys_ssh_enable(!sys_ssh_enabled());
		else if (r->id == A_SSHPW) {
			if (!sys_ssh_password_login() && !sys_ssh_has_password()) {
				P.choose = 1, P.sel = 0; // needs a password first
				toast("SET A ROOT PASSWORD FIRST");
				sfx_play(SFX_OPEN, APP_TERM);
				break;
			}
			sys_ssh_set_password_login(!sys_ssh_password_login());
		}
		sfx_play(r->on ? SFX_TOGGLE_OFF : SFX_TOGGLE_ON, 0);
		break;
	case R_ACTION:
		if (r->id == A_SCAN) {
			sys_wifi_rescan();
			sfx_play(SFX_CONFIRM, 0);
		} else if (r->id == A_ROOTPW) {
			P.choose = 1, P.sel = 0;
			sfx_play(SFX_OPEN, APP_TERM);
		}
		break;
	case R_NET: {
		net_t *n = &nets[r->id];
		const char *ssid;
		if (sys_wifi_status(&ssid, NULL) == WIFI_CONNECTED && !strcmp(ssid, n->ssid)) {
			snprintf(msg, sizeof msg, "ALREADY CONNECTED TO %s", n->ssid);
			toast(msg);
		} else if (n->secure && !n->saved) {
			sfx_play(SFX_OPEN, APP_WIFI);
			osk_open(OSK_WIFI, n->ssid);
		} else {
			sys_wifi_connect(n->ssid, NULL);
			snprintf(msg, sizeof msg, "CONNECTING TO %s", n->ssid);
			sfx_play(SFX_CONFIRM, 0);
			toast(msg);
		}
		break;
	}
	}
}

int net_update(int page, unsigned p, float dt)
{
	if (K.on) {
		osk_update(p, dt);
		return 0;
	}
	if (P.show) { // generated password: dismiss with A or B, then forget it
		if (p & (BTN_A | BTN_B)) {
			P.show = 0;
			wipe(P.pw, sizeof P.pw);
			sfx_play(SFX_TOGGLE_OFF, 0);
		}
		return 0;
	}
	if (P.choose) {
		int nopt = sys_ssh_has_password() ? 3 : 2;
		if (p & BTN_DOWN)
			P.sel = (P.sel + 1) % nopt, sfx_play(SFX_TICK, 0);
		if (p & BTN_UP)
			P.sel = (P.sel + nopt - 1) % nopt, sfx_play(SFX_TICK, 0);
		if (p & BTN_B)
			P.choose = 0, sfx_play(SFX_BACK, 0);
		if (p & BTN_A) {
			P.choose = 0;
			if (P.sel == 2) { // remove
				sys_ssh_set_password(NULL);
				sys_ssh_set_password_login(0);
				sfx_play(SFX_TOGGLE_OFF, 0);
				toast("PASSWORD REMOVED");
			} else if (P.sel == 0) {
				osk_open(OSK_ROOTPW, NULL);
			} else {
				char pw[20];
				if (random_password(pw)) {
					toast("NO RANDOM SOURCE: TYPE ONE INSTEAD");
					sfx_play(SFX_BUMP, 0);
				} else {
					set_root_password(pw, 1);
				}
				wipe(pw, sizeof pw);
			}
		}
		return 0;
	}
	build_rows(page);
	fix_sel(1);
	N.sel_y += (N.sel - N.sel_y) * (1 - expf(-dt * 22));
	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		return 1;
	}
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (d) {
		int s = N.sel + d;
		while (s >= 0 && s < nrows && !selectable(s))
			s += d;
		if (s >= 0 && s < nrows)
			N.sel = s, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if ((p & BTN_Y) && page == PAGE_WIFI && sys_wifi_enabled()) {
		sys_wifi_rescan();
		sfx_play(SFX_CONFIRM, 0);
	}
	row_t *r = &rows[N.sel];
	if ((p & BTN_A) || (r->kind == R_TOGGLE && (p & (BTN_LEFT | BTN_RIGHT))))
		activate(page, r);
	if ((p & BTN_X) && r->kind == R_NET && nets[r->id].saved) {
		sys_wifi_forget(nets[r->id].ssid);
		snprintf(msg, sizeof msg, "FORGOT %s", nets[r->id].ssid);
		sfx_play(SFX_TOGGLE_OFF, 0);
		toast(msg);
	}
	return 0;
}

static void draw_toggle(canvas *c, int xr, int mid, int on, int sel)
{
	if (sel)
		rrect(c, xr - 62, mid - 14, 64, 28, 5, BLACK);
	rrect(c, xr - 60, mid - 12, 60, 24, 4, on ? PAL[3] : 0x444444);
	fill(c, on ? xr - 26 : xr - 56, mid - 8, 22, 16, WHITE);
	if (on)
		text(c, xr - 54, mid - 7, "ON", 2, BLACK);
}

static void draw_bars(canvas *c, int x, int y, int bars, uint32_t col)
{
	for (int k = 0; k < 4; k++)
		fill(c, x + k * 5, y + 12 - k * 4, 3, 4 + k * 4, k < bars ? col : mix(col, BG0, 0.7f));
}

static void draw_lock(canvas *c, int x, int y, uint32_t col)
{
	fill(c, x + 2, y, 6, 2, col), fill(c, x + 1, y + 1, 2, 5, col), fill(c, x + 7, y + 1, 2, 5, col);
	fill(c, x, y + 5, 10, 8, col);
}

// One network: a kit row with signal bars, the name, its state and a lock.
static void draw_net_row(canvas *c, const row_t *r, int y, int sel, uint32_t col)
{
	const char *cur_ssid;
	int st = sys_wifi_status(&cur_ssid, NULL);
	const net_t *n = &nets[r->id];
	uint32_t ink = ui_row(c, y, "", sel, col);
	int mid = y + (UI_ROW - 6) / 2, xr = W - UI_MX - 16;
	draw_bars(c, UI_MX + 16, mid - 9, n->bars, sel ? ink : WHITE);
	int connected = st == WIFI_CONNECTED && !strcmp(cur_ssid, n->ssid);
	int connecting = st == WIFI_CONNECTING && !strcmp(cur_ssid, n->ssid);
	const char *tag = connected ? "CONNECTED" : connecting ? "..." : n->saved ? "SAVED" : "";
	if (n->secure)
		draw_lock(c, xr - 10, mid - 7, sel ? ink : GREY), xr -= 22;
	if (*tag)
		text(c, xr - text_width(tag, 2), mid - 7, tag, 2, sel ? ink : connected ? PAL[3] : GREY),
		    xr -= text_width(tag, 2) + 16;
	// the name, smaller if it would run into the tag
	int nx = UI_MX + 50, ns = UI_TS;
	while (ns > 1 && nx + text_width(n->ssid, ns) > xr)
		ns--;
	text(c, nx, mid - 7 * ns / 2, n->ssid, ns, sel ? ink : 0xdddddd);
}

// Wi-Fi page: the switch and the connection, then the networks.
static void draw_wifi(canvas *c)
{
	uint32_t col = PAL[APP_WIFI];
	int y = ui_page_header(c, APP_WIFI, "WI-FI", NULL);
	const char *ssid, *ip;
	int st = sys_wifi_status(&ssid, &ip), on = sys_wifi_enabled();

	ui_row(c, y, "WI-FI", N.sel == 0, col);
	ui_row_toggle(c, y, on, N.sel == 0);
	char line[80];
	uint32_t sc = GREY;
	switch (st) {
	case WIFI_CONNECTED: snprintf(line, sizeof line, "CONNECTED TO %s", ssid), sc = PAL[3]; break;
	case WIFI_CONNECTING: snprintf(line, sizeof line, "CONNECTING TO %s...", ssid), sc = PAL[2]; break;
	case WIFI_FAILED: snprintf(line, sizeof line, "%s", sys_wifi_error()), sc = PAL[0]; break;
	case WIFI_DISCONNECTED: snprintf(line, sizeof line, "NOT CONNECTED"); break;
	default: snprintf(line, sizeof line, "WI-FI IS OFF");
	}
	int sy = y + UI_ROW + 4, ls = UI_TS;
	int room = W - 2 * UI_MX - 44 - (st == WIFI_CONNECTED ? text_width(ip, 2) + 16 : 0);
	while (ls > 1 && text_width(line, ls) > room)
		ls--;
	fill(c, UI_MX + 16, sy + 7, 10, 10, sc);
	text(c, UI_MX + 34, sy + (UI_TS - ls) * 7 / 2 + 2, line, ls, sc == GREY ? GREY : WHITE);
	if (st == WIFI_CONNECTED)
		text(c, W - UI_MX - 16 - text_width(ip, 2), sy + 6, ip, 2, GREY);

	// networks
	int top = ui_section(c, sy + UI_INFO, "NETWORKS");
	if (on && sys_wifi_scanning()) {
		static const char *const dots[4] = {"SCANNING", "SCANNING.", "SCANNING..", "SCANNING..."};
		const char *sl = dots[(int)(ui_time() * 4) % 4];
		int sx = W - UI_MX - text_width("SCANNING...", 2);
		fill(c, sx - 10, top - 30 + 6, W - UI_MX - sx + 10, 18, BG0);
		text(c, sx, top - 30 + 8, sl, 2, PAL[2]);
	}
	if (!on || nrows <= 1) {
		const char *m = !on ? "TURN ON WI-FI TO SEE NETWORKS"
			      : sys_wifi_scanning() ? "LOOKING FOR NETWORKS..." : "NO NETWORKS FOUND";
		text(c, (W - text_width(m, UI_TS)) / 2, top + 50, m, UI_TS, 0x666666);
		return;
	}
	const int vis = (H - 44 - top) / UI_ROW;
	int li = N.sel - 1; // list index of the selection (-1: the switch)
	if (li >= 0 && li - N.scroll > vis - 1)
		N.scroll = (float)(li - vis + 1);
	if (li >= 0 && li < N.scroll)
		N.scroll = (float)li;
	for (int i = (int)N.scroll; i < nrows - 1 && i < (int)N.scroll + vis; i++)
		draw_net_row(c, &rows[i + 1], top + (i - (int)N.scroll) * UI_ROW, i == li, col);
	if (nrows - 1 > vis) { // scroll indicator
		int lh = vis * UI_ROW - 6;
		int th = lh * vis / (nrows - 1), tt = top + (int)(lh * N.scroll / (nrows - 1));
		fill(c, W - 8, top, 3, lh, 0x2a2a2a);
		fill(c, W - 8, tt, 3, th, GREY);
	}
}

// SSH page: a settings card with the switches, then read-only connection info.
static void draw_ssh(canvas *c)
{
	uint32_t col = PAL[APP_TERM];
	int y = ui_page_header(c, APP_TERM, "SSH", NULL);
	int on = sys_ssh_enabled(), has = sys_ssh_has_password(), keys = sys_ssh_keys();

	// the settings: two rows
	ui_row(c, y, "SSH", N.sel == 0, col);
	ui_row_toggle(c, y, on, N.sel == 0);
	ui_row(c, y + UI_ROW, "PASSWORD", N.sel == 1, col);
	ui_row_value(c, y + UI_ROW, has ? "SET" : "NOT SET", 1, N.sel == 1, col, has ? PAL[3] : PAL[2]);

	// how to connect
	y = ui_section(c, y + 2 * UI_ROW + 6, "CONNECT");
	const char *ip;
	int wifi = sys_wifi_status(NULL, &ip) == WIFI_CONNECTED;
	if (!on) {
		text(c, UI_MX + 16, y + 8, "TURN ON SSH TO CONNECT", UI_TS, GREY);
	} else {
		char cmd[64];
		snprintf(cmd, sizeof cmd, "ssh root@%s", wifi ? ip : sys_usb_ip());
		rrect(c, UI_MX, y + 2, W - 2 * UI_MX, 46, 6, 0x0c0c0c);
		text(c, UI_MX + 16, y + 15, "$", UI_TS, GREY);
		text(c, UI_MX + 52, y + 15, cmd, UI_TS, PAL[3]);
		const char *via = wifi ? "WI-FI" : "USB";
		text(c, W - UI_MX - 16 - text_width(via, 2), y + 18, via, 2, GREY);
		y += 58;
		char usb[64];
		snprintf(usb, sizeof usb, "ssh root@%s", sys_usb_ip());
		if (wifi)
			ui_info(c, y, "OVER USB", usb), y += UI_INFO;
		char with[64];
		if (keys && has)
			snprintf(with, sizeof with, "PASSWORD OR %d KEY%s", keys, keys > 1 ? "S" : "");
		else if (keys)
			snprintf(with, sizeof with, "%d KEY%s", keys, keys > 1 ? "S" : "");
		else
			snprintf(with, sizeof with, has ? "PASSWORD" : "NOTHING YET");
		ui_info(c, y, "LOG IN WITH", with);
		if (!keys && !has)
			text(c, UI_MX + 16, y + UI_INFO, "SET A PASSWORD OR ADD A KEY", 2, PAL[2]);
	}

	// password popup: a sheet with the same rows
	if (P.choose) {
		const char *opts[3] = {has ? "TYPE A NEW PASSWORD" : "TYPE A PASSWORD",
				       has ? "GENERATE A NEW ONE" : "GENERATE A RANDOM ONE", "REMOVE PASSWORD"};
		int n = has ? 3 : 2, ph = 60 + n * UI_ROW, py = H - 50 - ph;
		int ry = ui_panel(c, UI_MX - 10, py, W - 2 * UI_MX + 20, ph, "ROOT PASSWORD", col);
		for (int i = 0; i < n; i++)
			ui_row(c, ry + i * UI_ROW, opts[i], P.sel == i, i == 2 ? PAL[0] : col);
	} else if (P.show) {
		int ph = 210, py = H - 50 - ph;
		int ry = ui_panel(c, UI_MX - 10, py, W - 2 * UI_MX + 20, ph, "NEW ROOT PASSWORD", col);
		rrect(c, UI_MX, ry, W - 2 * UI_MX, 64, 6, 0x0c0c0c);
		text(c, (W - text_width(P.pw, 4)) / 2, ry + 18, P.pw, 4, PAL[3]);
		text(c, UI_MX + 16, ry + 82, "WRITE IT DOWN: IT WON'T", UI_TS, 0xdddddd);
		text(c, UI_MX + 16, ry + 108, "BE SHOWN AGAIN.", UI_TS, 0xdddddd);
	}
	if (K.on)
		osk_draw(c);
}

void net_draw(canvas *c, int page)
{
	if (page == PAGE_SSH) {
		draw_ssh(c);
		return;
	}
	if (page == PAGE_WIFI) {
		draw_wifi(c);
		if (K.on)
			osk_draw(c);
		return;
	}
	uint32_t col = page == PAGE_WIFI ? PAL[APP_WIFI] : PAL[APP_TERM];
	draw_header(c, page == PAGE_WIFI ? APP_WIFI : APP_TERM, page == PAGE_WIFI ? "WI-FI" : "SSH");
	const int top = 132, rh = 36, x = 20, w = W - 40, vis = page == PAGE_SSH ? 7 : 8;
	if (N.sel - N.scroll > vis - 1)
		N.scroll = (float)(N.sel - vis + 1);
	if (N.sel < N.scroll)
		N.scroll = (float)N.sel;
	int by = top + (int)lrintf((N.sel_y - N.scroll) * rh);
	wbox(c, x, by, w, rh - 6, col, W_NORMAL);
	uint32_t ink = wink(col, W_NORMAL);
	const char *cur_ssid;
	int st = sys_wifi_status(&cur_ssid, NULL);
	for (int i = (int)N.scroll; i < nrows && i < (int)N.scroll + vis; i++) {
		row_t *r = &rows[i];
		int y = top + (i - (int)N.scroll) * rh, mid = y + (rh - 6) / 2, ty = mid - 7;
		int sel = i == N.sel;
		uint32_t tc = sel ? ink : 0xdddddd;
		int xr = x + w - 14;
		switch (r->kind) {
		case R_TOGGLE:
			text(c, x + 14, ty, r->label, 2, tc);
			draw_toggle(c, xr, mid, r->on, sel);
			break;
		case R_INFO:
			text(c, x + 14, ty, r->label, 2, GREY);
			text(c, xr - text_width(r->value, 2), ty, r->value, 2,
			     st == WIFI_FAILED && page == PAGE_WIFI && i == 1 ? PAL[0] : WHITE);
			break;
		case R_ACTION:
			text(c, x + 14, ty, r->label, 2, tc);
			text(c, xr - 10, ty, ">", 2, sel ? ink : GREY);
			break;
		case R_NET: {
			net_t *n = &nets[r->id];
			draw_bars(c, x + 14, mid - 8, n->bars, sel ? ink : WHITE);
			text(c, x + 46, ty, n->ssid, 2, tc);
			int connected = st == WIFI_CONNECTED && !strcmp(cur_ssid, n->ssid);
			int connecting = st == WIFI_CONNECTING && !strcmp(cur_ssid, n->ssid);
			const char *tag = connected ? "CONNECTED" : connecting ? "..." : n->saved ? "SAVED" : "";
			int lx = xr - 10;
			if (n->secure)
				draw_lock(c, lx, mid - 7, sel ? ink : GREY);
			text(c, lx - 10 - text_width(tag, 2), ty, tag, 2,
			     sel ? ink : connected ? PAL[3] : GREY);
			break;
		}
		}
	}
	if (page == PAGE_SSH) { // ready-to-type command
		const char *ip;
		int wst = sys_wifi_status(NULL, &ip);
		char cmd[64];
		if (!sys_ssh_enabled())
			snprintf(cmd, sizeof cmd, "SSH IS OFF");
		else
			snprintf(cmd, sizeof cmd, "ssh root@%s", wst == WIFI_CONNECTED ? ip : sys_usb_ip());
		int cy = top + vis * rh + 8, cw = text_width(cmd, 2) + 32;
		wframe(c, (W - cw) / 2, cy, cw, 30, sys_ssh_enabled() ? col : 0x444444);
		fill(c, (W - cw) / 2, cy, cw, 30, 0x0c0c0c);
		text(c, (W - cw) / 2 + 16, cy + 8, cmd, 2, sys_ssh_enabled() ? PAL[3] : GREY);
	}
	if (K.on)
		osk_draw(c);
}

void net_hints(canvas *c, int page, int *x)
{
	(void)page;
	if (K.on) {
		draw_hint(c, x, "A", "TYPE", PAL[3]);
		draw_hint(c, x, "B", "DEL", PAL[0]);
		draw_hint(c, x, "Y", "SHIFT", PAL[2]);
		draw_hint(c, x, "X", K.show ? "HIDE" : "SHOW", PAL[4]);
		draw_hint(c, x, "START", "OK", 0xdddddd);
		return;
	}
	draw_hint(c, x, "^V", "MOVE", 0xdddddd);
	draw_hint(c, x, "A", "SELECT", PAL[3]);
	if (N.sel < nrows && rows[N.sel].kind == R_NET && nets[rows[N.sel].id].saved)
		draw_hint(c, x, "X", "FORGET", PAL[4]);
	if (page == PAGE_WIFI && sys_wifi_enabled())
		draw_hint(c, x, "Y", "SCAN", PAL[2]);
	draw_hint(c, x, "B", "BACK", PAL[0]);
}

// Short status for the main Settings list.
const char *net_wifi_note(void)
{
	const char *ssid;
	switch (sys_wifi_status(&ssid, NULL)) {
	case WIFI_CONNECTED: return ssid;
	case WIFI_CONNECTING: return "CONNECTING";
	case WIFI_OFF: return "OFF";
	default: return "NOT CONNECTED";
	}
}

const char *net_ssh_note(void)
{
	return sys_ssh_enabled() ? "ON" : "OFF";
}
