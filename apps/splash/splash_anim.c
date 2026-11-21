// Animated "cozisboy" boot splash: pure software renderer into an XRGB8888
// buffer. No dependencies, so the same code runs on the device (DRM/fbdev)
// and on the host (preview.c renders it to video).
//
// Timeline (seconds):
//   0.10 + i*0.09   letter i drops in from above with a bounce
//   ~0.75           sparkle on the dot of the "i" when it lands
//   1.20 - 1.55     white shine sweeps across the word
//   1.40 ->         idle: letters bob in a wave, progress bar fills
#include "splash_anim.h"

#include <math.h>
#include <stddef.h>

// Same glyph grid as art/logo/gen_logo.py: rows 0-1 ascender, 2-6 x-height,
// 7-8 descender.
static const char *const GLYPHS[8][9] = {
	{"", "", ".####", "##...", "##...", "##...", ".####"},                       // c
	{"", "", ".###.", "##.##", "##.##", "##.##", ".###."},                       // o
	{"", "", "#####", "...##", "..##.", ".##..", "#####"},                       // z
	{"##", "", "##", "##", "##", "##", "##"},                                    // i
	{"", "", ".####", "##...", ".###.", "...##", "####."},                       // s
	{"##...", "##...", "####.", "##.##", "##.##", "##.##", "####."},             // b
	{"", "", ".###.", "##.##", "##.##", "##.##", ".###."},                       // o
	{"", "", "##.##", "##.##", "##.##", ".####", "...##", "...##", "####."},     // y
};
static const uint32_t LETTER_COL[8] = {
	0xff4a4a, 0xff9a2e, 0xffd93d, 0x5ee05e, 0x3fc8ff, 0x6a7dff, 0xc56bff, 0xff6bc1,
};
#define NLETTERS 8
#define ROWS 9
#define PX 10            // screen pixels per logo cell
#define OUTLINE 3        // white outline thickness
#define OUTLINE2 2       // extra black ring around the white outline
#define SHADOW 3         // shadow offset per step
#define SHADOW_STEPS 2

#define T_FIRST 0.10f
#define T_STAGGER 0.09f
#define T_FALL 0.55f
#define T_SHINE 1.20f
#define T_SHINE_LEN 0.35f
#define T_IDLE 1.40f

struct canvas {
	uint32_t *px;
	int w, h, stride;
};

static int letter_x[NLETTERS], letter_w[NLETTERS];
static int word_w, inited;

static void init(void)
{
	int x = 0;
	for (int i = 0; i < NLETTERS; i++) {
		int w = 0;
		for (int r = 0; r < ROWS && GLYPHS[i][r]; r++) {
			int n = 0;
			while (GLYPHS[i][r][n])
				n++;
			if (n > w)
				w = n;
		}
		letter_x[i] = x;
		letter_w[i] = w;
		x += w + 1;
	}
	word_w = x - 1;
	inited = 1;
}

static void fill(struct canvas *c, int x, int y, int w, int h, uint32_t col)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
	int x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = c->px + (size_t)yy * c->stride;
		for (int xx = x0; xx < x1; xx++)
			row[xx] = col;
	}
}

static uint32_t mix(uint32_t a, uint32_t b, float t)
{
	if (t <= 0)
		return a;
	if (t >= 1)
		return b;
	int r = ((a >> 16) & 255) + (int)((((int)(b >> 16) & 255) - ((int)(a >> 16) & 255)) * t);
	int g = ((a >> 8) & 255) + (int)((((int)(b >> 8) & 255) - ((int)(a >> 8) & 255)) * t);
	int bl = (a & 255) + (int)((((int)b & 255) - ((int)a & 255)) * t);
	return (uint32_t)(r << 16 | g << 8 | bl);
}

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : v > hi ? hi : v;
}

static float ease_out_bounce(float t)
{
	const float n = 7.5625f, d = 2.75f;
	if (t < 1 / d)
		return n * t * t;
	if (t < 2 / d) {
		t -= 1.5f / d;
		return n * t * t + 0.75f;
	}
	if (t < 2.5f / d) {
		t -= 2.25f / d;
		return n * t * t + 0.9375f;
	}
	t -= 2.625f / d;
	return n * t * t + 0.984375f;
}

// Draw every cell of letter i at (ox, oy), grown by `grow` pixels.
// col == LETTER uses the letter's own colour, with the shine applied.
#define LETTER 0xff000000u
static void draw_letter(struct canvas *c, int i, int ox, int oy, int grow,
			uint32_t col, float shine_x)
{
	for (int r = 0; r < ROWS && GLYPHS[i][r]; r++) {
		for (int k = 0; GLYPHS[i][r][k]; k++) {
			if (GLYPHS[i][r][k] != '#')
				continue;
			int x = ox + k * PX, y = oy + r * PX;
			uint32_t fc = col;
			if (col == LETTER) {
				fc = LETTER_COL[i];
				// diagonal band of light, measured in cells
				float d = fabsf((float)(x + y / 2) - shine_x) / PX;
				if (d < 2.5f)
					fc = mix(fc, 0xffffff, d < 1.2f ? 0.75f : 0.35f);
			}
			fill(c, x - grow, y - grow, PX + 2 * grow, PX + 2 * grow, fc);
		}
	}
}

