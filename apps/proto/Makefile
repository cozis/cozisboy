# Link-cable prototype (PC): two libmgba emulators wired together.
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=gnu11 -DWITH_SDL

linktest: linktest.c
	$(CC) $(CFLAGS) -o $@ $< $(shell pkg-config --cflags --libs sdl2) -lmgba -lpthread

clean:
	rm -f linktest *.ppm *.sav

.PHONY: clean
