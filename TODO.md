# cozisboy TODO

State (2026-11-21): mainline Linux 7.3.5 + U-Boot 2026.10 boot from TF1; display,
splash + jingle, USB networking (SSH 10.55.0.2), buttons, read-only root with a
writable `/data` partition. Reset -> logo ~3 s (2.48 s after chip start).

Current boot timeline (chip start = 0):

| Stage | At | Took |
|---|---|---|
| Boot ROM | 0.11 s | 0.11 s |
| SPL (DRAM + load 784 KB U-Boot FIT) | 0.43 s | 0.33 s |
| TF-A | 0.47 s | 0.04 s |
| U-Boot init (driver model 0.18 s) | 0.94 s | 0.47 s |
| Load kernel + DT (6.5 MB LZ4) | 1.38 s | 0.46 s |
| bootm (copy, decompress) | 1.75 s | 0.37 s |
| Linux -> init | +0.43 s | |
| Splash first frame (display on ~0.15 s) | 2.48 s | |

Measure with `/run/splash.log`, `/proc/device-tree/bootstage`, and a wall clock.

## Boot time

- [ ] **Bootloader-drawn logo**: draw the splash in SPL/U-Boot (port TCON/DE33 +
      BOE panel SPI init), hand the running display over to Linux without
      re-init. Logo at ~0.3-0.5 s after reset instead of ~2.5 s. Biggest
      perceived win; largest job.
- [ ] **Why the SPL reads MMC at ~3 MB/s** (U-Boot proper ~18 MB/s, same PIO
      driver). SPL D-cache already on (patch 0003, 1.4 -> 3 MB/s). Check card
      clock/bus width/HS mode actually used in SPL, FIFO access path, AHB clocks.
      If fixed, enable fast boot (below) and the U-Boot FIT load also shrinks.
- [ ] **Enable fast boot (falcon-style)**: implemented and verified, disabled
      (`CONFIG_SUNXI_FAST_BOOT_SECTOR=0x0` in `uboot.fragment`). SPL loads TF-A +
      trampoline + Linux FIT from 1 MiB; Menu held = U-Boot. Only pays off once
      the SPL reads fast. Gotchas: hook sector is relative to the SPL's 8 KiB
      offset; TF-A needs the DT marker in the trampoline.
- [ ] **Trim U-Boot**: driver model init 0.18 s (prune U-Boot DT / drivers,
      `bootph` tags), drop unused commands, network stack (`eth_common_init`),
      USB gadget/UMS if unused. U-Boot init is 0.47 s total.
- [ ] **bootm 0.37 s**: find what remains (copy to `kernel_comp_addr_r`, cache
      flushes, FDT fixups). LZ4 at -O3 made no difference.
- [ ] **SD card speed in U-Boot** (~16 MB/s): check HS/UHS, bus clock.
- [ ] **Smaller kernel** (14 MB raw / 6.5 MB LZ4): every MB ~50 ms. Built-in
      drivers still include generic stuff (e.g. VIRTIO pulled in by something).
- [ ] Linux -> init 0.43 s: SD card detection (~0.3 s), root mount.
- [ ] `/data` mount after a reset needs ext4 journal recovery: keep it out of
      the boot-critical path (mount in background, or use a fs without journal
      replay cost).

## Display / splash / UI

- [ ] **Device runner for the launcher UI** (`apps/ui`): DRM/fbdev output,
      evdev buttons -> `BTN_*`, ALSA sound (alsa-lib, not tinyalsa), started
      when the splash ends; splash leaves its last frame for a flash-free handoff.
- [ ] Splash -> UI handoff protocol (`/run/splash.stop` exists; wire it up).
- [ ] **Volume buttons**: map to the mixer (Line Out / DAC volume), show an OSD.
- [x] PWM backlight: H616 PWM driver from the upstream v4 series (patches
      0018-0021), pwm-backlight 0..255, default 160. Verified 255/15/0/128.
- [ ] Brightness control in the UI (and remember it on /data).
- [ ] Panel waits: tuned to 5/1/5/25/25/0 ms; re-verify on a cold device.
- [ ] Check for horizontal ghosting on the panel (looked fine by eye).

## Hardware

