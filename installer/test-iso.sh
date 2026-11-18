#!/usr/bin/env bash
# Installs into a blank QEMU disk with the default one-button path and then boots
# the installed disk through the same firmware. Not part of ./test.sh; run it
# after building the ISO with installer/build-iso.sh. BMX_TEST_MEDIA=usb boots
# the ISO as a USB stick instead of a CD-ROM; BMX_TEST_MEDIA=fat reproduces a
# Rufus ISO-mode stick (a FAT32 volume with the extracted payload and the
# BMXINSTALL label). BMX_TEST_BOOT=bios uses SeaBIOS instead of OVMF.
# BMX_TEST_KEEP=1 keeps the target disk, so a second run installs over the
# previous installation instead of starting from a blank disk.
set -euo pipefail
cd "$(dirname "$0")/.."

ISO=build/installer/bmx-installer.iso
TARGET=${BMX_TEST_DISK:-$HOME/.cache/bmx/installer-test/target.img}
INSTALL_LOG=build/installer/install.log
BOOT_LOG=build/installer/installed.log
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}
MEDIA=${BMX_TEST_MEDIA:-cd}
BOOT=${BMX_TEST_BOOT:-uefi}
LAYOUT=${BMX_TEST_LAYOUT:-auto}
KEEP=${BMX_TEST_KEEP:-0}
# The QEMU human monitor has no authentication. Give each run a private
# directory (mktemp creates it 0700), and only open the monitor for the manual
# layout, which is the only one that drives the TUI.
RUN_DIR=$(mktemp -d "${TMPDIR:-/tmp}/bmx-test.XXXXXX") || exit 1
trap 'rm -rf "$RUN_DIR"' EXIT
MON="$RUN_DIR/monitor.sock"
if [ "${#MON}" -gt 100 ]; then
  echo "FAIL: the monitor socket path is too long for a Unix socket: $MON" >&2
  exit 1
fi

case "$BOOT" in
uefi | bios) ;;
*)
  echo "FAIL: BMX_TEST_BOOT must be uefi or bios" >&2
  exit 1
  ;;
esac
case "$LAYOUT" in
auto | manual) ;;
*)
  echo "FAIL: BMX_TEST_LAYOUT must be auto or manual" >&2
  exit 1
  ;;
esac
if [ "$BOOT" = uefi ]; then
  [ -f "$OVMF_CODE" ] || { echo "FAIL: $OVMF_CODE is missing" >&2; exit 1; }
  [ -f "$OVMF_VARS" ] || { echo "FAIL: $OVMF_VARS is missing" >&2; exit 1; }
  cp "$OVMF_VARS" build/installer/OVMF_VARS.fd
  FIRMWARE=(
    -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE"
    -drive if=pflash,format=raw,file=build/installer/OVMF_VARS.fd
  )
else
  FIRMWARE=()
fi

[ -f "$ISO" ] || { echo "FAIL: $ISO is missing, run installer/build-iso.sh" >&2; exit 1; }
mkdir -p "$(dirname "$TARGET")"
if [ "$KEEP" = 1 ] && [ -f "$TARGET" ]; then
  echo "keeping the existing target at $TARGET"
else
  rm -f "$TARGET"
  truncate -s 2G "$TARGET"
fi
rm -f "$INSTALL_LOG" "$BOOT_LOG"
# The manual path is driven through cfdisk; seed the table it would produce so
# the test can leave cfdisk without scripting its menu, and let the installer's
# own validation, PARTUUID patch and format run.
if [ "$LAYOUT" = manual ]; then
  sectors=$(( $(stat -c %s "$TARGET") / 512 ))
  root_size=$((sectors - 133120))
  printf 'label: dos\nstart=2048, size=131072, type=c\nstart=133120, size=%d, type=83\n' \
    "$root_size" | sfdisk --force "$TARGET" >/dev/null
fi
echo "boot: $BOOT (media: $MEDIA, layout: $LAYOUT)"

case "$MEDIA" in
cd | usb | fat) ;;
*)
  echo "FAIL: BMX_TEST_MEDIA must be cd, usb or fat" >&2
  exit 1
  ;;
esac
if [ "$BOOT" = bios ] && [ "$MEDIA" = fat ]; then
  echo "FAIL: the fat media mode is the UEFI extraction; use BMX_TEST_MEDIA=usb for a BIOS stick" >&2
  exit 1
fi

