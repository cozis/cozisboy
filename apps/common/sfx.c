#include "sfx.h"

#include <math.h>
#include <stdatomic.h>
#include <string.h>

enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE };

typedef struct {
	int on, wave;
	float duty;      // square duty cycle
	float freq;      // Hz
	float slide;     // octaves per second (negative = falling)
	float vol;       // peak 0..1
	float attack;    // seconds
	float len;       // total seconds (linear decay after attack)
	float delay;     // seconds before it starts
	float t, phase;
	uint16_t lfsr;
} voice_t;

#define NVOICES 64
static voice_t voices[NVOICES];
static int rate = 48000;
static _Atomic float master = 0.5f;
static float lp; // one-pole low-pass state, softens the harsh square edges

// lock-free event queue (UI thread -> audio thread)
#define QN 64
static struct {
	int id, param;
} queue[QN];
static atomic_uint q_head, q_tail;

void sfx_init(int sample_rate)
{
	rate = sample_rate;
	memset(voices, 0, sizeof voices);
}

void sfx_set_volume(float v)
{
	atomic_store(&master, v < 0 ? 0 : v > 1 ? 1 : v);
}

void sfx_play(int id, int param)
{
	unsigned h = atomic_load_explicit(&q_head, memory_order_relaxed);
	unsigned t = atomic_load_explicit(&q_tail, memory_order_acquire);
	if (h - t >= QN)
		return; // full: drop
	queue[h % QN].id = id;
	queue[h % QN].param = param;
	atomic_store_explicit(&q_head, h + 1, memory_order_release);
}

/* ---- sound recipes ------------------------------------------------------ */

static float note(float semis) // semitones above C5
{
	return 523.25f * powf(2, semis / 12);
}

// Pentatonic note per home tile: C D E G A C D E
static const float TILE_NOTE[8] = {0, 2, 4, 7, 9, 12, 14, 16};

static void voice(int wave, float duty, float freq, float slide, float vol,
		  float attack, float len, float delay)
{
	for (int i = 0; i < NVOICES; i++) {
		voice_t *v = &voices[i];
		if (v->on)
			continue;
		*v = (voice_t){1, wave, duty, freq, slide, vol, attack, len, delay, 0, 0, 0x7fff};
		return;
	}
}

/* ---- boot jingles --------------------------------------------------------
 * Timings mirror apps/splash/splash_anim.c: letter i starts falling at
 * 0.10 + i*0.09 s, its bounce first touches down 0.20 s later (LAND) and a
 * second time 0.40 s later (LAND2). Sparkle on the "i" at 0.57 s, white
 * shine 1.20-1.55 s, letters settle into their wave from 1.55 s.
 */
#define LAND(i) (0.30f + (i) * 0.09f)
#define LAND2(i) (0.50f + (i) * 0.09f)
#define T_SPARKLE 0.57f
#define T_SHINE 1.20f
#define T_SETTLE 1.55f

static const char *const JINGLE_NAMES[] = {
	"classic", "bubbly", "fanfare", "cozy", "minimal", "drums",
};

const char *sfx_boot_name(int variant)
{
	return variant >= 0 && variant < SFX_BOOT_VARIANTS ? JINGLE_NAMES[variant] : "?";
}

static void chord(int wave, float duty, const float *semis, int n, float vol,
		  float attack, float len, float at)
{
	for (int k = 0; k < n; k++)
		voice(wave, duty, note(semis[k]), 0, vol, attack, len, at);
}

