#!/bin/sh
set -e

SRC_DIR="$1"
ROOTFS_DIR="$2"
BUILD_DIR="$3"

mkdir -p "$BUILD_DIR"
mkdir -p "$ROOTFS_DIR/lib/modules"

if [ -f "$SRC_DIR/kpnvme.ko" ]; then
    cp "$SRC_DIR/kpnvme.ko" "$ROOTFS_DIR/lib/modules/"
    echo "kpnvme.ko copied to rootfs"
fi

cd "$ROOTFS_DIR"
find . | cpio -H newc -o 2>/dev/null | gzip > "$BUILD_DIR/rootfs.cpio.gz"
echo "rootfs packed"