if [ "$MEDIA" = fat ]; then
  USB_IMG=$HOME/.cache/bmx/installer-test/usb.img
  rm -f "$USB_IMG"
  truncate -s 512M "$USB_IMG"
  mformat -i "$USB_IMG" -F -v BMXINSTALL ::
  mmd -i "$USB_IMG" ::/EFI ::/EFI/BOOT ::/payload
  mcopy -i build/installer/efi.img ::/EFI/BOOT/BOOTX64.EFI "$RUN_DIR/bmx-bootx64.efi"
  mcopy -i "$USB_IMG" "$RUN_DIR/bmx-bootx64.efi" ::/EFI/BOOT/BOOTX64.EFI
  mcopy -s -i "$USB_IMG" build/installer/payload/* ::/payload
  INSTALL_MEDIA=(-device qemu-xhci -device usb-storage,drive=usbstick,bootindex=1
                 -drive if=none,id=usbstick,file="$USB_IMG",format=raw)
elif [ "$MEDIA" = usb ]; then
  INSTALL_MEDIA=(-device qemu-xhci -device usb-storage,drive=usbstick,bootindex=1
                 -drive if=none,id=usbstick,file="$ISO",format=raw)
else
  INSTALL_MEDIA=(-cdrom "$ISO")
fi
# A pre-partitioned target has a bootable MBR, and SeaBIOS tries the disk before
# the CD; give the CD priority so the installer boots whether the table is seeded
# or not. The USB media carries bootindex=1 for the same reason.
BOOTARGS=()
if [ "$BOOT" = bios ] && [ "$MEDIA" = cd ]; then
  BOOTARGS=(-boot order=d)
fi
MONARGS=()
if [ "$LAYOUT" = manual ]; then
  MONARGS=(-monitor "unix:$MON,server,nowait")
fi

# Sends one HMP command to the QEMU monitor. The installer reads the keyboard
# from tty0, so sendkey is how the harness drives the TUI.
monitor() {
  python3 - "$MON" "$1" <<'PY'
import socket, sys, time
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.connect(sys.argv[1])
time.sleep(0.1)
s.sendall((sys.argv[2] + "\n").encode())
time.sleep(0.2)
s.close()
PY
}

key() {
  monitor "sendkey $1"
  sleep "${2:-0.35}"
}

# Walks the menu to the manual path. cfdisk is left with q without changing the
# seeded table, so what gets exercised is the installer's validation, PARTUUID
# patch, format and boot, not a scripted cfdisk menu.
drive_manual() {
  for _ in $(seq 1 240); do
    grep -q 'BMX-INSTALLER: ISO .* mounted' "$INSTALL_LOG" 2>/dev/null && break
    sleep 0.5
  done
  sleep 2
  key a 1.5
  key down
  key down
  key down
  key ret 1.5
  key ret 1.5
  key ret 1.5
  key q 1.5
  key ret 1.5
  key ret 1.2
}

qemu-system-x86_64 -machine q35 -cpu qemu64 -m 1024 \
  "${FIRMWARE[@]}" \
  -drive file="$TARGET",format=raw,if=none,id=target \
  -device ide-hd,drive=target \
  "${INSTALL_MEDIA[@]}" "${BOOTARGS[@]}" "${MONARGS[@]}" \
  -display none -serial "file:$INSTALL_LOG" -no-reboot \
  >/dev/null 2>&1 &
pid=$!
if [ "$LAYOUT" = manual ]; then
  drive_manual &
  drive_pid=$!
fi
for _ in $(seq 1 1800); do
  if grep -q 'BMX-INSTALLER: complete' "$INSTALL_LOG" 2>/dev/null; then
    break
  fi
  kill -0 "$pid" 2>/dev/null || break
  sleep 0.5
done
kill "$pid" 2>/dev/null || true
if [ "$LAYOUT" = manual ]; then
  wait "$drive_pid" 2>/dev/null || true
fi
wait "$pid" 2>/dev/null || true
echo "=== install log ==="
grep -a 'BMX-INSTALLER' "$INSTALL_LOG" || true
if ! grep -q 'BMX-INSTALLER: complete' "$INSTALL_LOG"; then
  echo "FAIL: the default install did not complete" >&2
  tail -20 "$INSTALL_LOG" >&2
  exit 1
fi
if [ "$LAYOUT" = manual ] &&
   ! grep -q 'BMX-INSTALLER: disk /dev/sda layout manual' "$INSTALL_LOG"; then
  echo "FAIL: the install log does not show the manual layout" >&2
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

if [ "$BOOT" = uefi ]; then
  cp "$OVMF_VARS" build/installer/OVMF_VARS.fd
fi
qemu-system-x86_64 -machine q35 -cpu qemu64 -m 1024 \
  "${FIRMWARE[@]}" \
  -drive file="$TARGET",format=raw,if=none,id=target \
  -device ide-hd,drive=target \
  -nic user,model=e1000 \
  -display none -serial "file:$BOOT_LOG" -no-reboot \
  >/dev/null 2>&1 &
pid=$!
for _ in $(seq 1 300); do
  if grep -q '^Kernel: 6\.12\.' "$BOOT_LOG" 2>/dev/null; then
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
grep -q '^Kernel: 6\.12\.' "$BOOT_LOG" || { echo "FAIL: the installed image did not print the boot banner" >&2; exit 1; }
echo "PASS: the default install boots"
