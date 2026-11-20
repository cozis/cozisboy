#!/bin/sh
# Boot the RG35XX SP entirely over USB (FEL): U-Boot + kernel + DTB + initramfs.
# Needs: device in FEL mode (power on with no card in TF1), sunxi-fel from src/sunxi-tools.
set -e
TOP="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="$TOP/output"
WORK="${WORK:-$OUT/fel}"
FEL="$TOP/src/sunxi-tools/sunxi-fel"
DTB="${DTB:-$OUT/images/allwinner/sun50i-h700-anbernic-rg35xx-sp.dtb}"
mkdir -p "$WORK"

# Kernel: gzip (FEL runs at USB full speed, ~1 MB/s)
gzip -9 -c "$OUT/images/Image" > "$WORK/Image.gz"

# Initramfs from the Buildroot target, without kernel image/modules
rm -rf "$WORK/initramfs"; mkdir -p "$WORK/initramfs"
rsync -a --exclude=/boot --exclude=/lib/modules "$OUT/target/" "$WORK/initramfs/"
install -m 755 "$TOP/src/buildroot/fs/cpio/init" "$WORK/initramfs/init"
(cd "$WORK/initramfs" && find . -print0 | cpio --null -o -H newc -R 0:0 --quiet | gzip -9) > "$WORK/initramfs.cpio.gz"

# DTB with the green LED as heartbeat (visible "Linux is alive" signal)
cp "$DTB" "$WORK/board.dtb"
fdtput -t s "$WORK/board.dtb" /leds/led-0 linux,default-trigger heartbeat
fdtput -d "$WORK/board.dtb" /leds/led-0 default-state 2>/dev/null || true

# Boot script with the initramfs size baked in
SIZE=$(printf '0x%x' "$(stat -c%s "$WORK/initramfs.cpio.gz")")
sed "s/\${initrd_size}/$SIZE/" "$TOP/tools/fel/boot.cmd" > "$WORK/boot.cmd"
"$OUT/host/bin/mkimage" -A arm64 -T script -C none -d "$WORK/boot.cmd" "$WORK/boot.scr" >/dev/null

ls -la "$WORK/Image.gz" "$WORK/initramfs.cpio.gz" "$WORK/board.dtb"
"$FEL" ver
# Arm the watchdog (16 s, system reset) last, right before U-Boot runs,
# so a hang anywhere resets back into FEL (with no card inserted).
time "$FEL" -v -p uboot "$OUT/images/u-boot-sunxi-with-spl.bin" \
	write 0x40080000 "$WORK/Image.gz" \
	write 0x4fa00000 "$WORK/board.dtb" \
	write 0x4fc00000 "$WORK/boot.scr" \
	write 0x4ff00000 "$WORK/initramfs.cpio.gz" \
	writel 0x030090b4 1 \
	writel 0x030090b8 0xb1
