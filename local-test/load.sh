#!/bin/sh
# Load the FL2000 driver without taking over the display, so the desktop (Xorg) can pick it up.
# Run as root from anywhere: sudo sh local-test/load.sh
set -u
cd "$(dirname "$0")/.." || exit 1

lsusb -d 1d5c:2000 >/dev/null || { echo "FL2000 adapter not connected"; exit 1; }
[ -e /etc/udev/rules.d/72-fl2000-desktop-ignore.rules ] &&
	echo "Note: desktop-ignore udev rule is installed, the desktop will not use the adapter"
lsmod | grep -q '^fl2000 ' || insmod fl2000.ko || exit 1
lsmod | grep -q '^it66121 ' || insmod it66121.ko || exit 1
sleep 5

card=
for c in /sys/class/drm/card*; do
	case ${c##*/} in *-*) continue ;; esac
	readlink -f "$c" | grep -q '/usb[0-9]*/.*/drm/' && card=$c
done
echo "Display device: ${card:-not found}"
[ -n "$card" ] || exit 1
for s in "$card"-*/status; do echo "  $s: $(cat "$s")"; done
udevadm info -q property -p "$card" | grep -E '^ID_SEAT=' | sed 's/^/  udev /' ||
	echo "  udev ID_SEAT not set (seat0, desktop may use it)"
