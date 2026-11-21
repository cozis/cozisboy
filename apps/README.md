# cozisboy apps: splash, UI, sounds

Plain C, no dependencies. Everything renders into a 640x480 XRGB8888 buffer;
sounds are synthesized (mono int16). Only PC preview front ends exist so far:
the device front ends (DRM output, evdev input, ALSA audio) are not written yet.

| Path | What |
|---|---|
| `splash/splash_anim.{c,h}` | Boot animation. `splash_render(buf, w, h, stride_px, t, progress)`: `t` = seconds since start, `progress` 0..1 drives the bar. Letters land 0.3-0.93 s, shine 1.2-1.55 s, then an idle wave loop. |
| `splash/preview.c` | PC only: renders frames to stdout. `make boot.mp4` renders the animation with its jingle. |
| `ui/ui.{c,h}`, `ui/gfx.{c,h}` | Launcher UI. `ui_init()`, then each frame `ui_frame(buf, stride_px, dt, held_buttons)` (`BTN_*` bitmask in `ui.h`). Handles key repeat, animations and sounds itself. |
| `ui/host_sdl.c` | PC preview window (`cd ui && make run`). Keys: arrows, Z = A, X = B, Q/W = L/R. |
| `ui/host_shot.c` | Headless scripted screenshots (PC). |
| `common/library.{c,h}` | **Game library**: games, battery saves, save states, covers. The only code that knows how they're stored (see below). |
| `common/coz-lib.c` | Command-line tool for the library (`list`, `add FILE...`, `delete`, `rename`, `states`). |
| `web/` | Web app server (`cozisboy-web`): manage games and saves from a browser, built on the library. |
| `common/sfx.{c,h}` | Chiptune sound engine shared by both programs. Call `sfx_init(rate)`, then `sfx_render(out, frames)` from the audio thread and `sfx_play(id, param)` from anywhere (a lock-free queue sits between them). The boot jingle is `sfx_play(SFX_BOOT, SFX_BOOT_DEFAULT)`, started on the first splash frame. |
| `../art/logo/` | Logo (`cozisboy.svg/.png`), static 640x480 splash (`cozisboy-splash.png`), animation with jingle (`cozisboy-boot.mp4`), generator `gen_logo.py`. |

## Device notes

- **Display:** any 640x480 XRGB8888 framebuffer works: a DRM dumb buffer or `/dev/fb0`. Pass `stride_px` = pitch / 4.
- **Buttons:** map the `gpio-keys-gamepad` evdev key codes to the `BTN_*` mask and pass the held state every frame.
- **Audio timing:** the jingle starts at about 1.1 s. With `CONFIG_SND*=m` in `linux.fragment`, the codec only appears around 2.2 s, so the jingle would be silent. Either build the sound driver into the kernel or let the splash skip audio when it isn't ready.
- **Startup order:** the splash should start first, before `rcS`, with startup scripts advancing its progress. The UI then replaces the `getty` line in `inittab` with a line that restarts it if it quits (`::respawn:`), and the splash fades out when the UI takes over the screen.

## Game library (`common/library.h`)

Games, saves and save states go through `library.h`; no program builds paths to
them. On the device the library lives under `/data/library` (`$COZ_LIBRARY`
overrides it, e.g. on the PC), shared by the launcher and the web app.

- **Games:** `coz_lib_scan()` lists every game with a stable `id`, `sys`
  (GB/GBC/GBA), `name` (No-Intro, unique per system), display `title` and
  `region`, and whether it has a save, states and a cover. To run one, open
  `coz_game_rom_path()`.
- **Battery save:** the emulator reads and writes `coz_game_save_path()`
  directly (it may not exist yet). The web app replaces saves through the
  library, which keeps the previous one as a backup.
- **Save states:** stored with `coz_state_add(game, data, size, pixels, w, h,
  stride)`: the library names them by time (no slots) and writes the
  screenshot as a PNG. `coz_states_list()` returns them newest first;
  read one with `coz_state_path()`, delete with `coz_state_delete()`.
- **Covers:** `coz_game_cover_path()` (PNG, from libretro-thumbnails), when
  `has_cover`.
- Paths returned by the library are for opening that one file only, never to
  derive other paths: the layout may change.
- The library isn't thread-safe per `coz_lib`; the launcher should rescan
  (`coz_lib_scan`) when it becomes active, since the web app may have added
  or removed games meanwhile.