- [x] Wi-Fi (RTL8821CS on mmc1): works (scan OK). Fix: CONFIG_RESET_GPIO must
      be OFF (its reset-gpios fallback only supports 2-cell GPIOs; sunxi has 3).
- [x] Wi-Fi connects at boot in the background (S45wifi: wpa_supplicant +
      udhcpc), IP ~10 s after power-on. Add networks with
      `ssh -t root@<ip> cozisboy-wifi-add "SSID"` (hidden prompt, PSK hash in
      /data/wifi). Reconnects after sleep.
- [ ] Wi-Fi: first scan takes ~5 s (association at ~9 s): remember the last
      channel (`scan_freq`) for a faster reconnect; Wi-Fi page in the UI.
- [x] Clock: system time from the battery-backed PCF8563 at boot (S01clock,
      driver built in), busybox ntpd once Wi-Fi is up writes it back.
- [x] GPU (Panfrost): kernel side works (Mali-G31, /dev/dri/renderD128). Fix:
      CONFIG_SUN50I_H6_PRCM_PPU (GPU power domain) was missing.
- [ ] GPU userspace: Mesa (panfrost gallium, GLES) for emulators/UI.
- [x] Battery / charging readout (AXP717): /sys/class/power_supply/axp20x-battery
      (capacity, status, voltage, current); power button = axp20x-pek input.
- [ ] Low-battery handling and power-button actions in the UI.
- [x] Sleep: `cozisboy-sleep` (s2idle; no deep suspend without SCP firmware):
      Wi-Fi unloaded (rtw88 refuses to suspend), backlight off, wakes on the
      power button, USB gadget re-attached via soft_connect.
- [ ] Hook sleep to the power button / lid in the UI; measure s2idle drain.
- [ ] TF2 slot (`mmc2`?) not described in the mainline device tree.
- [x] Rumble motor: gpio-vibrator on PH2, force feedback via evdev (FF_RUMBLE).
- [ ] HDMI out.

## System

- [x] File transfer over SSH: scp, sftp (file managers: `sftp://root@<ip>/data`)
      and rsync. ~8.7 MB/s over USB, ~2.5 MB/s over Wi-Fi.
- [ ] Wi-Fi throughput only ~2.5 MB/s at 72 Mbit/s link (rtw88 SDIO): check
      SDIO clock/bus width, 5 GHz.
- [x] `/data` grows to the whole card on first boot (S03data: sfdisk keeps the
      GPT at 1008K, resizepart, online resize2fs in the background, ~4 min
      for 58 GB). Image's data.ext4 uses 4 KiB blocks.
- [ ] U-Boot USB disk mode (Menu at power-on -> `ums`) never triggered.
- [ ] `tools/deploy.sh` causes ext4 orphan recovery on the next boot (replaces
      running binaries); stop services first or reboot into a maintenance mode.
- [ ] Drop the fast-boot FIT from the SD image while fast boot is disabled
      (saves writing 14 MB per flash).
- [ ] Remove tinyalsa (no longer used; tinyalsa 2.0 never started the stream
      on this codec).

## Web app (apps/web) + game library (apps/common/library.h)

- [x] Game library: one folder per game (ROM, save + backup, states with PNG
      screenshots, cover), stable ids, GB/GBC detected from the ROM header.
- [x] Web app prototype: pairing code, cover grid, drop ROMs (covers fetched
      by the browser from libretro-thumbnails), game page with battery save
      (replace/delete keep a backup, restore), save-state gallery, zip backup.
- [ ] Started at boot for now (S60web; code in /run/cozisboy-web.code): the
      launcher should switch it on/off and show the pairing code.
- [ ] Launcher: move to the library (rescan when active, states via
      coz_state_add).
- [x] Discover: Homebrew Hub catalogue (GB/GBC/GBA games) through a relay on
      the device (/api/hh/*, its JSON API isn't readable cross-origin; libcurl
      + CA bundle, cached 1 h), install = browser downloads ROM + cover.
- [ ] Recognise ROMs by checksum (libretro-database No-Intro DATs): proper
      names, covers, year/genre/players.
- [ ] Launcher: browse/install from Homebrew Hub on the device itself.
- [ ] Live screen over WebSocket (JPEG), remote input.

## Games

- [ ] RetroArch + cores (needs Panfrost), ROM browser in the launcher.
