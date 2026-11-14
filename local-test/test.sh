#!/bin/sh
# Load the FL2000 driver and show a modetest pattern on the adapter's monitor for 20 s.
# Run as root from the driver folder: sudo sh local-test/test.sh [MODE], e.g. 1280x720
# The driver stays loaded afterwards (unload is a separate step).
set -u
mode=${1:-1920x1080}
cd "$(dirname "$0")/.." || exit 1

lsusb -d 1d5c:2000 >/dev/null || { echo "FL2000 adapter not connected"; exit 1; }
lsmod | grep -q '^fl2000 ' || insmod fl2000.ko || exit 1
lsmod | grep -q '^it66121 ' || insmod it66121.ko || exit 1
sleep 5

card=
for c in /sys/class/drm/card*; do
	case ${c##*/} in *-*) continue ;; esac
	readlink -f "$c" | grep -q '/usb[0-9]*/.*/drm/' && card=$c
done
echo "Display device: ${card:-not found}"
[ -n "${card:-}" ] && for s in "$card"-*/status; do echo "  $s: $(cat "$s")"; done
[ -n "$card" ] && udevadm info -q property -p "$card" | grep -E '^(ID_SEAT|CURRENT_TAGS)=' | sed 's/^/  udev /'

conn=$(modetest -M fl2000_drm -c 2>/dev/null | awk '$3=="connected"{print $1; exit}')
if [ -z "$conn" ]; then
	echo "No connected monitor on the adapter"
	modetest -M fl2000_drm -c | head -20
else
	echo "Showing test pattern ($mode) on connector $conn for 20 s..."
	sleep 20 | modetest -M fl2000_drm -s "$conn:$mode" 2>&1 | tail -5
fi

echo "--- kernel log"
dmesg | grep -iE "fl2000|it66121|i2c-[0-9]+:|WARNING|urb" | tail -25

echo "--- driver left loaded; unload separately with: sudo rmmod it66121 && sudo rmmod fl2000"
