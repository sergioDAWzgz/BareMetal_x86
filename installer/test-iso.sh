#!/usr/bin/env bash
# Installs into a blank QEMU disk with the default one-button path and then boots
# the installed disk through OVMF. Not part of ./test.sh; run it after building
# the ISO with installer/build-iso.sh. BMX_TEST_MEDIA=usb boots the ISO as a USB
# stick instead of a CD-ROM.
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=build/installer/bmx-installer.iso
TARGET=${BMX_TEST_DISK:-$HOME/.cache/bmx/installer-test/target.img}
INSTALL_LOG=build/installer/install.log
BOOT_LOG=build/installer/installed.log
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
MEDIA=${BMX_TEST_MEDIA:-cd}

[ -f "$ISO" ] || { echo "FAIL: $ISO is missing, run installer/build-iso.sh" >&2; exit 1; }
mkdir -p "$(dirname "$TARGET")"
rm -f "$TARGET"
truncate -s 2G "$TARGET"
rm -f "$INSTALL_LOG" "$BOOT_LOG"
cp "$OVMF_VARS" build/installer/OVMF_VARS.fd

if [ "$MEDIA" = usb ]; then
  INSTALL_MEDIA=(-device qemu-xhci -device usb-storage,drive=usbstick
                 -drive if=none,id=usbstick,file="$ISO",format=raw)
else
  INSTALL_MEDIA=(-cdrom "$ISO")
fi

qemu-system-x86_64 -machine q35 -cpu qemu64 -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/installer/OVMF_VARS.fd \
  -drive file="$TARGET",format=raw,if=none,id=target \
  -device ide-hd,drive=target \
  "${INSTALL_MEDIA[@]}" -display none -serial "file:$INSTALL_LOG" -no-reboot \
  >/dev/null 2>&1 &
pid=$!
for _ in $(seq 1 1800); do
  if grep -q 'BMX-INSTALLER: complete' "$INSTALL_LOG" 2>/dev/null; then
    break
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.5
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
echo "=== install log ==="
grep -a 'BMX-INSTALLER' "$INSTALL_LOG" || true
if ! grep -q 'BMX-INSTALLER: complete' "$INSTALL_LOG"; then
  echo "FAIL: the default install did not complete" >&2
  tail -20 "$INSTALL_LOG" >&2
  exit 1
fi

python3 - "$TARGET" <<'EOF'
import struct, sys
data = open(sys.argv[1], 'rb').read(512)
total = 2 * 1024 * 1024 * 1024 // 512
parts = []
for i in range(4):
    e = data[446 + i * 16:446 + (i + 1) * 16]
    if e[4] == 0:
        continue
    start, size = struct.unpack('<II', e[8:16])
    parts.append((i + 1, e[4], start, size))
assert len(parts) == 2, "expected exactly two partitions, got %r" % (parts,)
esp, root = parts
assert esp[1] == 0x0c, "ESP type is not 0x0c"
assert esp[2] == 2048 and esp[3] == 131072, "ESP is not 64 MiB at LBA 2048"
assert root[1] == 0x83, "root type is not 0x83"
assert root[2] == 133120, "root does not start after the ESP"
assert root[2] + root[3] == total, "root does not fill the disk"
print("layout ok: ESP start=2048 size=131072, root start=%d size=%d" %
      (root[2], root[3]))
EOF

cp "$OVMF_VARS" build/installer/OVMF_VARS.fd
qemu-system-x86_64 -machine q35 -cpu qemu64 -m 1024 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/installer/OVMF_VARS.fd \
  -drive file="$TARGET",format=raw,if=none,id=target \
  -device ide-hd,drive=target \
  -nic user,model=e1000 \
  -display none -serial "file:$BOOT_LOG" -no-reboot \
  >/dev/null 2>&1 &
pid=$!
for _ in $(seq 1 300); do
  if grep -q 'OpenRC [0-9]' "$BOOT_LOG" 2>/dev/null; then
    break
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.5
done
kill "$pid" 2>/dev/null || true
wait "$pid" 2>/dev/null || true
echo "=== installed boot ==="
grep -aE 'Linux version|VFS: Mounted root|Run /sbin/init|OpenRC [0-9]' "$BOOT_LOG" | head -6
grep -q 'OpenRC [0-9]' "$BOOT_LOG" || { echo "FAIL: installed disk did not reach OpenRC" >&2; exit 1; }
echo "PASS: the default install boots"
