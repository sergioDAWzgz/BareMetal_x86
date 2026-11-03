#!/usr/bin/env bash
# Boots build/baremetal.img in QEMU with the serial console on stdout.
set -euo pipefail

cd "$(dirname "$0")"

if [ ! -f build/baremetal.img ]; then
  echo "error: build/baremetal.img is missing, run ./build.sh first" >&2
  exit 1
fi

exec qemu-system-i386 \
  -drive file=build/baremetal.img,format=raw,if=ide,index=0 \
  -m 256 \
  -boot c \
  -display none \
  -serial stdio \
  -no-reboot \
  "$@"
