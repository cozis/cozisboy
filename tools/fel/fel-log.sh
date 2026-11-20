#!/bin/sh
# After the device reset back into FEL: re-init DRAM with the SPL and read the
# ramoops area (0x7c000000, 1 MiB) left by the previous kernel. Prints the text.
set -e
TOP="$(cd "$(dirname "$0")/../.." && pwd)"
FEL="$TOP/src/sunxi-tools/sunxi-fel"
OUT="${1:-$TOP/output/fel/ramoops.bin}"
"$FEL" spl "$TOP/output/images/u-boot-sunxi-with-spl.bin"
"$FEL" read 0x7c000000 0x100000 "$OUT"
# ramoops records: "PRSZ" header + text; print printable runs
python3 - "$OUT" <<'PY'
import sys,re
d=open(sys.argv[1],'rb').read()
print("PRSZ headers at:", [hex(m.start()) for m in re.finditer(b'PRSZ', d)][:8])
txt=re.sub(rb'[^\x09\x0a\x20-\x7e]+', b'\n', d).decode()
lines=[l for l in txt.splitlines() if len(l.strip())>3]
print("\n".join(lines))
PY