static void boot_jingle(int variant)
{
	switch (variant) {
	default:
	case 0: { // classic: square notes per letter, sparkle, arpeggio, major chord
		for (int i = 0; i < 8; i++)
			voice(WAVE_SQUARE, 0.5f, note(TILE_NOTE[i]), 0, 0.20f, 0.002f, 0.14f, LAND(i));
		voice(WAVE_TRI, 0, note(-24), 0, 0.45f, 0.003f, 0.35f, LAND(0));
		voice(WAVE_TRI, 0, note(-17), 0, 0.45f, 0.003f, 0.35f, LAND(4));
		voice(WAVE_TRI, 0, note(36), 0.5f, 0.28f, 0.001f, 0.12f, T_SPARKLE);
		voice(WAVE_TRI, 0, note(31), 0.5f, 0.18f, 0.001f, 0.10f, T_SPARKLE + 0.06f);
		static const float gl[7] = {12, 16, 19, 24, 28, 31, 36};
		for (int k = 0; k < 7; k++)
			voice(WAVE_SQUARE, 0.125f, note(gl[k]), 0, 0.12f, 0.001f, 0.09f,
			      T_SHINE + k * 0.045f);
		static const float ch[3] = {0, 4, 7};
		chord(WAVE_SQUARE, 0.25f, ch, 3, 0.11f, 0.01f, 0.75f, T_SETTLE);
		voice(WAVE_TRI, 0, note(-24), 0, 0.50f, 0.005f, 0.85f, T_SETTLE);
		break;
	}
	case 1: { // bubbly: triangle boings with echoes, walking bass, maj7 ending
		for (int i = 0; i < 8; i++) {
			voice(WAVE_TRI, 0, note(TILE_NOTE[i] + 12) * 0.84f, 1.6f, 0.42f, 0.002f, 0.12f, LAND(i));
			voice(WAVE_TRI, 0, note(TILE_NOTE[i] + 12), 0, 0.12f, 0.002f, 0.06f, LAND2(i));
		}
		static const float walk[4] = {-24, -20, -17, -12};
		for (int k = 0; k < 4; k++)
			voice(WAVE_TRI, 0, note(walk[k]), 0, 0.40f, 0.003f, 0.17f, LAND(k * 2));
		voice(WAVE_TRI, 0, note(36), 1, 0.25f, 0.001f, 0.10f, T_SPARKLE);
		voice(WAVE_TRI, 0, note(40), 1, 0.15f, 0.001f, 0.10f, T_SPARKLE + 0.05f);
		static const float gl[8] = {24, 26, 28, 31, 33, 36, 38, 40};
		for (int k = 0; k < 8; k++)
			voice(WAVE_TRI, 0, note(gl[k]), 0, 0.22f, 0.001f, 0.07f, T_SHINE + k * 0.035f);
		static const float ch[4] = {0, 4, 7, 11};
		chord(WAVE_TRI, 0, ch, 4, 0.20f, 0.02f, 1.0f, T_SETTLE);
		voice(WAVE_SQUARE, 0.125f, note(23), 0, 0.05f, 0.02f, 0.9f, T_SETTLE);
		voice(WAVE_TRI, 0, note(-24), 0, 0.45f, 0.005f, 1.0f, T_SETTLE);
		break;
	}
	case 2: { // fanfare: doubled lead, octave bass, snare, power-up, two hits
		for (int i = 0; i < 8; i++) {
			voice(WAVE_SQUARE, 0.5f, note(TILE_NOTE[i]), 0, 0.16f, 0.002f, 0.10f, LAND(i));
			voice(WAVE_SQUARE, 0.25f, note(TILE_NOTE[i] - 5), 0, 0.10f, 0.002f, 0.10f, LAND(i));
			voice(WAVE_SQUARE, 0.5f, note(i & 1 ? -12 : -24), 0, 0.14f, 0.002f, 0.07f, LAND(i));
			if (i & 1)
				voice(WAVE_NOISE, 0, 6000, 0, 0.14f, 0.001f, 0.06f, LAND(i));
		}
		voice(WAVE_TRI, 0, note(36), 0, 0.25f, 0.001f, 0.10f, T_SPARKLE);
		voice(WAVE_SQUARE, 0.5f, note(12), 2.2f, 0.14f, 0.005f, 0.33f, T_SHINE);
		voice(WAVE_SQUARE, 0.25f, note(7), 2.2f, 0.08f, 0.005f, 0.33f, T_SHINE);
		static const float ch[4] = {0, 4, 7, 12};
		chord(WAVE_SQUARE, 0.5f, ch, 4, 0.09f, 0.003f, 0.12f, T_SETTLE);
		chord(WAVE_SQUARE, 0.5f, ch, 4, 0.10f, 0.003f, 0.65f, T_SETTLE + 0.16f);
		voice(WAVE_SQUARE, 0.5f, note(-24), 0, 0.16f, 0.003f, 0.12f, T_SETTLE);
		voice(WAVE_SQUARE, 0.5f, note(-24), 0, 0.16f, 0.003f, 0.65f, T_SETTLE + 0.16f);
		voice(WAVE_NOISE, 0, 3000, 0, 0.13f, 0.002f, 0.7f, T_SETTLE + 0.16f);
		break;
	}
	case 3: { // cozy: soft bells, Fmaj7 -> Cmaj9
		for (int i = 0; i < 8; i++) {
			voice(WAVE_TRI, 0, note(TILE_NOTE[i]), 0, 0.32f, 0.004f, 0.30f, LAND(i));
			voice(WAVE_TRI, 0, note(TILE_NOTE[i] + 19), 0, 0.06f, 0.002f, 0.18f, LAND(i));
		}
		voice(WAVE_TRI, 0, note(-19), 0, 0.35f, 0.02f, 0.55f, LAND(0));
		voice(WAVE_TRI, 0, note(-17), 0, 0.35f, 0.02f, 0.55f, LAND(4));
		voice(WAVE_TRI, 0, note(31), 0, 0.15f, 0.002f, 0.25f, T_SPARKLE);
		static const float f7[4] = {-7, -3, 0, 4};
		chord(WAVE_TRI, 0, f7, 4, 0.13f, 0.04f, 0.45f, T_SHINE);
		voice(WAVE_TRI, 0, note(-31), 0, 0.40f, 0.03f, 0.45f, T_SHINE);
		static const float c9[5] = {0, 4, 7, 11, 14};
		chord(WAVE_TRI, 0, c9, 5, 0.11f, 0.05f, 1.3f, T_SETTLE);
		voice(WAVE_TRI, 0, note(-24), 0, 0.42f, 0.03f, 1.3f, T_SETTLE);
		break;
	}
	case 4: { // minimal: ticks, sparkle, one soft chord
		for (int i = 0; i < 8; i++)
			voice(WAVE_SQUARE, 0.125f, note(24 + TILE_NOTE[i]), 0, 0.05f, 0.001f, 0.025f, LAND(i));
		voice(WAVE_TRI, 0, note(36), 0.3f, 0.20f, 0.001f, 0.15f, T_SPARKLE);
		static const float ch[3] = {0, 7, 16};
		chord(WAVE_TRI, 0, ch, 3, 0.18f, 0.03f, 1.1f, T_SETTLE);
		voice(WAVE_TRI, 0, note(-24), 0, 0.35f, 0.03f, 1.1f, T_SETTLE);
		break;
	}
	case 5: { // drums: tuned toms per letter, kick, cymbal swell, stab + crash
		voice(WAVE_TRI, 0, 160, -4, 0.65f, 0.001f, 0.16f, LAND(0));
		for (int i = 0; i < 8; i++) {
			voice(WAVE_TRI, 0, note(TILE_NOTE[i] - 12), -2, 0.50f, 0.001f, 0.13f, LAND(i));
			voice(WAVE_NOISE, 0, 9000, 0, 0.10f, 0.001f, 0.015f, LAND(i));
		}
		voice(WAVE_TRI, 0, 160, -4, 0.65f, 0.001f, 0.16f, LAND(4));
		voice(WAVE_TRI, 0, note(36), 0, 0.22f, 0.001f, 0.08f, T_SPARKLE);
		voice(WAVE_NOISE, 0, 4000, 1, 0.12f, 0.30f, 0.35f, T_SHINE);
		voice(WAVE_TRI, 0, 160, -4, 0.70f, 0.001f, 0.18f, T_SETTLE);
		static const float ch[3] = {0, 4, 7};
		chord(WAVE_SQUARE, 0.25f, ch, 3, 0.11f, 0.003f, 0.35f, T_SETTLE);
		voice(WAVE_NOISE, 0, 5000, 0, 0.14f, 0.002f, 0.8f, T_SETTLE);
		break;
	}
	}
}

