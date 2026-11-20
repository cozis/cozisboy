#!/bin/sh
# Generate extlinux.conf and genimage.cfg with a fresh root PARTUUID.
set -e
BOARD_DIR="$(dirname "$0")"
PARTUUID="$("$HOST_DIR/bin/uuidgen")"

install -d "$TARGET_DIR/boot/extlinux"
sed "s/%PARTUUID%/$PARTUUID/g" "$BOARD_DIR/extlinux.conf" > "$TARGET_DIR/boot/extlinux/extlinux.conf"
sed "s/%PARTUUID%/$PARTUUID/g" "$BOARD_DIR/genimage.cfg" > "$BINARIES_DIR/genimage.cfg"

# SSH key login only
chmod 700 "$TARGET_DIR/root" "$TARGET_DIR/root/.ssh"
chmod 600 "$TARGET_DIR/root/.ssh/authorized_keys"
