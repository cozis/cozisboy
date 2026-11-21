#!/bin/sh
# Update the running RG35XX SP over SSH from the Buildroot output, then reboot.
#   tools/deploy.sh kernel   -> /boot/Image + device tree
#   tools/deploy.sh rootfs   -> all of output/target except /boot
#   tools/deploy.sh uboot    -> bootloader (written to the card at 8 KiB)
#   tools/deploy.sh all      -> kernel + rootfs (default; not the bootloader)
# Add NOREBOOT=1 to skip the reboot.
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$TOP/output"
HOST="${HOST:-root@10.55.0.2}"
# The device regenerates its SSH host key at every boot (read-only root, keys in
# RAM) and is only reachable over the USB link: don't pin the host key.
SSH_OPTS="-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR"
DTB=allwinner/sun50i-h700-anbernic-rg35xx-sp.dtb
WHAT="${1:-all}"

case "$WHAT" in kernel|rootfs|uboot|all) ;; *) echo "usage: $0 [kernel|rootfs|uboot|all]" >&2; exit 2;; esac

# The root filesystem is read-only at runtime; make it writable for the update.
ssh $SSH_OPTS "$HOST" 'mount -o remount,rw /'
trap 'ssh $SSH_OPTS "$HOST" "sync; mount -o remount,ro / 2>/dev/null" || true' EXIT

if [ "$WHAT" = rootfs ] || [ "$WHAT" = all ]; then
	echo ">>> rootfs"
	tar -C "$OUT/target" --exclude=./boot --exclude=./dev --exclude=./proc \
		--exclude=./sys --exclude=./tmp --exclude=./run --exclude=./etc/dropbear \
		--owner=0 --group=0 -czf - . | ssh $SSH_OPTS "$HOST" 'gzip -dc | tar -C / -xf -'
fi
if [ "$WHAT" = kernel ] || [ "$WHAT" = all ]; then
	echo ">>> kernel + dtb"
	# dropbear has no sftp-server, so stream with cat instead of scp
	ssh $SSH_OPTS "$HOST" 'cat > /boot/Image.lz4.new' < "$OUT/images/Image.lz4"
	ssh $SSH_OPTS "$HOST" "cat > /boot/$DTB.new" < "$OUT/images/$DTB"
	ssh $SSH_OPTS "$HOST" "mv /boot/Image.lz4.new /boot/Image.lz4 && mv /boot/$DTB.new /boot/$DTB"
fi
if [ "$WHAT" = uboot ]; then
	echo ">>> u-boot (SPL + TF-A + U-Boot at 8 KiB on mmcblk0)"
	U="$OUT/images/u-boot-sunxi-with-spl.bin"
	[ "$(stat -c%s "$U")" -lt $((1000 * 1024)) ] || { echo "u-boot too big for the 1000 KiB slot" >&2; exit 1; }
	ssh $SSH_OPTS "$HOST" 'cat > /tmp/u-boot.bin && dd if=/tmp/u-boot.bin of=/dev/mmcblk0 bs=1024 seek=8 conv=fsync && rm /tmp/u-boot.bin' < "$U"
fi
ssh $SSH_OPTS "$HOST" sync
if [ -z "$NOREBOOT" ]; then
	trap - EXIT
	ssh $SSH_OPTS "$HOST" 'mount -o remount,ro /' || true
	echo ">>> reboot"
	ssh $SSH_OPTS "$HOST" 'reboot' || true
fi
