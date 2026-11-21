#!/bin/sh
# Build the fast-boot FIT (TF-A + trampoline + Linux + DT), then the SD image.
set -e
BOARD_DIR="$(dirname "$0")"
FB="$BINARIES_DIR/fastboot"
mkdir -p "$FB"

# Trampoline (BL33 stand-in that jumps to Linux with x0 = device tree)
"$HOST_DIR/bin/aarch64-linux-gcc" -c -DKERNEL_ADDR=0x40200000 ${FASTBOOT_DEBUG_LED:+-DDEBUG_LED} ${FASTBOOT_DEBUG_BLINK:+-DDEBUG_BLINK} \
	-o "$FB/trampoline.o" "$BOARD_DIR/fastboot/trampoline.S"
"$HOST_DIR/bin/aarch64-linux-ld" -Ttext=0x4a000000 -o "$FB/trampoline.elf" "$FB/trampoline.o"
"$HOST_DIR/bin/aarch64-linux-objcopy" -O binary "$FB/trampoline.elf" "$FB/trampoline.bin"

# Device tree with what U-Boot would otherwise add at boot: memory + cmdline
DTB="$BINARIES_DIR/allwinner/sun50i-h700-anbernic-rg35xx-sp.dtb"
cp "$DTB" "$FB/fastboot.dtb"
"$HOST_DIR/bin/fdtput" -c "$FB/fastboot.dtb" /memory@40000000
"$HOST_DIR/bin/fdtput" -t s "$FB/fastboot.dtb" /memory@40000000 device_type memory
"$HOST_DIR/bin/fdtput" -t x "$FB/fastboot.dtb" /memory@40000000 reg 0 0x40000000 0 0x40000000
"$HOST_DIR/bin/fdtput" -t s "$FB/fastboot.dtb" /chosen bootargs "$(cat "$BOARD_DIR/cmdline.txt")"

cp "$BINARIES_DIR/bl31.bin" "$BINARIES_DIR/Image" "$FB/"
cp "$BOARD_DIR/fastboot/fastboot.its" "$FB/"
(cd "$FB" && "$HOST_DIR/bin/mkimage" -E -f fastboot.its fastboot.itb >/dev/null)
cp "$FB/fastboot.itb" "$BINARIES_DIR/fastboot.itb"
ls -l "$BINARIES_DIR/fastboot.itb"

exec support/scripts/genimage.sh -c "$BINARIES_DIR/genimage.cfg"
