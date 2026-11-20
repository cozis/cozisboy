#!/bin/sh
# Update the running RG35XX SP over SSH from the Buildroot output, then reboot.
#   tools/deploy.sh kernel   -> /boot/Image + device tree
#   tools/deploy.sh rootfs   -> all of output/target except /boot
#   tools/deploy.sh all      -> both (default)
# Add NOREBOOT=1 to skip the reboot.
set -e
TOP="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$TOP/output"
HOST="${HOST:-root@10.55.0.2}"
DTB=allwinner/sun50i-h700-anbernic-rg35xx-sp.dtb
WHAT="${1:-all}"

case "$WHAT" in kernel|rootfs|all) ;; *) echo "usage: $0 [kernel|rootfs|all]" >&2; exit 2;; esac

if [ "$WHAT" = rootfs ] || [ "$WHAT" = all ]; then
	echo ">>> rootfs"
	tar -C "$OUT/target" --exclude=./boot --exclude=./dev --exclude=./proc \
		--exclude=./sys --exclude=./tmp --exclude=./run --exclude=./etc/dropbear \
		--owner=0 --group=0 -czf - . | ssh "$HOST" 'gzip -dc | tar -C / -xf -'
fi
if [ "$WHAT" = kernel ] || [ "$WHAT" = all ]; then
	echo ">>> kernel + dtb"
	# dropbear has no sftp-server, so stream with cat instead of scp
	ssh "$HOST" 'cat > /boot/Image.new' < "$OUT/images/Image"
	ssh "$HOST" "cat > /boot/$DTB.new" < "$OUT/images/$DTB"
	ssh "$HOST" "mv /boot/Image.new /boot/Image && mv /boot/$DTB.new /boot/$DTB"
fi
ssh "$HOST" sync
if [ -z "$NOREBOOT" ]; then
	echo ">>> reboot"
	ssh "$HOST" 'reboot' || true
fi
