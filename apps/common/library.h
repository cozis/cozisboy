// cozisboy game library: games, battery saves and save states.
//
// The one place that knows how games are stored. The launcher, the web app and
// tools go through this API and never build paths themselves, so the storage
// can change without touching them.
//
// Model
//   game   a ROM for one system, identified by a stable id (from the system
//          and the game's name, so it survives restarts and rescans)
//   save   a battery save (what the in-game "Save" writes). A game has up to
//          COZ_SAVE_SLOTS numbered saves: 1 is the main one (what A plays),
//          the others are extra playthroughs. Replacing or deleting a save
//          keeps the previous one as that save's backup
//   state  an emulator save state with a screenshot, identified by the time it
//          was taken (no slot numbers)
//   cover  box art (PNG), optional
//
// Root: $COZ_LIBRARY, else /data/library. Not thread-safe per coz_lib; the web
// server serialises calls with a lock. Functions returning int give 0 on
// success and -errno on failure.

#ifndef COZ_LIBRARY_H
#define COZ_LIBRARY_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

typedef enum {
	COZ_SYS_GB,
	COZ_SYS_GBC,
	COZ_SYS_GBA,
	COZ_SYS_COUNT,
} coz_system;

#define COZ_ID_LEN 17		// 16 hex digits + NUL
#define COZ_NAME_MAX 200	// game name (No-Intro style), bytes

typedef struct {
	char id[COZ_ID_LEN];
	coz_system sys;
	char name[COZ_NAME_MAX + 1];	// unique per system, e.g. "Example Quest (USA, Europe)"
	char title[COZ_NAME_MAX + 1];	// for display, tags removed: "Example Quest"
	char region[64];		// from the name's first tag: "USA, Europe" (may be empty)
	long long rom_size;
	time_t added;
	int has_save;
	long long save_size;
	time_t save_time;
	int has_save_backup;
	unsigned save_slots;		// bit n-1: save n exists (its save or its backup)
	int has_cover;
	time_t cover_time;		// changes when the cover is replaced
	int nstates;
	time_t last_state;		// newest state, 0 if none
} coz_game;

#define COZ_SAVE_SLOTS 8		// save 1 = the main save

typedef struct {
	int slot;			// 1..COZ_SAVE_SLOTS
	int has_save;			// 0: only a backup is left (deleted)
	long long size;
	time_t time;
	int has_backup;
	time_t backup_time;
} coz_save;

typedef struct {
	char id[24];			// time-based, e.g. "20261121-143205-0"
	time_t time;
	long long size;
	int has_thumb;
} coz_state;

typedef struct {
	coz_game *games;
	int n;
} coz_lib;

// ---- systems
const char *coz_sys_key(coz_system s);		// "gba"
const char *coz_sys_label(coz_system s);	// "Game Boy Advance"
const char *coz_sys_short(coz_system s);	// "GBA"
// From a file name's extension; -1 if not a supported ROM.
int coz_sys_from_filename(const char *filename);

// ---- library
const char *coz_lib_root(void);
// Read all games (sorted by system, then title). Free with coz_lib_free.
int coz_lib_scan(coz_lib *lib);
void coz_lib_free(coz_lib *lib);
const coz_game *coz_lib_find(const coz_lib *lib, const char *id);
// Re-read one game's details after a change (the id stays the same unless renamed).
int coz_game_refresh(coz_game *g);

// ---- adding games
// Import a ROM in steps, so it can be streamed (web uploads):
//   coz_import_begin  -> write the bytes to imp.fd -> coz_import_commit
// The system is detected from `filename` and checked against the ROM header
// on commit (GB vs GBC from the header's colour flag). The game's name is its
// No-Intro name when the ROM's CRC32 is in the lists, else the file name
// without extension. Commit fails with -EEXIST if the game exists
// (unless replace), -EINVAL if the data isn't a ROM for that system.
typedef struct {
	int fd;
	char tmp[512];
	char name[COZ_NAME_MAX + 1];
	int sys;
} coz_import;
int coz_import_begin(coz_import *imp, const char *filename);
int coz_import_commit(coz_import *imp, int replace, coz_game *out);
void coz_import_abort(coz_import *imp);

