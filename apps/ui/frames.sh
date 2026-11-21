#!/bin/sh
# Render every state in frames.txt to art/frames/<name>.png plus sheet.png.
#   ./frames.sh [filter]   only frames whose name contains <filter>
set -e
cd "$(dirname "$0")"
OUT=../../art/frames
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"
export COZ_CLOCK=09:41
export COZ_ROMS= # freeze frames use the built-in mock games
TWO=$(sed -n 's/^@TWO *= *//p' frames.txt)
grep -v '^\s*#' frames.txt | grep '|' | while IFS='|' read -r name script; do
	name=$(echo "$name" | tr -d ' ')
	case "$name" in *"$1"*) ;; *) continue ;; esac
	script=$(echo "$script" | sed "s/@TWO/$TWO/g")
	# shellcheck disable=SC2086
	./cozisboy-shot $script "s$TMP/$name"
	magick "$TMP/$name.ppm" "$OUT/$name.png"
	echo "  $name"
done
cd "$OUT"
magick montage -label '%t' -font DejaVu-Sans -pointsize 16 -fill white -background '#1a1a1a' \
	-geometry 320x240+8+8 -tile 4x [0-9]*.png sheet.png
echo "sheet: $(pwd)/sheet.png"
