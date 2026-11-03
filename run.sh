#!/usr/bin/env bash
# Boots the built artefacts in QEMU with the serial console on stdout.
# Pass --uefi to boot the EFI path through OVMF instead of the BIOS image.
set -euo pipefail

cd "$(dirname "$0")"

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

if [ "${1:-}" = "--uefi" ]; then
  shift

  if [ ! -f build/esp.img ]; then
    echo "error: build/esp.img is missing, run ./build.sh first" >&2
    exit 1
  fi
  if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "error: OVMF firmware not found, install the ovmf package" >&2
    exit 1
  fi

  # OVMF writes its variables back, so it needs a private copy.
  cp "$OVMF_VARS" build/OVMF_VARS.fd

  exec qemu-system-x86_64 \
    -machine pc -cpu qemu64 -m 256 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
    -drive file=build/esp.img,format=raw,if=ide,index=0 \
    -display none \
    -serial stdio \
    -no-reboot \
    "$@"
fi

if [ ! -f build/baremetal.img ]; then
  echo "error: build/baremetal.img is missing, run ./build.sh first" >&2
  exit 1
fi

exec qemu-system-x86_64 \
  -machine pc -cpu qemu64 \
  -drive file=build/baremetal.img,format=raw,if=ide,index=0 \
  -m 256 \
  -boot c \
  -display none \
  -serial stdio \
  -no-reboot \
  "$@"
