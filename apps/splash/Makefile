# PC preview of the boot splash.
#   make boot.mp4   render the animation with its jingle
CFLAGS ?= -O2 -g
CFLAGS += -Wall -Wextra -std=c11 -I../common

preview: preview.c splash_anim.c splash_anim.h ../common/sfx.c ../common/sfx.h
	$(CC) $(CFLAGS) -o $@ preview.c splash_anim.c ../common/sfx.c -lm

boot.mp4: preview
	./preview 4 30 jingle.wav | ffmpeg -loglevel error -y -f rawvideo -pix_fmt rgb24 \
		-s 640x480 -r 30 -i - -i jingle.wav -c:v libx264 -pix_fmt yuv420p -crf 12 \
		-c:a aac -b:a 160k -shortest $@

# all jingle variants back to back, labelled
VARIANTS = 0 1 2 3 4 5
NAMES = classic bubbly fanfare cozy minimal drums
variations.mp4: preview
	set -e; i=1; for v in $(VARIANTS); do \
		name=$$(echo $(NAMES) | cut -d' ' -f$$i); \
		./preview 3.2 30 jingle-$$v.wav $$v | ffmpeg -loglevel error -y -f rawvideo \
			-pix_fmt rgb24 -s 640x480 -r 30 -i - -i jingle-$$v.wav \
			-vf "drawtext=text='$$i  $$name':fontcolor=white:fontsize=28:x=20:y=h-50" \
			-c:v libx264 -pix_fmt yuv420p -crf 14 -c:a aac -b:a 160k -shortest var-$$v.mp4; \
		echo "file 'var-$$v.mp4'" >> list.txt.tmp; i=$$((i+1)); done; \
	mv list.txt.tmp list.txt; ffmpeg -loglevel error -y -f concat -safe 0 -i list.txt -c copy $@; \
	rm -f list.txt

clean:
	rm -f preview jingle*.wav boot.mp4 var-*.mp4 variations.mp4

.PHONY: clean
