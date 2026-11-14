#!/bin/sh
# Install the FL2000 driver with DKMS: rebuilt and signed (Ubuntu MOK) for every 6.8 kernel,
# loaded automatically when the adapter is plugged in.
# Run as root: sudo sh local-test/dkms-install.sh        (sudo sh local-test/dkms-install.sh remove to uninstall)
set -eu
name=fl2000-drm
ver=$(sed -n 's/^PACKAGE_VERSION="\(.*\)"/\1/p' "$(dirname "$0")/../dkms.conf")
src=/usr/src/$name-$ver
conf=/etc/modprobe.d/fl2000.conf

if dkms status -m "$name" -v "$ver" | grep -q .; then
	dkms remove -m "$name" -v "$ver" --all
fi
rm -rf "$src"
# Older installs loaded it66121 via softdep; it is now loaded by its I2C device alias
rm -f "$conf"
if [ "${1:-}" = remove ]; then
	echo "Removed $name $ver"
	exit 0
fi

cd "$(dirname "$0")/.."
mkdir -p "$src/bridge"
cp dkms.conf Makefile LICENSE ./*.c ./*.h "$src/"
cp bridge/*.c bridge/*.h "$src/bridge/"
mkdir -p "$src/simd-include"
cp simd-include/*.h "$src/simd-include/"
rm -f "$src"/*.mod.c "$src"/bridge/*.mod.c

dkms install -m "$name" -v "$ver"
dkms status -m "$name"
modinfo -n fl2000 it66121