static void start(int id, int p)
{
	float n = TILE_NOTE[(p % 8 + 8) % 8];
	switch (id) {
	case SFX_MOVE:
		voice(WAVE_SQUARE, 0.25f, note(n), 0, 0.30f, 0.002f, 0.07f, 0);
		voice(WAVE_SQUARE, 0.25f, note(n), 0, 0.10f, 0.002f, 0.06f, 0.075f); // echo
		break;
	case SFX_BUMP:
		voice(WAVE_SQUARE, 0.5f, 140, -3, 0.35f, 0.001f, 0.10f, 0);
		voice(WAVE_NOISE, 0, 1800, 0, 0.12f, 0.001f, 0.04f, 0);
		break;
	case SFX_OPEN: {
		static const float arp[4] = {0, 4, 7, 12};
		for (int k = 0; k < 4; k++)
			voice(WAVE_SQUARE, 0.5f, note(n + arp[k]), 0, 0.22f, 0.002f,
			      k == 3 ? 0.18f : 0.07f, k * 0.045f);
		voice(WAVE_TRI, 0, note(n - 24), 0, 0.45f, 0.002f, 0.25f, 0);
		break;
	}
	case SFX_BACK:
		voice(WAVE_SQUARE, 0.25f, note(7), 0, 0.22f, 0.002f, 0.06f, 0);
		voice(WAVE_SQUARE, 0.25f, note(0), 0, 0.22f, 0.002f, 0.10f, 0.055f);
		voice(WAVE_TRI, 0, note(-24), 0, 0.40f, 0.002f, 0.15f, 0.055f);
		break;
	case SFX_TICK:
		voice(WAVE_SQUARE, 0.125f, note(12), 0, 0.18f, 0.001f, 0.03f, 0);
		break;
	case SFX_CHANGE:
		voice(WAVE_SQUARE, 0.25f, note(-5 + p * 2), 0, 0.24f, 0.001f, 0.05f, 0);
		break;
	case SFX_TOGGLE_ON:
		voice(WAVE_SQUARE, 0.5f, note(4), 0, 0.22f, 0.002f, 0.05f, 0);
		voice(WAVE_SQUARE, 0.5f, note(9), 0, 0.22f, 0.002f, 0.08f, 0.05f);
		break;
	case SFX_TOGGLE_OFF:
		voice(WAVE_SQUARE, 0.5f, note(9), 0, 0.22f, 0.002f, 0.05f, 0);
		voice(WAVE_SQUARE, 0.5f, note(4), 0, 0.22f, 0.002f, 0.08f, 0.05f);
		break;
	case SFX_CONFIRM:
		voice(WAVE_SQUARE, 0.5f, note(11), 0, 0.22f, 0.002f, 0.06f, 0);
		voice(WAVE_SQUARE, 0.5f, note(16), 0, 0.22f, 0.002f, 0.28f, 0.06f);
		break;
	case SFX_WHOOSH:
		voice(WAVE_NOISE, 0, 900, 3.5f, 0.10f, 0.12f, 0.32f, 0);
		break;
	case SFX_BOOT:
		boot_jingle(p);
		break;
	case SFX_JUMP:
		voice(WAVE_SQUARE, 0.5f, note(-5), 2.5f, 0.16f, 0.002f, 0.14f, 0);
		break;
	case SFX_COIN:
		voice(WAVE_SQUARE, 0.5f, note(11), 0, 0.14f, 0.001f, 0.05f, 0);
		voice(WAVE_SQUARE, 0.5f, note(16), 0, 0.14f, 0.001f, 0.18f, 0.05f);
		break;
	case SFX_POP:
		voice(WAVE_TRI, 0, note(7), 6, 0.40f, 0.001f, 0.06f, 0);
		break;
	}
}

