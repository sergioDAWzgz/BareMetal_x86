#!/usr/bin/env bash
# Boots the built image in QEMU and asserts the expected serial output.
# This is the phase gate: it must pass before the image is written to a stick.
set -euo pipefail

cd "$(dirname "$0")"

QEMU=${QEMU:-qemu-system-x86_64}
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
  -machine pc -cpu qemu64 \
  -drive file=build/baremetal.img,format=raw,if=ide,index=0 \
  -m 256 -boot c -display none -serial "file:$LOG" -no-reboot \
  >/dev/null 2>&1 || true

echo '--- serial output ---'
cat "$LOG"
echo '---------------------'

fail() { echo "FAIL: $1" >&2; exit 1; }

grep -q 'stage2: real mode up'                 "$LOG" || fail 'stage 2 never reached real mode'
grep -q 'stage2: BIOS memory map collected'    "$LOG" || fail 'E820 collection did not run'
grep -q 'stage2: kernel loaded at 0x10000'     "$LOG" || fail 'kernel was not loaded'
grep -q 'BareMetal_x86 - phase 2'              "$LOG" || fail 'kernel banner missing'
grep -q 'mode:      64-bit long mode'          "$LOG" || fail 'long mode not reached'
grep -q 'cpu: *[A-Za-z]'                       "$LOG" || fail 'cpu brand string missing'
grep -q 'mem: usable 25[0-6] MiB'               "$LOG" || fail 'usable memory is not ~256 MiB'
grep -q 'pmm: allocated 0x0000000000100000'    "$LOG" || fail 'first frame is not 0x100000'
grep -q 'pmm: free frames 6[0-9][0-9][0-9][0-9]' "$LOG" || fail 'frame allocator test missing'
grep -q 'pit: *[0-9]* ticks observed'          "$LOG" || fail 'PIT ticks not observed'
grep -q '#BP breakpoint'                       "$LOG" || fail 'breakpoint exception not reported'
grep -q 'exc: resumed after breakpoint'        "$LOG" || fail 'breakpoint did not resume'
grep -q '#DE divide error'                     "$LOG" || fail 'divide error not reported'
grep -q 'exc: resumed after divide error'      "$LOG" || fail 'divide error did not resume'
grep -q 'Phase 2 complete'                     "$LOG" || fail 'phase 2 did not complete'

echo 'PASS: phase 2 reaches 64-bit long mode, maps memory and survives faults'
