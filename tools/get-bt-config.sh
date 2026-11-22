#!/bin/sh
# Copy the RTL8821CS Bluetooth config out of a backup of the stock Anbernic
# firmware (not redistributed here): the stock root filesystem keeps it as
# /lib/firmware/rtlbt/rtl8821c_config, mainline btrtl wants it as
# rtl_bt/rtl8821cs_config.bin. Without it Bluetooth doesn't start.
#
# usage: tools/get-bt-config.sh [stock-rootfs.img | stock-rootfs.img.zst]
#        (default: backup-stock/mmcblk0p5.img.zst, the stock root partition)
set -e
top=$(cd "$(dirname "$0")/.." && pwd)
src=${1:-$top/backup-stock/mmcblk0p5.img.zst}
out=$top/br2-external/board/rg35xx-sp/rootfs-overlay/lib/firmware/rtl_bt/rtl8821cs_config.bin
img=$src
case "$src" in *.zst)
	img=$(mktemp)
	trap 'rm -f "$img"' EXIT
	zstd -dqf "$src" -o "$img" ;;
esac
mkdir -p "$(dirname "$out")"
debugfs -R "dump /lib/firmware/rtlbt/rtl8821c_config $out" "$img" 2>/dev/null
[ -s "$out" ] || { echo "not found in $src" >&2; exit 1; }
echo "wrote $out ($(wc -c < "$out") bytes)"
