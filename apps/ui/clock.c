// Settings > CLOCK: set automatically (network time) or by hand (date, time),
// and the time zone.
//
// TIME ZONE: pick a region, then a city. The choice is the link
// /data/etc/localtime -> /usr/share/zoneinfo/<zone>; the read-only root's
// /etc/localtime points at it, so every program (and the next boot) uses it.
// No link: UTC.
//
// COZ_ZONEINFO and COZ_TZ_LINK override the two paths (desktop builds, tests).

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ui_priv.h"
#include "sys.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAXZ 640

static const char *const REGIONS[] = {"UTC",       "AFRICA",   "AMERICA", "ANTARCTICA", "ASIA",   "ATLANTIC",
				      "AUSTRALIA", "EUROPE",   "INDIAN",  "PACIFIC"};
static const char *const REGION_DIR[] = {"",          "Africa", "America", "Antarctica", "Asia", "Atlantic",
					 "Australia", "Europe", "Indian",  "Pacific"};
#define NREG ((int)(sizeof REGIONS / sizeof *REGIONS))

static struct {
	int level;        // 0: regions, 1: cities of `region`
	int region, sel, scroll;
	int n;            // cities
	char *zone[MAXZ]; // "Europe/Rome"
	long off[MAXZ];   // UTC offset, seconds
} T;

static const char *zoneinfo(void)
{
	return getenv("COZ_ZONEINFO") ? getenv("COZ_ZONEINFO") : "/usr/share/zoneinfo";
}

static const char *link_path(void)
{
	return getenv("COZ_TZ_LINK") ? getenv("COZ_TZ_LINK") : "/data/etc/localtime";
}

// The zone in use ("Europe/Rome"), or "" for UTC.
static const char *current(void)
{
	static char buf[PATH_MAX];
	ssize_t n = readlink(link_path(), buf, sizeof buf - 1);
	if (n <= 0)
		return "";
	buf[n] = 0;
	size_t zl = strlen(zoneinfo());
	if (!strncmp(buf, zoneinfo(), zl) && buf[zl] == '/')
		return buf + zl + 1;
	return "";
}

// "Europe/Rome" -> "ROME", "America/Argentina/Buenos_Aires" -> "ARGENTINA / BUENOS AIRES"
static void pretty(const char *zone, int strip_region, char *out, size_t n)
{
	const char *s = zone;
	if (strip_region && strchr(s, '/'))
		s = strchr(s, '/') + 1;
	size_t k = 0;
	for (; *s && k + 4 < n; s++) {
		if (*s == '/')
			out[k++] = ' ', out[k++] = '/', out[k++] = ' ';
		else
			out[k++] = *s == '_' ? ' ' : (char)toupper((unsigned char)*s);
	}
	out[k] = 0;
}

static const char *tz_note(void)
{
	static char buf[64];
	const char *z = current();
	if (!*z)
		return "UTC";
	pretty(z, 1, buf, sizeof buf);
	return buf;
}

// UTC offset of a zone now, in seconds.
static long zone_offset(const char *zone)
{
	char tz[PATH_MAX];
	snprintf(tz, sizeof tz, ":%s/%s", zoneinfo(), zone);
	char *old = getenv("TZ") ? strdup(getenv("TZ")) : NULL;
	setenv("TZ", tz, 1);
	tzset();
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	long off = tm.tm_gmtoff;
	if (old)
		setenv("TZ", old, 1), free(old);
	else
		unsetenv("TZ");
	tzset();
	return off;
}

static int is_zone_file(const char *path)
{
	struct stat st;
	if (stat(path, &st) || !S_ISREG(st.st_mode) || st.st_size < 44)
		return 0;
	FILE *f = fopen(path, "rb");
	char m[4] = {0};
	if (f) {
		if (fread(m, 1, 4, f) != 4)
			m[0] = 0;
		fclose(f);
	}
	return !memcmp(m, "TZif", 4);
}

static int by_name(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}

