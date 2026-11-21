// Headless test driver: plays a scripted button sequence at 60 fps and saves
// screenshots as PPM. Each argument is one step:
//   w<sec>      wait (no buttons) for <sec> seconds, e.g. w0.5
//   p<keys>     tap buttons for 3 frames: U D L R A B X Y, M = menu,
//               S = select, T = start, [ ] = L R shoulders; e.g. pR or pA
//   h<keys>,<s> hold buttons for <s> seconds, e.g. hR,0.5
//   H<keys>,<s> same, but don't release afterwards (to capture mid-hold)
//   g<n>        widget style variation (0-3)
//   i<g><s>     icon variation for Games / Settings (0-3)
//   k<file>     render the icon sheet to <file>.ppm
//   f<n>        hub cover frame style (0 current, 1 clean, 2 thin, 3 shadow)
//   o<n>        on-screen keyboard key style (0 boxes, 1 flat, 2 none, 3 plate)
//   c<b><t><o>  chrome variation: background b, top bar t, bottom bar o
//   l<h><c>     layout variation: games hub h, Control Center c (e.g. l12)
//   s<file>     save the current frame to <file>.ppm
#include "ui.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t buf[UI_W * UI_H];
static const float DT = 1.0f / 60;

static void run(float secs, unsigned held)
{
	for (int f = 0; f < (int)(secs * 60 + 0.5f); f++)
		ui_frame(buf, UI_W, DT, held);
}

static void save(const char *name)
{
	char path[512];
	snprintf(path, sizeof path, "%s.ppm", name);
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(1);
	}
	fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
	for (int i = 0; i < UI_W * UI_H; i++) {
		unsigned char px[3] = {buf[i] >> 16, buf[i] >> 8, buf[i]};
		fwrite(px, 1, 3, f);
	}
	fclose(f);
}

void net_set_osk_style(int st);     // net.c (design comparison hook)
void games_set_cover_style(int st); // games.c (design comparison hook)

static unsigned keybit(char k)
{
	switch (k) {
	case 'U': return BTN_UP;
	case 'D': return BTN_DOWN;
	case 'L': return BTN_LEFT;
	case 'R': return BTN_RIGHT;
	case 'A': return BTN_A;
	case 'B': return BTN_B;
	case 'X': return BTN_X;
	case 'Y': return BTN_Y;
	case 'M': return BTN_MENU;
	case 'S': return BTN_SELECT;
	case 'T': return BTN_START;
	case '[': return BTN_L;
	case ']': return BTN_R;
	}
	return 0;
}

int main(int argc, char **argv)
{
	ui_init();
	for (int i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (a[0] == 'w') {
			run(strtof(a + 1, NULL), 0);
		} else if (a[0] == 'p' || a[0] == 'h' || a[0] == 'H') {
			unsigned b = 0;
			for (const char *k = a + 1; *k && *k != ','; k++)
				b |= keybit(*k);
			const char *comma = strchr(a, ',');
			run(a[0] != 'p' && comma ? strtof(comma + 1, NULL) : 3.0f / 60, b);
			if (a[0] != 'H') // H keeps holding (screenshot mid-hold)
				run(1.0f / 60, 0);
		} else if (a[0] == 'f' && a[1]) { // f<n> cover frame style
			games_set_cover_style(a[1] - '0');
		} else if (a[0] == 'o' && a[1]) { // o<n> keyboard key style
			net_set_osk_style(a[1] - '0');
		} else if (a[0] == 'i' && a[1] && a[2]) { // i<g><s> icon variations
			ui_set_icons(a[1] - '0', a[2] - '0');
		} else if (a[0] == 'k') { // k<file>: icon sheet
			ui_icon_sheet(buf, UI_W);
			save(a + 1);
		} else if (a[0] == 'g' && a[1]) { // g<n> widget style
			ui_set_widgets(a[1] - '0');
		} else if (a[0] == 'c' && a[1] && a[2] && a[3]) { // c<bg><top><bot>
			ui_set_chrome(a[1] - '0', a[2] - '0', a[3] - '0');
		} else if (a[0] == 'l' && a[1] && a[2]) { // l<hub><cc> layout variation
			ui_set_layout(a[1] - '0', a[2] - '0');
		} else if (a[0] == 's') {
			save(a + 1);
		}
	}
	return 0;
}
