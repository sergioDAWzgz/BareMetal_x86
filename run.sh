#!/usr/bin/env bash
# Boots the built disk image in QEMU with the serial console on stdout.
# Pass --uefi to boot the same image through OVMF instead of SeaBIOS.
set -euo pipefail

cd "$(dirname "$0")"

OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

# The single image carries the boot record, the kernel and the FAT32 volume, so
# every path boots the same disk. The `pc` machine has no AHCI controller, and the
# kernel reads the volume through AHCI: one is added here, exactly as the gate
# does it.
DISK=(
  -device ich9-ahci,id=sata
  -drive file=build/disk.img,format=raw,if=none,id=disk0
  -device ide-hd,drive=disk0,bus=sata.0,bootindex=1
)

if [ ! -f build/disk.img ]; then
  echo "error: build/disk.img is missing, run ./build.sh first" >&2
  exit 1
fi

if [ "${1:-}" = "--uefi" ]; then
  shift

  if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
    echo "error: OVMF firmware not found, install the ovmf package" >&2
    exit 1
  fi

  # OVMF writes its variables back, so it needs a private copy.
  cp "$OVMF_VARS" build/OVMF_VARS.fd

  exec qemu-system-x86_64 \
    -machine pc -cpu qemu64 -m 512 \
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
    -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
    "${DISK[@]}" \
    -nic user,model=e1000 \
    -display none \
    -serial stdio \
    -no-reboot \
    "$@"
fi

exec qemu-system-x86_64 \
  -machine pc -cpu qemu64 \
  "${DISK[@]}" \
  -m 512 \
  -boot c \
  -nic user,model=e1000 \
  -display none \
  -serial stdio \
  -no-reboot \
  "$@"