// Cities of a region: its zone files, and those one folder down
// (America/Argentina/Buenos_Aires).
static void load_region(int r)
{
	for (int i = 0; i < T.n; i++)
		free(T.zone[i]);
	T.n = 0;
	if (r == 0) { // UTC
		T.zone[T.n++] = strdup("Etc/UTC");
	} else {
		char d[PATH_MAX];
		snprintf(d, sizeof d, "%s/%s", zoneinfo(), REGION_DIR[r]);
		DIR *dir = opendir(d);
		struct dirent *e;
		while (dir && (e = readdir(dir)) && T.n < MAXZ) {
			if (e->d_name[0] == '.')
				continue;
			char p[PATH_MAX];
			snprintf(p, sizeof p, "%s/%s", d, e->d_name);
			struct stat st;
			if (stat(p, &st))
				continue;
			if (S_ISDIR(st.st_mode)) {
				DIR *sub = opendir(p);
				struct dirent *f;
				while (sub && (f = readdir(sub)) && T.n < MAXZ) {
					char q[PATH_MAX];
					snprintf(q, sizeof q, "%s/%s", p, f->d_name);
					if (f->d_name[0] != '.' && is_zone_file(q) &&
					    asprintf(&T.zone[T.n], "%s/%s/%s", REGION_DIR[r], e->d_name, f->d_name) > 0)
						T.n++;
				}
				if (sub)
					closedir(sub);
			} else if (is_zone_file(p) && asprintf(&T.zone[T.n], "%s/%s", REGION_DIR[r], e->d_name) > 0) {
				T.n++;
			}
		}
		if (dir)
			closedir(dir);
		qsort(T.zone, (size_t)T.n, sizeof *T.zone, by_name);
	}
	for (int i = 0; i < T.n; i++)
		T.off[i] = zone_offset(T.zone[i]);
}

static void tz_open(void)
{
	const char *z = current();
	T.level = 0, T.sel = 0, T.scroll = 0, T.region = 0;
	for (int r = 1; r < NREG; r++) {
		size_t l = strlen(REGION_DIR[r]);
		if (!strncmp(z, REGION_DIR[r], l) && z[l] == '/')
			T.sel = r;
	}
}

static int set_zone(const char *zone)
{
	char target[PATH_MAX], tmp[PATH_MAX], dir[PATH_MAX];
	snprintf(target, sizeof target, "%s/%s", zoneinfo(), zone);
	snprintf(dir, sizeof dir, "%s", link_path());
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = 0;
		mkdir(dir, 0755);
	}
	snprintf(tmp, sizeof tmp, "%s.new", link_path());
	unlink(tmp);
	if (symlink(target, tmp) || rename(tmp, link_path()))
		return -errno;
	sync();
	tzset(); // the launcher's own clock follows at once
	return 0;
}

static int tz_update(unsigned p, float dt)
{
	(void)dt;
	int rows = T.level ? T.n : NREG;
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (p & BTN_R)
		d = 8;
	if (p & BTN_L)
		d = -8;
	if (d) {
		int s = T.sel + d;
		if (s < 0)
			s = 0;
		if (s >= rows)
			s = rows - 1;
		if (s != T.sel)
			T.sel = s, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		if (!T.level)
			return 1;
		T.level = 0, T.sel = T.region, T.scroll = 0;
		ui_page_anim(1);
		return 0;
	}
	if (!(p & BTN_A))
		return 0;
	if (!T.level) {
		T.region = T.sel;
		load_region(T.region);
		T.level = 1, T.sel = 0, T.scroll = 0;
		const char *z = current();
		for (int i = 0; i < T.n; i++)
			if (!strcmp(T.zone[i], z))
				T.sel = i;
		ui_page_anim(0);
		sfx_play(SFX_OPEN, APP_SETTINGS);
		return 0;
	}
	if (T.n == 0)
		return 0;
	if (set_zone(T.zone[T.sel])) {
		toast("COULD NOT SET THE TIME ZONE");
		sfx_play(SFX_BUMP, 0);
		return 0;
	}
	static char msg[96];	// toast() keeps the pointer
	char nm[64];
	pretty(T.zone[T.sel], T.region != 0, nm, sizeof nm);
	snprintf(msg, sizeof msg, "TIME ZONE: %s", T.region ? nm : "UTC");
	toast(msg);
	sfx_play(SFX_CONFIRM, 0);
	return 1;
}