// Recognise the game by its ROM's CRC32 in the No-Intro lists
// ($COZ_NOINTRO, else /usr/share/cozisboy/nointro/<sys>.txt) and rename it to
// its canonical name ("Example Quest (USA, Europe)"). Imports do
// this by themselves. Returns 1 renamed (g updated), 0 already right, -ENOENT
// not in the lists, -EEXIST another game has that name.
int coz_game_identify(coz_game *g);

// Everything about the game goes: ROM, save, backups, states, cover.
int coz_game_delete(const coz_game *g);
// New name (keeps the system and everything attached). Updates g, new id.
int coz_game_rename(coz_game *g, const char *new_name);

// ---- files the emulator / server open directly (paths are internal: use
// them only to open the file, never to derive other paths)
int coz_game_rom_path(const coz_game *g, char *buf, size_t len);
int coz_game_save_path(const coz_game *g, char *buf, size_t len);	// may not exist yet
int coz_game_save_backup_path(const coz_game *g, char *buf, size_t len);
int coz_game_cover_path(const coz_game *g, char *buf, size_t len);

// ---- battery saves
// The functions without "slot" work on the main save (slot 1).
// Replace the save with new bytes (streamed like an import). The current save,
// if any, becomes the backup.
typedef coz_import coz_save_import;
int coz_save_import_begin(const coz_game *g, coz_save_import *imp);
int coz_save_import_commit(const coz_game *g, coz_save_import *imp);
// The save becomes the backup (the game starts fresh).
int coz_save_delete(const coz_game *g);
// Swap the save and its backup (undo the last replace/delete).
int coz_save_restore_backup(const coz_game *g);

// Numbered saves. List: every save that exists (save or backup), by number;
// the main save is always listed (has_save 0 if the game was never saved).
// Returns the count (up to COZ_SAVE_SLOTS) or -errno.
int coz_saves_list(const coz_game *g, coz_save *out);
int coz_save_slot_path(const coz_game *g, int slot, char *buf, size_t len);	// may not exist yet
int coz_save_slot_backup_path(const coz_game *g, int slot, char *buf, size_t len);
int coz_save_slot_import_commit(const coz_game *g, int slot, coz_save_import *imp);
int coz_save_slot_delete(const coz_game *g, int slot);		// save -> backup
int coz_save_slot_restore_backup(const coz_game *g, int slot);
// Save `slot` and its backup are gone for good (not the main save: -EINVAL).
int coz_save_slot_remove(const coz_game *g, int slot);
// Copy save `from` into `to` (to's current save becomes its backup).
int coz_save_slot_copy(const coz_game *g, int from, int to);
// Exchange two saves (each with its backup), e.g. to make save n the main one.
int coz_save_slot_swap(const coz_game *g, int a, int b);
// The lowest unused save number (2..), or -ENOSPC.
int coz_save_slot_free(const coz_game *g);

// ---- cover
int coz_cover_import_begin(const coz_game *g, coz_import *imp);	// PNG bytes
int coz_cover_import_commit(const coz_game *g, coz_import *imp);
int coz_cover_delete(const coz_game *g);
// Decode the cover (any PNG) and scale it to fill w x h (centre-cropped, like
// object-fit: cover) into XRGB8888 pixels. 0 on success, -ENOENT if none.
int coz_cover_load(const coz_game *g, int w, int h, uint32_t *out);

// ---- save states
// Newest first. Free *out with free().
int coz_states_list(const coz_game *g, coz_state **out, int *n);
// Store a state: emulator data + a screenshot (XRGB8888, stride in pixels),
// written as PNG. thumb may be NULL. Fills out (may be NULL).
int coz_state_add(const coz_game *g, const void *data, size_t size,
		  const uint32_t *thumb, int w, int h, int stride, coz_state *out);
int coz_state_delete(const coz_game *g, const char *state_id);
int coz_state_path(const coz_game *g, const char *state_id, char *buf, size_t len);
int coz_state_thumb_path(const coz_game *g, const char *state_id, char *buf, size_t len);

// ---- helpers
// Write XRGB8888 pixels as a PNG file (uncompressed deflate, no zlib).
int coz_png_write(const char *path, const uint32_t *px, int w, int h, int stride);

#endif
