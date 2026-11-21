// Tiny software renderer for the cozisboy UI: XRGB8888 canvas, rectangles,
// a 5x7 pixel font and outlined "arcade" sprites. No dependencies.
#ifndef GFX_H
#define GFX_H

#include <stdint.h>

typedef struct {
	uint32_t *px;
	int w, h, stride; // stride in pixels
} canvas;

#define BLACK 0x000000u
#define WHITE 0xffffffu

void fill(canvas *c, int x, int y, int w, int h, uint32_t col);
// rectangle with pixel-notched corners (r = notch size)
void rrect(canvas *c, int x, int y, int w, int h, int r, uint32_t col);
// filled pixel circle of radius r centred on (cx, cy)
void disc(canvas *c, int cx, int cy, int r, uint32_t col);
uint32_t mix(uint32_t a, uint32_t b, float t);
// multiply a rectangle towards black (amount 0..1)
void darken(canvas *c, int x, int y, int w, int h, float amount);

// 5x7 font: ASCII letters (both cases), digits and common symbols. Advance = 6*scale.
int text_width(const char *s, int scale);
void text(canvas *c, int x, int y, const char *s, int scale, uint32_t col);
// Arcade style: coloured fill, white outline, black ring, hard black shadow.
void text_arcade(canvas *c, int x, int y, const char *s, int scale, uint32_t col);

// Cell-map sprite, n x n cells (n <= ICON_MAX). Cell characters:
//   '.' empty   '#' main colour   'k' detail colour   'a' accent (tinted detail)
//   palette: w white  l light grey  g grey  d dark  r red  y yellow  n green
//            c cyan   b blue        p pink  v purple
// Every non-empty cell gets a black outline and a drop shadow.
#define ICON_N 16
#define ICON_MAX 24
typedef struct {
	int n;
	char cell[ICON_MAX][ICON_MAX];
} icon_t;
void icon_from_rows(icon_t *ic, const char *const *rows, int n);
void draw_icon(canvas *c, const icon_t *ic, int x, int y, int scale,
	       uint32_t main, uint32_t detail, uint32_t shadow);
// Same, with every colour pulled towards `to` by `dim` (0..1).
void draw_icon_dim(canvas *c, const icon_t *ic, int x, int y, int scale, uint32_t main,
		   uint32_t detail, uint32_t shadow, float dim, uint32_t to);

#endif
