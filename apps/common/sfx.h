// Chiptune sound effects, synthesized on the fly (square / triangle / noise
// voices with simple envelopes). No sample files.
//
// sfx_play() is called from the UI thread, sfx_render() from the audio
// thread; they talk through a lock-free single-producer/single-consumer queue.
#ifndef SFX_H
#define SFX_H

#include <stdint.h>

enum {
	SFX_MOVE,       // param: tile index (each tile has its own note)
	SFX_BUMP,       // tried to move past the end
	SFX_OPEN,       // param: tile index
	SFX_BACK,
	SFX_TICK,       // list cursor moved
	SFX_CHANGE,     // param: new value 0..10
	SFX_TOGGLE_ON,
	SFX_TOGGLE_OFF,
	SFX_CONFIRM,
	SFX_WHOOSH,     // screen transition
	SFX_POP,        // toast appears
	SFX_JUMP,       // mock game
	SFX_COIN,       // mock game
	SFX_BOOT,       // boot jingle in sync with apps/splash (start at t=0); param: variant
	SFX_COUNT
};

#define SFX_BOOT_VARIANTS 6
#define SFX_BOOT_DEFAULT 4 // "minimal", chosen 2026-11-21
const char *sfx_boot_name(int variant);

void sfx_init(int sample_rate);
void sfx_play(int id, int param);
void sfx_set_volume(float v); // 0..1
void sfx_render(int16_t *out, int frames); // mono

#endif
