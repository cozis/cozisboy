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
| `common/sfx.{c,h}` | Chiptune sound engine shared by both programs. Call `sfx_init(rate)`, then `sfx_render(out, frames)` from the audio thread and `sfx_play(id, param)` from anywhere (a lock-free queue sits between them). The boot jingle is `sfx_play(SFX_BOOT, SFX_BOOT_DEFAULT)`, started on the first splash frame. |
| `../art/logo/` | Logo (`cozisboy.svg/.png`), static 640x480 splash (`cozisboy-splash.png`), animation with jingle (`cozisboy-boot.mp4`), generator `gen_logo.py`. |

## Device notes

- **Display:** any 640x480 XRGB8888 framebuffer works: a DRM dumb buffer or `/dev/fb0`. Pass `stride_px` = pitch / 4.
- **Buttons:** map the `gpio-keys-gamepad` evdev key codes to the `BTN_*` mask and pass the held state every frame.
- **Audio timing:** the jingle starts at about 1.1 s. With `CONFIG_SND*=m` in `linux.fragment`, the codec only appears around 2.2 s, so the jingle would be silent. Either build the sound driver into the kernel or let the splash skip audio when it isn't ready.
- **Startup order:** the splash should start first, before `rcS`, with startup scripts advancing its progress. The UI then replaces the `getty` line in `inittab` with a line that restarts it if it quits (`::respawn:`), and the splash fades out when the UI takes over the screen.