static void offset_text(long off, char *out, size_t n)
{
	long a = off < 0 ? -off : off;
	if (a % 3600)
		snprintf(out, n, "UTC%c%ld:%02ld", off < 0 ? '-' : '+', a / 3600, a % 3600 / 60);
	else if (off)
		snprintf(out, n, "UTC%c%ld", off < 0 ? '-' : '+', a / 3600);
	else
		snprintf(out, n, "UTC");
}

static void tz_draw(canvas *c)
{
	uint32_t col = PAL[APP_SETTINGS];
	char sub[80];
	if (T.level)
		snprintf(sub, sizeof sub, "%s", REGIONS[T.region]);
	int y = ui_page_header(c, APP_SETTINGS, "TIME ZONE", T.level ? sub : NULL);
	int rows = T.level ? T.n : NREG, view = (H - 52 - y) / UI_ROW;
	if (T.sel < T.scroll)
		T.scroll = T.sel;
	if (T.sel >= T.scroll + view)
		T.scroll = T.sel - view + 1;
	const char *cur = current();
	for (int i = T.scroll; i < rows && i < T.scroll + view; i++) {
		int ry = y + (i - T.scroll) * UI_ROW, sel = i == T.sel;
		char label[64], val[24] = "";
		int here;
		if (T.level) {
			pretty(T.zone[i], T.region != 0, label, sizeof label);
			offset_text(T.off[i], val, sizeof val);
			here = !strcmp(T.zone[i], cur) || (T.region == 0 && !*cur);
		} else {
			snprintf(label, sizeof label, "%s", REGIONS[i]);
			size_t l = strlen(REGION_DIR[i]);
			here = i == 0 ? !*cur : !strncmp(cur, REGION_DIR[i], l) && cur[l] == '/';
		}
		ui_row(c, ry, label, sel, col);
		if (here && !T.level)
			ui_row_value(c, ry, "CURRENT", 1, sel, col, PAL[3]);
		else if (T.level)
			ui_row_value(c, ry, here ? "CURRENT" : val, 0, sel, col, here ? PAL[3] : GREY);
		else
			ui_row_value(c, ry, NULL, 1, sel, col, GREY);
	}
	if (rows > view) { // scroll bar on the right edge
		int lh = view * UI_ROW - 6, bh = lh * view / rows, bt = y + (int)((long)lh * T.scroll / rows);
		fill(c, W - 6, y, 2, lh, 0x2a2a2a);
		fill(c, W - 6, bt, 2, bh < 8 ? 8 : bh, GREY);
	}
}

static void tz_hints(canvas *c, int *x)
{
	draw_hint(c, x, "^V", "MOVE", 0xdddddd);
	if (T.level)
		draw_hint(c, x, "L/R", "PAGE", 0xdddddd);
	draw_hint(c, x, "A", T.level ? "SET" : "OPEN", PAL[3]);
	draw_hint(c, x, "B", "BACK", PAL[0]);
}

/* ---- the CLOCK page --------------------------------------------------------- */

enum { CR_AUTO, CR_DATE, CR_TIME, CR_ZONE, NCR };
static const char *const CR_NAMES[NCR] = {"SET AUTOMATICALLY", "DATE", "TIME", "TIME ZONE"};
static const char *const MON[12] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN",
				    "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
static struct {
	int sel;
	int zone;   // the TIME ZONE pages are open
	int edit;   // 0, CR_DATE or CR_TIME: changing it in place
	int field;  // date: day, month, year; time: hours, minutes
	struct tm tm;
} C;

const char *clock_note(void)
{
	static char buf[16];
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	snprintf(buf, sizeof buf, "%02d:%02d", tm.tm_hour, tm.tm_min);
	return buf;
}

void clock_open(void)
{
	memset(&C, 0, sizeof C);
	tzset();
}

