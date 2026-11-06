#!/usr/bin/env bash
# QEMU smoke test: boot a NuOS raw image with OVMF and wait for the
# NUOS_BOOT_OK marker on the serial console. No camera, Wi-Fi or other
# hardware is present — hardware daemons must fail/restart harmlessly and
# not block the marker.
#
# Usage: qemu-smoke.sh <image.img>
# Requires: qemu-system-x86, /usr/share/OVMF/OVMF_CODE.fd (override with OVMF_CODE=...)
set -euo pipefail

if [ $# -ne 1 ]; then
    echo "usage: $0 <image.img>" >&2
    exit 2
fi
IMG=$1
[ -f "$IMG" ] || { echo "ERROR: image not found: $IMG" >&2; exit 2; }

command -v qemu-system-x86_64 >/dev/null 2>&1 || {
    echo "ERROR: qemu-system-x86_64 not installed (apt install qemu-system-x86)" >&2
    exit 2
}
OVMF="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}"
[ -f "$OVMF" ] || OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd
[ -f "$OVMF" ] || { echo "ERROR: OVMF firmware not found (apt install ovmf)"; exit 1; }
[ -f "$OVMF" ] || { echo "ERROR: OVMF firmware not found at $OVMF (apt install ovmf)" >&2; exit 2; }

LOG="$(mktemp /tmp/nuos-smoke-serial.XXXXXX.log)"
PIDFILE="$(mktemp /tmp/nuos-smoke-qemu.XXXXXX.pid)"
trap 'kill "$(cat "$PIDFILE")" 2>/dev/null || true; rm -f "$LOG" "$PIDFILE"' EXIT

echo "==> booting $IMG under QEMU/OVMF (serial -> $LOG)"
qemu-system-x86_64 \
    -M q35 \
    -cpu max \
    -m 2G \
    -smp 2 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive file="$IMG",format=raw,if=virtio \
    -serial file:"$LOG" \
    -display none \
    -nic none &
QEMU_PID=$!
echo "$QEMU_PID" > "$PIDFILE"

TIMEOUT="${SMOKE_TIMEOUT:-180}"
echo "==> waiting up to ${TIMEOUT}s for NUOS_BOOT_OK"
ELAPSED=0
RESULT=""
while [ "$ELAPSED" -lt "$TIMEOUT" ]; do
    sleep 5
    ELAPSED=$((ELAPSED + 5))

    # fail fast on kernel panic
    if grep -qE 'Kernel panic|Kernel Offset: disabled' "$LOG"; then
        echo "FAIL: kernel panic detected after ${ELAPSED}s:" >&2
        tail -30 "$LOG" >&2 || true
        exit 1
    fi

    if grep -q 'NUOS_BOOT_OK' "$LOG"; then
        RESULT=ok
        break
    fi
done

if [ "$RESULT" = "ok" ]; then
    echo "PASS: NUOS_BOOT_OK after ~${ELAPSED}s"
    exit 0
fi

echo "FAIL: no NUOS_BOOT_OK within ${TIMEOUT}s" >&2
echo "---- last serial output ----" >&2
tail -50 "$LOG" >&2 || true
exit 1
