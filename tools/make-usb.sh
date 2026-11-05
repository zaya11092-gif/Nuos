#!/usr/bin/env bash
# Flash a NuOS raw image to a USB drive.
#
# Usage: make-usb.sh <image.img> /dev/sdX
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "usage: $0 <image.img> </dev/sdX>" >&2
    exit 2
fi
IMG=$1
DEV=$2

[ -f "$IMG" ] || { echo "ERROR: image not found: $IMG" >&2; exit 2; }
[ -w "$DEV" ] || {
    echo "ERROR: $DEV is not writable (run as root? right device?)" >&2
    exit 2
}
case "$DEV" in
    /dev/sd?|/dev/nvme?n?|/dev/mmcblk?) ;;
    *) echo "ERROR: refusing non-block device path: $DEV" >&2; exit 2 ;;
esac
[ -b "$DEV" ] || { echo "ERROR: $DEV is not a block device" >&2; exit 2; }

IMG_BYTES=$(stat -c %s "$IMG")
DEV_BYTES=$(blockdev --getsize64 "$DEV")
if [ "$IMG_BYTES" -gt "$DEV_BYTES" ]; then
    echo "ERROR: image ($IMG_BYTES bytes) larger than device ($DEV_BYTES bytes)" >&2
    exit 1
fi

echo "About to ERASE $DEV:"
echo "  device: $(lsblk -dn -o MODEL "$DEV" 2>/dev/null || echo '?'), $DEV_BYTES bytes"
echo "  image:  $IMG, $IMG_BYTES bytes"
echo "ALL DATA ON $DEV WILL BE DESTROYED."
printf 'Type the device name to confirm: '
read -r ANSWER
[ "$ANSWER" = "$DEV" ] || { echo "aborted" >&2; exit 1; }

echo "==> writing image"
dd if="$IMG" of="$DEV" bs=4M status=progress conv=fsync
sync
echo "==> done. Eject and boot via the MrChromebox UEFI boot menu (F7/ESC)."
