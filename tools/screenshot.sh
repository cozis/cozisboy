#!/bin/sh
# Grab the RG35XX SP framebuffer over SSH and save it as PNG.
# Shows what Linux draws (fb0 memory), not what the panel physically displays.
# Usage: tools/screenshot.sh [out.png]   (default: output/screenshots/<time>.png)
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
HOST="${HOST:-root@10.55.0.2}"
OUT="${1:-$TOP/output/screenshots/$(date +%Y%m%d-%H%M%S).png}"
mkdir -p "$(dirname "$OUT")"
ssh "$HOST" 'set -e; f=/sys/class/graphics/fb0; echo "$(cat $f/virtual_size),$(cat $f/stride),$(cat $f/bits_per_pixel)"; cat /dev/fb0' > "$OUT.raw"
python3 - "$OUT.raw" "$OUT" <<'PY'
import sys
from PIL import Image
raw = open(sys.argv[1], 'rb').read()
hdr, data = raw.split(b'\n', 1)
w, h, stride, bpp = map(int, hdr.split(b','))
assert bpp == 32, bpp
img = Image.frombuffer('RGB', (w, h), data[:stride * h], 'raw', 'BGRX', stride, 1)
img.save(sys.argv[2])
print(sys.argv[2], f'{w}x{h}')
PY
rm -f "$OUT.raw"
