// Libretro host: runs one emulator core (e.g. mgba_libretro.so) inside the UI.
// Video comes out as a 240x160 XRGB frame, audio goes into a ring buffer that
// the audio thread mixes in with emu_audio_mix().
//
// Only one game is loaded in the core at a time. The UI keeps several "open"
// games by snapshotting the live one (emu_save_state) before loading another.
#ifndef COZ_EMU_H
#define COZ_EMU_H

#include <stddef.h>
#include <stdint.h>

#define EMU_W 240
#define EMU_H 160

int emu_init(const char *core_path); // 0 on success
int emu_available(void);

// Load a ROM (unloading any current game). Optional snapshot to restore and
// battery save file (.sav) to read. Returns 0 on success.
int emu_load(const char *rom_path, const void *state, size_t state_size, const char *sav_path);
// Write the battery save (if any) and unload the current game.
void emu_unload(const char *sav_path);
void emu_write_sav(const char *sav_path);
int emu_loaded(void);

size_t emu_state_size(void);
int emu_save_state(void *buf, size_t size); // 0 on success
int emu_load_state(const void *buf, size_t size);
void emu_reset(void);

// Run `frames` emulated frames with these UI buttons held (BTN_* mask).
// Audio is kept only for the first frame (fast-forward is silent).
void emu_run(unsigned buttons, int frames);
const uint32_t *emu_frame(void); // EMU_W x EMU_H XRGB

// Audio thread: add the emulator's audio (mono, `rate` Hz) into out.
void emu_audio_mix(int16_t *out, int frames, int rate);
void emu_set_volume(float v);

// Core option, e.g. ("mgba_color_correction", "GBA"). Applied on the next frame.
void emu_set_option(const char *key, const char *val);

#endif
