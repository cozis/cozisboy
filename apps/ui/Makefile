# PC builds of the cozisboy UI.
#   make run     open the preview window (SDL2)
#   make cozisboy-shot    headless screenshot tool (see host_shot.c)
#   make sounds.wav   render every sound effect to a WAV file
#   make frames  freeze frames of the UI states in frames.txt (art/frames/)
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -D_DEFAULT_SOURCE -I../common
CORE = ui.c games.c net.c sys_mock.c emu.c gfx.c ../common/sfx.c
HDRS = ui.h ui_priv.h gfx.h emu.h sys.h libretro_min.h ../common/sfx.h

all: cozisboy-preview cozisboy-shot sfx-demo

cozisboy-preview: host_sdl.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ host_sdl.c $(CORE) $(shell pkg-config --cflags --libs sdl2) -lm -ldl

cozisboy-shot: host_shot.c $(CORE) $(HDRS)
	$(CC) $(CFLAGS) -o $@ host_shot.c $(CORE) -lm -ldl

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

run: cozisboy-preview
	./cozisboy-preview

clean:
	rm -f cozisboy-preview cozisboy-shot sfx-demo sounds.wav

.PHONY: all run clean frames icons