static int days_in(int mon, int year)
{
	static const int d[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	int leap = (year % 4 == 0 && year % 100) || year % 400 == 0;
	return d[mon] + (mon == 1 && leap);
}

static void edit_step(int d)
{
	struct tm *t = &C.tm;
	if (C.edit == CR_DATE) {
		if (C.field == 0)
			t->tm_mday = (t->tm_mday - 1 + d + days_in(t->tm_mon, t->tm_year + 1900)) %
					     days_in(t->tm_mon, t->tm_year + 1900) + 1;
		else if (C.field == 1)
			t->tm_mon = (t->tm_mon + d + 12) % 12;
		else {
			t->tm_year += d;
			if (t->tm_year < 2024 - 1900)
				t->tm_year = 2024 - 1900;
			if (t->tm_year > 2099 - 1900)
				t->tm_year = 2099 - 1900;
		}
		int dm = days_in(t->tm_mon, t->tm_year + 1900);
		if (t->tm_mday > dm)
			t->tm_mday = dm;
	} else {
		if (C.field == 0)
			t->tm_hour = (t->tm_hour + d + 24) % 24;
		else
			t->tm_min = (t->tm_min + d + 60) % 60;
	}
}

int clock_update(unsigned p, float dt)
{
	if (C.zone) {
		if (tz_update(p, dt))
			C.zone = 0, ui_page_anim(1);
		return 0;
	}
	if (C.edit) { // ^v change the field, <> move between fields
		int nf = C.edit == CR_DATE ? 3 : 2;
		if (p & BTN_UP)
			edit_step(1), sfx_play(SFX_TICK, 0);
		if (p & BTN_DOWN)
			edit_step(-1), sfx_play(SFX_TICK, 0);
		if ((p & BTN_RIGHT) && C.field < nf - 1)
			C.field++, sfx_play(SFX_TICK, 0);
		if ((p & BTN_LEFT) && C.field > 0)
			C.field--, sfx_play(SFX_TICK, 0);
		if (p & BTN_B) {
			C.edit = 0;
			sfx_play(SFX_BACK, 0);
		} else if (p & BTN_A) {
			C.tm.tm_sec = 0, C.tm.tm_isdst = -1;
			time_t t = mktime(&C.tm);
			if (t != (time_t)-1 && sys_clock_set((long)t) == 0) {
				toast(C.edit == CR_DATE ? "DATE SET" : "TIME SET");
				sfx_play(SFX_CONFIRM, 0);
			} else {
				toast("COULD NOT SET THE CLOCK");
				sfx_play(SFX_BUMP, 0);
			}
			C.edit = 0;
		}
		return 0;
	}
	int d = (p & BTN_DOWN) ? 1 : (p & BTN_UP) ? -1 : 0;
	if (d) {
		if (C.sel + d >= 0 && C.sel + d < NCR)
			C.sel += d, sfx_play(SFX_TICK, 0);
		else
			sfx_play(SFX_BUMP, 0);
	}
	if (p & BTN_B) {
		sfx_play(SFX_BACK, 0);
		return 1;
	}
	int toggle = (C.sel == CR_AUTO && (p & (BTN_A | BTN_LEFT | BTN_RIGHT)));
	if (toggle) {
		int on = !sys_clock_auto();
		if (sys_clock_set_auto(on) == 0) {
			sfx_play(on ? SFX_TOGGLE_ON : SFX_TOGGLE_OFF, 0);
			toast(on ? "TIME FROM THE INTERNET WHEN WI-FI IS ON" : "SET THE DATE AND TIME BY HAND");
		} else {
			toast("COULD NOT CHANGE IT");
			sfx_play(SFX_BUMP, 0);
		}
		return 0;
	}
	if (!(p & BTN_A))
		return 0;
	if (C.sel == CR_ZONE) {
		C.zone = 1;
		tz_open();
		ui_page_anim(0);
		sfx_play(SFX_OPEN, APP_SETTINGS);
	} else if (sys_clock_auto()) {
		toast("TURN OFF SET AUTOMATICALLY FIRST");
		sfx_play(SFX_BUMP, 0);
	} else {
		time_t now = time(NULL);
		localtime_r(&now, &C.tm);
		C.edit = C.sel, C.field = 0;
		sfx_play(SFX_OPEN, APP_SETTINGS);
	}
	return 0;
}

// Right-aligned fields in row y; the selected one (while editing) is boxed.
static void draw_fields(canvas *c, int y, const char *const *f, int n, int selected, int sel, uint32_t col, uint32_t ink)
{
	int x = W - UI_MX - 16, ty = y + (UI_ROW - 6 - 7 * UI_TS) / 2;
	for (int i = n - 1; i >= 0; i--) {
		int w = text_width(f[i], UI_TS);
		x -= w;
		if (i == selected) {
			rrect(c, x - 6, y + 4, w + 12, UI_ROW - 14, 4, WHITE);
			text(c, x, ty, f[i], UI_TS, BLACK);
		} else {
			text(c, x, ty, f[i], UI_TS, sel ? ink : col);
		}
		x -= 18;
	}
}

void clock_draw(canvas *c)
{
	if (C.zone) {
		tz_draw(c);
		return;
	}
	uint32_t col = PAL[APP_SETTINGS];
	int y = ui_page_header(c, APP_SETTINGS, "CLOCK", NULL), au = sys_clock_auto();
	time_t now = time(NULL);
	struct tm tm;
	localtime_r(&now, &tm);
	const struct tm *show = C.edit ? &C.tm : &tm;
	for (int i = 0; i < NCR; i++) {
		int ry = y + i * UI_ROW, sel = i == C.sel;
		int dim = au && (i == CR_DATE || i == CR_TIME);
		uint32_t ink = ui_row(c, ry, CR_NAMES[i], sel, dim && !sel ? GREY : col);
		if (dim && !sel)	// automatic: the date and time can't be changed
			text(c, UI_MX + 16, ry + (UI_ROW - 6 - 7 * UI_TS) / 2, CR_NAMES[i], UI_TS, 0x666666);
		char a[8], b[8], d[8];
		if (i == CR_AUTO) {
			ui_row_toggle(c, ry, au, sel);
		} else if (i == CR_DATE) {
			snprintf(a, sizeof a, "%d", show->tm_mday);
			snprintf(b, sizeof b, "%s", MON[show->tm_mon]);
			snprintf(d, sizeof d, "%d", show->tm_year + 1900);
			const char *f[3] = {a, b, d};
			draw_fields(c, ry, f, 3, C.edit == CR_DATE ? C.field : -1, sel, dim ? 0x666666 : col, ink);
		} else if (i == CR_TIME) {
			snprintf(a, sizeof a, "%02d", show->tm_hour);
			snprintf(b, sizeof b, "%02d", show->tm_min);
			const char *f[2] = {a, b};
			draw_fields(c, ry, f, 2, C.edit == CR_TIME ? C.field : -1, sel, dim ? 0x666666 : col, ink);
			if (C.edit != CR_TIME) // the colon between hours and minutes
				text(c, W - UI_MX - 16 - text_width(b, UI_TS) - 13, ry + (UI_ROW - 6 - 7 * UI_TS) / 2, ":",
				     UI_TS, sel ? ink : dim ? 0x666666 : col);
		} else {
			ui_row_value(c, ry, tz_note(), 1, sel, col, col);
		}
	}
	y += NCR * UI_ROW + 10;
	const char *note = au ? (sys_wifi_status(NULL, NULL) == WIFI_CONNECTED ? "THE TIME COMES FROM THE INTERNET."
							     : "THE TIME COMES FROM THE INTERNET WHEN WI-FI IS ON.")
			      : "THE CLOCK KEEPS THE TIME YOU SET, EVEN WHEN OFF.";
	text(c, UI_MX + 16, y, note, 2, GREY);
}

void clock_hints(canvas *c, int *x)
{
	if (C.zone) {
		tz_hints(c, x);
		return;
	}
	if (C.edit) {
		draw_hint(c, x, "<>", "FIELD", 0xdddddd);
		draw_hint(c, x, "^V", "CHANGE", 0xdddddd);
		draw_hint(c, x, "A", "SET", PAL[3]);
		draw_hint(c, x, "B", "CANCEL", PAL[0]);
		return;
	}
	draw_hint(c, x, "^V", "MOVE", 0xdddddd);
	draw_hint(c, x, "A", C.sel == CR_AUTO ? "TOGGLE" : C.sel == CR_ZONE ? "OPEN" : "CHANGE", PAL[3]);
	draw_hint(c, x, "B", "BACK", PAL[0]);
}
