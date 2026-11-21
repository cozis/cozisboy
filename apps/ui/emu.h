// GBA emulation for the UI, on mGBA's library (libmgba).
//
// Every open game is its own emulator, fully loaded: switching games is
// instant. Only the game on screen runs. Games can be linked with an emulated
// link cable; linked games run together in one thread, in lockstep.
#ifndef COZ_EMU_H
#define COZ_EMU_H

#include <stddef.h>
#include <stdint.h>

#define EMU_W 240
#define EMU_H 160

typedef struct emu_game emu_game;

int emu_init(void); // 0 on success
int emu_available(void);

// Load a ROM. The battery save lives in sav_out; if that doesn't exist yet it
// starts as a copy of sav_in (e.g. the .sav next to the ROM), which is never
// written. Returns NULL on failure.
emu_game *emu_open(const char *rom, const char *sav_in, const char *sav_out);
void emu_close(emu_game *g); // flushes the battery save

void emu_reset(emu_game *g);
size_t emu_state_size(emu_game *g);
int emu_save_state(emu_game *g, void *buf, size_t size); // 0 on success
int emu_load_state(emu_game *g, const void *buf, size_t size);

// Run `frames` frames of g (and of the games linked with it), with these UI
// buttons (BTN_* mask) held on g. Audio is kept from g's first frame only.
void emu_run(emu_game *g, unsigned buttons, int frames);
const uint32_t *emu_frame(emu_game *g); // EMU_W x EMU_H XRGB

// Link cable: connect b to a's group (up to 4 games). Returns 0 on success.
int emu_link(emu_game *a, emu_game *b);
int emu_linked(emu_game *g); // number of games in g's group (1 = not linked)

void emu_set_color_fix(int on); // GBA screen colour correction
void emu_set_volume(float v);
// Audio thread: add the emulator's audio (mono, 48 kHz) into out.
void emu_audio_mix(int16_t *out, int frames, int rate);

#endif
