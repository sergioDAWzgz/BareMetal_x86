#!/usr/bin/env bash
# Boots the built image in QEMU and asserts the expected serial output.
# This is the phase gate: it must pass before the image is written to a stick.
set -euo pipefail

cd "$(dirname "$0")"

QEMU=${QEMU:-qemu-system-i386}
LOG=build/serial.log
BOOT_SECONDS=${BOOT_SECONDS:-8}

if ! command -v "$QEMU" >/dev/null 2>&1; then
  echo "skip: $QEMU is not installed" >&2
  exit 0
fi

if [ ! -f build/baremetal.img ]; then
  ./build.sh
fi

rm -f "$LOG"
timeout "$BOOT_SECONDS" "$QEMU" \
  -drive file=build/baremetal.img,format=raw,if=ide,index=0 \
  -m 256 -boot c -display none -serial "file:$LOG" -no-reboot \
  >/dev/null 2>&1 || true

echo '--- serial output ---'
cat "$LOG"
echo '---------------------'

fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q 'stage2: real mode up'                "$LOG" || fail 'stage 2 never reached real mode'
grep -q 'stage2: kernel loaded at 0x10000'    "$LOG" || fail 'kernel was not loaded'
grep -q 'BareMetal_x86 - phase 1'             "$LOG" || fail 'kernel banner missing'
grep -q 'cpu: *[A-Za-z]'                      "$LOG" || fail 'cpu brand string missing'
grep -q 'boot disk: 0x80'                     "$LOG" || fail 'boot drive not 0x80'

echo 'PASS: phase 1 boots, loads the kernel and prints the CPU brand'
