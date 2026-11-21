// cozisboy UI: screens, input handling and animation. Platform code (SDL on
// the PC, DRM + evdev on the device) only supplies a buffer, a frame time
// and the set of buttons currently held.
#ifndef COZ_UI_H
#define COZ_UI_H

#include <stdint.h>

enum {
	BTN_UP = 1 << 0,
	BTN_DOWN = 1 << 1,
	BTN_LEFT = 1 << 2,
	BTN_RIGHT = 1 << 3,
	BTN_A = 1 << 4,
	BTN_B = 1 << 5,
	BTN_X = 1 << 6,
	BTN_Y = 1 << 7,
	BTN_L = 1 << 8,
	BTN_R = 1 << 9,
	BTN_START = 1 << 10,
	BTN_SELECT = 1 << 11,
	BTN_MENU = 1 << 12,
};

#define UI_W 640
#define UI_H 480

void ui_init(void);
// Advance by dt seconds with `held` buttons down, then draw into buf.
void ui_frame(uint32_t *buf, int stride, float dt, unsigned held);
// Pick the games hub / Control Center layout variation (also in Settings).
void ui_set_layout(int hub, int cc);
// Pick background / top bar / bottom bar variations being compared.
void ui_set_chrome(int bg, int top, int bot);
// Pick the widget style variation (also in Settings).
void ui_set_widgets(int style);
// Pick the Games / Settings icon variation, and render all of them on a sheet.
void ui_set_icons(int games, int settings);
void ui_icon_sheet(uint32_t *buf, int stride);

#endif
