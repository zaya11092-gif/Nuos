#!/usr/bin/env bash
# QEMU smoke test for the NuOS installer ISO: boots the ISO via UEFI and
# waits for the installer's NUOS_INSTALLER_READY marker on serial.
# Usage: qemu-smoke-iso.sh <nuos.iso>
set -uo pipefail
IMG="${1:?usage: qemu-smoke-iso.sh <nuos.iso>}"
[ -f "$IMG" ] || { echo "ERROR: iso not found: $IMG"; exit 1; }

OVMF="${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE.fd}"
[ -f "$OVMF" ] || OVMF=/usr/share/OVMF/OVMF_CODE_4M.fd
[ -f "$OVMF" ] || { echo "ERROR: OVMF firmware not found (apt install ovmf)"; exit 1; }
VARS="$(mktemp /tmp/nuos-vars.XXXXXX.fd)"
cp "/usr/share/OVMF/OVMF_VARS_4M.fd" "$VARS" 2>/dev/null || cp "${OVMF%_CODE*}OVMF_VARS.fd" "$VARS"

LOG="$(mktemp /tmp/nuos-iso-smoke.XXXXXX.log)"
TIMEOUT="${SMOKE_TIMEOUT:-240}"
echo "==> booting $IMG under QEMU/OVMF (serial -> $LOG)"
timeout "$TIMEOUT" qemu-system-x86_64 \
    -m 4G -smp 2 -machine q35 -cpu max \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF" \
    -drive if=pflash,format=raw,file="$VARS" \
    -drive file="$IMG",if=virtio,format=raw,readonly=on \
    -display none -serial file:"$LOG" -nic none
RC=$?

if grep -aq "NUOS_INSTALLER_READY" "$LOG" || grep -aq "Reached target Graphical Interface" "$LOG"; then
    echo "PASS: installer reached ($TIMEOUT s window)"
    RC=0
else
    echo "FAIL: no NUOS_INSTALLER_READY within ${TIMEOUT}s"
    RC=1
fi
echo "---- last serial output ----"
tail -c 1200 "$LOG" | tr -d '\r'
rm -f "$LOG" "$VARS"
exit $RC
