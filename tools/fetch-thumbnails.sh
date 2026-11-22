#!/usr/bin/env bash
# Download box art, title screens and gameplay snapshots for every GBA ROM from
# the libretro-thumbnails database. Images are matched by the ROM's No-Intro
# file name, e.g. "Example Quest (USA).gba".
#
#   tools/fetch-thumbnails.sh [rom_dir ...]
#
# ROM folders: the arguments, else $COZ_ROMS (colon separated), else the stock
# card backup. Output, one PNG per game and type:
#   apps/ui/thumbs/boxart/<name>.png   512x512 box art
#   apps/ui/thumbs/title/<name>.png    240x160 title screen
#   apps/ui/thumbs/snap/<name>.png     240x160 gameplay
# plus the converted files the UI loads (cover/*.ppm, screen/*.ppm, see below).
# The art is copyrighted: the folder is gitignored, keep it local.
# Re-running only fetches what is missing; games the database doesn't have are
# listed in thumbs/missing.txt and skipped next time (FORCE=1 retries them).
set -euo pipefail

TOP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${THUMBS:-$TOP/apps/ui/thumbs}"
BASE="https://raw.githubusercontent.com/libretro-thumbnails/Nintendo_-_Game_Boy_Advance/master"
JOBS="${JOBS:-8}"

if [ $# -gt 0 ]; then
	DIRS=("$@")
elif [ -n "${COZ_ROMS:-}" ]; then
	IFS=: read -r -a DIRS <<<"$COZ_ROMS"
else
	DIRS=("$TOP/backup-stock/roms/Roms/GBA")
fi

mkdir -p "$OUT"/{boxart,title,snap}
touch "$OUT/missing.txt"
[ -n "${FORCE:-}" ] && : >"$OUT/missing.txt"

# one game, one image type: download unless present or known missing
fetch() {
	local name="$1" type="$2" dir="$3"
	local out="$OUT/$dir/$name.png"
	[ -s "$out" ] && return 0
	grep -qxF "$dir|$name" "$OUT/missing.txt" && return 0
	# libretro replaces these characters in thumbnail names: & * / : ` < > ? \ |
	local tname="${name//[&*\/:\`<>?\\|]/_}"
	local url="$BASE/$type/$(python3 -I -c 'import sys, urllib.parse; print(urllib.parse.quote(sys.argv[1]))' "$tname").png"
	if curl -sfL --retry 2 --max-time 30 -o "$out.part" "$url"; then
		mv "$out.part" "$out"
		echo "  + $dir: $name"
	else
		rm -f "$out.part"
		echo "$dir|$name" >>"$OUT/missing.txt"
		echo "  - $dir: $name (not in database)"
	fi
}
export -f fetch
export OUT BASE

n=0
for d in "${DIRS[@]}"; do
	[ -d "$d" ] || { echo "skip: $d (not a folder)" >&2; continue; }
	for rom in "$d"/*.gba "$d"/*.GBA; do
		[ -e "$rom" ] || continue
		name="$(basename "$rom")"
		name="${name%.*}"
		printf '%s\0%s\0%s\0' "$name" Named_Boxarts boxart "$name" Named_Titles title \
			"$name" Named_Snaps snap
		n=$((n + 1))
	done
done | xargs -0 -n 3 -P "$JOBS" bash -c 'fetch "$1" "$2" "$3"' _

# Ready-to-use files for the UI (raw PPM, no PNG decoder needed on the device):
#   cover/<name>.ppm   160x160 from the box art
#   screen/<name>.ppm  240x160 from the gameplay snapshot, else the title screen
mkdir -p "$OUT/cover" "$OUT/screen"
for png in "$OUT"/boxart/*.png; do
	[ -e "$png" ] || continue
	ppm="$OUT/cover/$(basename "$png" .png).ppm"
	[ "$ppm" -nt "$png" ] || magick "$png" -resize 160x160 -background black -gravity center \
		-extent 160x160 "$ppm" 2>/dev/null
done
for png in "$OUT"/snap/*.png "$OUT"/title/*.png; do
	[ -e "$png" ] || continue
	ppm="$OUT/screen/$(basename "$png" .png).ppm"
	case "$png" in */title/*) [ -e "$ppm" ] && continue ;; esac # snapshots win
	[ "$ppm" -nt "$png" ] || magick "$png" -resize '240x160!' "$ppm" 2>/dev/null
done

echo "done: $(ls "$OUT/boxart" | wc -l) box arts, $(ls "$OUT/title" | wc -l) title screens," \
	"$(ls "$OUT/snap" | wc -l) snapshots in $OUT"
missing=$(wc -l <"$OUT/missing.txt")
[ "$missing" -gt 0 ] && echo "$missing images not in the database (see $OUT/missing.txt)"
exit 0
