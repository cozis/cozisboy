# Builds of the cozisboy UI, for the PC and for the console.
#   make run     open the preview window (SDL2)
#   make cozisboy-shot    headless screenshot tool (see host_shot.c)
#   make sounds.wav   render every sound effect to a WAV file
#   make frames  freeze frames of the UI states in frames.txt (art/frames/)
#   make device  the console build (cozisboy-ui), cross-compiled with the
#                Buildroot toolchain (build the image once first: libmgba, ALSA)
#   make deploy  device build -> copied to the console over SSH and restarted
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -Wno-format-truncation -std=c11 -D_DEFAULT_SOURCE -I../common
CORE = ui.c games.c net.c pclink.c clock.c sys_mock.c emu.c gfx.c ../common/sfx.c ../common/library.c
HDRS = ui.h ui_priv.h gfx.h emu.h sys.h ../common/sfx.h ../common/library.h

all: cozisboy-preview cozisboy-shot sfx-demo

cozisboy-preview: host_sdl.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ host_sdl.c $(CORE) $(shell pkg-config --cflags --libs sdl2) -lm -lmgba -lpng -lz

cozisboy-shot: host_shot.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ host_shot.c $(CORE) -lm -lmgba -lpng -lz

sfx-demo: sfx_demo.c ../common/sfx.c ../common/sfx.h
	$(CC) $(CFLAGS) -o $@ sfx_demo.c ../common/sfx.c -lm

# every sound effect in a row, as a WAV file
sounds.wav: sfx-demo
	./sfx-demo $@

# freeze frames of every UI state in frames.txt -> art/frames/
frames: cozisboy-shot
	./frames.sh

# icon design sheet (all Games / Settings icon variations) -> art/frames/icons.png
icons: cozisboy-shot
	./cozisboy-shot k/tmp/cozisboy-icons && magick /tmp/cozisboy-icons.ppm -scale 150% ../../art/frames/icons.png

# The PC preview reads games from a game library (apps/common/library.h) like
# the console. `make library` makes one in ./library from a folder of .gba
# files: ROMs are linked, not copied; a .sav next to a ROM is copied once.
PC_LIBRARY ?= library
ROMS ?= ../../backup-stock/roms/Roms/GBA
export COZ_LIBRARY ?= $(PC_LIBRARY)

library:
	@for f in "$(ROMS)"/*.gba; do \
		n=$$(basename "$$f" .gba); d="$(PC_LIBRARY)/gba/$$n"; \
		mkdir -p "$$d"; ln -sfn "$$(realpath "$$f")" "$$d/rom.gba"; \
		s="$${f%.gba}.sav"; [ -f "$$s" ] && [ ! -e "$$d/save.sav" ] && cp "$$s" "$$d/save.sav"; \
	done; echo "$(PC_LIBRARY): $$(ls "$(PC_LIBRARY)/gba" | wc -l) games"

run: cozisboy-preview
	./cozisboy-preview

# ---- console (RG35XX SP) ----------------------------------------------------
# Same UI code; the device front end (framebuffer, evdev, ALSA) and the real
# Wi-Fi/SSH backend replace SDL and the mock. Buildroot's package builds this
# target too (with its own DEV_CC / DEV_EXTRA_CFLAGS).
BR_OUT ?= ../../output
DEV_CC ?= $(BR_OUT)/host/bin/aarch64-linux-gcc
DEV_CFLAGS = -O2 -Wall -Wextra -Wno-format-truncation -std=c11 -D_DEFAULT_SOURCE -I../common $(DEV_EXTRA_CFLAGS)
DEV_SRCS = host_dev.c sys_dev.c $(filter-out sys_mock.c,$(CORE))
DEVICE ?= root@10.55.0.2
SSH_OPTS ?= -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR

device: cozisboy-ui

cozisboy-ui: $(DEV_SRCS) $(HDRS)
	$(DEV_CC) $(DEV_CFLAGS) -o $@ $(DEV_SRCS) $(DEV_LDFLAGS) -lmgba -lasound -lpng -lz -lpthread -lm

# Replace the launcher on the console (root is read-only: remount for the copy);
# init restarts it.
deploy: cozisboy-ui
	ssh $(SSH_OPTS) $(DEVICE) 'mount -o remount,rw / && cat > /usr/bin/cozisboy-ui.new && \
		chmod 755 /usr/bin/cozisboy-ui.new && mv /usr/bin/cozisboy-ui.new /usr/bin/cozisboy-ui && \
		sync; mount -o remount,ro / 2>/dev/null; killall cozisboy-ui; true' < cozisboy-ui
	@echo "deployed to $(DEVICE)"

clean:
	rm -f cozisboy-preview cozisboy-shot sfx-demo sounds.wav cozisboy-ui

.PHONY: all run clean frames icons device deploy library
