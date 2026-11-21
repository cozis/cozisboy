#!/bin/sh
# Generate extlinux.conf and genimage.cfg with a fresh root PARTUUID.
set -e
BOARD_DIR="$(dirname "$0")"
PARTUUID="$("$HOST_DIR/bin/uuidgen")"

install -d "$TARGET_DIR/boot/extlinux"
sed "s/%PARTUUID%/$PARTUUID/g" "$BOARD_DIR/extlinux.conf" > "$TARGET_DIR/boot/extlinux/extlinux.conf"
sed "s/%PARTUUID%/$PARTUUID/g" "$BOARD_DIR/genimage.cfg" > "$BINARIES_DIR/genimage.cfg"

# Kernel as LZ4 *frame* format (U-Boot's decompressor rejects the kernel's
# own Image.lz4, which uses the legacy format). Independent blocks (default).
"$HOST_DIR/bin/lz4" -q -9 -f "$BINARIES_DIR/Image" "$BINARIES_DIR/Image.lz4"
install -m 644 "$BINARIES_DIR/Image.lz4" "$TARGET_DIR/boot/Image.lz4"
rm -f "$TARGET_DIR/boot/Image"

# Not needed on the handheld, and each one costs boot time
rm -f "$TARGET_DIR"/etc/init.d/S01seedrng "$TARGET_DIR"/etc/init.d/S01syslogd \
	"$TARGET_DIR"/etc/init.d/S02klogd "$TARGET_DIR"/etc/init.d/S50crond

# OpenSSH is only here for sftp-server (dropbear serves SSH)
rm -f "$TARGET_DIR"/etc/init.d/S50sshd "$TARGET_DIR"/usr/sbin/sshd \
	"$TARGET_DIR"/usr/libexec/sshd-session "$TARGET_DIR"/usr/libexec/sshd-auth \
	"$TARGET_DIR"/usr/libexec/ssh-keysign "$TARGET_DIR"/usr/libexec/sshd-keygen-wrapper

# SSH host keys live on the writable data partition (generated once, kept)
rm -rf "$TARGET_DIR/etc/dropbear"
ln -s /data/dropbear "$TARGET_DIR/etc/dropbear"
mkdir -p "$TARGET_DIR/data"

# SSH key login only
chmod 700 "$TARGET_DIR/root" "$TARGET_DIR/root/.ssh"
chmod 600 "$TARGET_DIR/root/.ssh/authorized_keys"
