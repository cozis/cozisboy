#!/usr/bin/env python3
"""Build the console's ROM name lists from the No-Intro DATs in libretro-database.

Writes <out>/{gb,gbc,gba}.txt: one "CRC32 name" line per ROM (CRC as 8 upper-
case hex digits, sorted), the canonical No-Intro name without the extension,
e.g. "9D4F1E18 007 - Everything or Nothing (USA, Europe) (En,Fr,De)".
The library looks a ROM's CRC up there when it's imported (library.c), and
the web app finds box art by that name (libretro-thumbnails).

usage: tools/gen-nointro.py [out-dir]   (default: the rootfs overlay)
"""
import os, re, sys, urllib.parse, urllib.request

BASE = "https://raw.githubusercontent.com/libretro/libretro-database/master/metadat/no-intro/"
SYSTEMS = {"gb": "Nintendo - Game Boy.dat", "gbc": "Nintendo - Game Boy Color.dat",
           "gba": "Nintendo - Game Boy Advance.dat"}
here = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(
    here, "..", "br2-external", "board", "rg35xx-sp", "rootfs-overlay", "usr", "share", "cozisboy", "nointro")
os.makedirs(out, exist_ok=True)
ROM = re.compile(r'rom \( name "(.+?)\.(?:gb|gbc|gba|sgb|cgb)" size \d+ crc ([0-9A-Fa-f]{8})')
for key, dat in SYSTEMS.items():
    req = urllib.request.Request(BASE + urllib.parse.quote(dat), headers={"User-Agent": "cozisboy-build"})
    text = urllib.request.urlopen(req, timeout=60).read().decode("utf-8", "replace")
    seen = {}
    for name, crc in ROM.findall(text):
        if "/" in name or "\n" in name:
            continue
        seen.setdefault(crc.upper(), name)  # first name wins for duplicate dumps
    with open(os.path.join(out, key + ".txt"), "w", encoding="utf-8") as f:
        for crc in sorted(seen):
            f.write(f"{crc} {seen[crc]}\n")
    print(f"{key}: {len(seen)} ROMs")