/* ---- mixer -------------------------------------------------------------- */

void sfx_render(int16_t *out, int frames)
{
	// pick up new events
	unsigned t = atomic_load_explicit(&q_tail, memory_order_relaxed);
	unsigned h = atomic_load_explicit(&q_head, memory_order_acquire);
	for (; t != h; t++)
		start(queue[t % QN].id, queue[t % QN].param);
	atomic_store_explicit(&q_tail, t, memory_order_release);

	float m = atomic_load(&master);
	float dt = 1.0f / rate;
	float a = 1 - expf(-2 * 3.14159265f * 7000.0f / rate); // ~7 kHz low-pass

	for (int i = 0; i < frames; i++) {
		float s = 0;
		for (int k = 0; k < NVOICES; k++) {
			voice_t *v = &voices[k];
			if (!v->on)
				continue;
			if (v->delay > 0) {
				v->delay -= dt;
				continue;
			}
			float env = v->t < v->attack ? v->t / v->attack
				  : 1 - (v->t - v->attack) / (v->len - v->attack);
			if (env <= 0 && v->t >= v->attack) {
				v->on = 0;
				continue;
			}
			float f = v->freq * exp2f(v->slide * v->t);
			float x;
			v->phase += f * dt;
			if (v->wave == WAVE_NOISE) {
				// NES-style 15-bit LFSR, clocked at f
				while (v->phase >= 1) {
					v->phase -= 1;
					unsigned bit = (v->lfsr ^ (v->lfsr >> 1)) & 1;
					v->lfsr = (uint16_t)((v->lfsr >> 1) | (bit << 14));
				}
				x = (v->lfsr & 1) ? 1.0f : -1.0f;
			} else {
				v->phase -= floorf(v->phase);
				// pulse minus its average, so narrow duties don't thump
				x = v->wave == WAVE_SQUARE
					? (v->phase < v->duty ? 1.0f : -1.0f) - (2 * v->duty - 1)
					: 4 * fabsf(v->phase - 0.5f) - 1;
			}
			s += x * v->vol * env;
			v->t += dt;
		}
		lp += a * (s - lp);
		float y = lp * m * 1.6f;
		// linear up to the knee, then a soft limiter (no distortion on quiet parts)
		float ay = fabsf(y);
		if (ay > 0.6f)
			y = copysignf(0.6f + 0.39f * tanhf((ay - 0.6f) / 0.39f), y);
		out[i] = (int16_t)(y * 32000);
	}
}