// 4-point pixel sparkle centred at (cx, cy), size 0..1
static void sparkle(struct canvas *c, int cx, int cy, float size, uint32_t col)
{
	int arm = (int)(size * 5 + 0.5f);
	int s = 4;
	for (int k = -arm; k <= arm; k++) {
		fill(c, cx + k * s - s / 2, cy - s / 2, s, s, col);
		fill(c, cx - s / 2, cy + k * s - s / 2, s, s, col);
	}
}

void splash_render(uint32_t *buf, int w, int h, int stride, float t, float progress)
{
	struct canvas cv = {buf, w, h, stride}, *c = &cv;
	if (!inited)
		init();

	// background: vertical gradient
	for (int y = 0; y < h; y++)
		fill(c, 0, y, w, 1, mix(0x111111, 0x1d1d1d, (float)y / h));

	// twinkling stars (fixed pseudo-random positions)
	uint32_t seed = 12345;
	for (int s = 0; s < 40; s++) {
		seed = seed * 1103515245u + 12345u;
		int x = (int)((seed >> 8) % (uint32_t)w);
		seed = seed * 1103515245u + 12345u;
		int y = (int)((seed >> 8) % (uint32_t)h);
		int sz = 2 + (s & 1) * 2;
		float b = 0.25f + 0.55f * (0.5f + 0.5f * sinf(t * (1.5f + (s % 5) * 0.6f) + s));
		fill(c, x, y, sz, sz, mix(0x111111, 0xffffff, b));
	}

	int wx = (w - word_w * PX) / 2;
	int wy = (h - ROWS * PX) / 2 - 24;

	// per-letter vertical offset
	int yoff[NLETTERS], vis[NLETTERS];
	float idle_amp = clampf((t - T_IDLE) / 0.5f, 0, 1);
	for (int i = 0; i < NLETTERS; i++) {
		float lt = (t - (T_FIRST + i * T_STAGGER)) / T_FALL;
		vis[i] = lt > 0;
		float drop = (1 - ease_out_bounce(clampf(lt, 0, 1))) * (wy + ROWS * PX + 20);
		float wave = sinf((t - T_IDLE) * 5.0f - i * 0.7f) * 4.0f * idle_amp;
		// snap the wave to 2-pixel steps so it stays chunky
		yoff[i] = -(int)drop + 2 * (int)lrintf(wave / 2);
	}

	float shine_t = (t - T_SHINE) / T_SHINE_LEN;
	float shine_x = shine_t > 0 && shine_t < 1
		? wx - 60 + shine_t * (word_w * PX + 180)
		: -100000.0f;

	// layers back to front, across all letters so outlines never cover fills
	for (int s = SHADOW_STEPS; s >= 1; s--)
		for (int i = 0; i < NLETTERS; i++)
			if (vis[i])
				draw_letter(c, i, wx + letter_x[i] * PX + s * SHADOW,
					    wy + yoff[i] + s * SHADOW, OUTLINE, 0x000000, 0);
	for (int i = 0; i < NLETTERS; i++)
		if (vis[i])
			draw_letter(c, i, wx + letter_x[i] * PX, wy + yoff[i],
				    OUTLINE + OUTLINE2, 0x000000, 0);
	for (int i = 0; i < NLETTERS; i++)
		if (vis[i])
			draw_letter(c, i, wx + letter_x[i] * PX, wy + yoff[i],
				    OUTLINE, 0xffffff, 0);
	for (int i = 0; i < NLETTERS; i++)
		if (vis[i])
			draw_letter(c, i, wx + letter_x[i] * PX, wy + yoff[i], 0,
				    LETTER, shine_x);

	// sparkle on the dot of the "i" (letter 3) just after it lands
	float land_i = T_FIRST + 3 * T_STAGGER + T_FALL * 0.36f;
	float sp = (t - land_i) / 0.45f;
	if (sp > 0 && sp < 1) {
		float size = sp < 0.3f ? sp / 0.3f : 1 - (sp - 0.3f) / 0.7f;
		int cx = wx + letter_x[3] * PX + PX + 18;
		int cy = wy + yoff[3] - 10;
		sparkle(c, cx, cy, size, mix(0xffffff, LETTER_COL[3], sp));
	}

	// rainbow progress bar: 8 segments in the letter colours
	float bar_a = clampf((t - 1.1f) / 0.3f, 0, 1);
	if (bar_a > 0) {
		int seg = 22, bh = 8;
		int bw = seg * NLETTERS;
		int bx = (w - bw) / 2, by = wy + ROWS * PX + 44;
		fill(c, bx - 3, by - 3, bw + 6, bh + 6, mix(0x111111, 0x000000, bar_a));
		fill(c, bx - 2, by - 2, bw + 4, bh + 4, mix(0x111111, 0x5a5a5a, bar_a));
		fill(c, bx, by, bw, bh, mix(0x111111, 0x000000, bar_a));
		float p = clampf(progress, 0, 1) * NLETTERS;
		for (int k = 0; k < NLETTERS && p > k; k++) {
			int sw = p >= k + 1 ? seg : (int)((p - k) * seg);
			fill(c, bx + k * seg, by, sw, bh, mix(0x111111, LETTER_COL[k], bar_a));
			fill(c, bx + k * seg, by, sw, 2, mix(0x111111, 0xffffff, bar_a * 0.5f));
		}
	}
}
