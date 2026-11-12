#!/usr/bin/env bash
# Builds build/installer/bmx-installer.iso: a UEFI installer ISO carrying the
# same Linux kernel plus an installer initramfs and the OS payload.
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR=build
LINUX_CACHE=${BMX_LINUX_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/bmx}
LINUX_VERSION=6.12.115
ALPINE_VERSION=3.24.4
ALPINE_BRANCH=v3.24
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH"
ALPINE_CACHE="$LINUX_CACHE/alpine-$ALPINE_VERSION"
ALPINE_MINIROOTFS="$ALPINE_CACHE/alpine-minirootfs-$ALPINE_VERSION-x86_64.tar.gz"
ALPINE_APK="$ALPINE_CACHE/apk.static"
ALPINE_ROOT="$ALPINE_CACHE/rootfs"
INSTALLER_CACHE="$LINUX_CACHE/installer-$ALPINE_VERSION"
INSTALLER_ROOT="$INSTALLER_CACHE/rootfs"
INSTALLER_OUT="$BUILD_DIR/installer"
INSTALLER_PACKAGES="e2fsprogs dosfstools util-linux cfdisk dialog kbd-bkeymaps ncurses-terminfo-base"
SRC="$LINUX_CACHE/linux-$LINUX_VERSION"
OBJ="$LINUX_CACHE/build-installer-$LINUX_VERSION"
XORRISO=${XORRISO:-xorriso}

step() { echo; echo "==> $*"; }

for f in build/kernel.bin build/bzImage build/stage1.bin build/stage2.bin \
         build/uefi/BOOTX64.EFI build/INITRD build/USER.ELF data/HELLO.TXT; do
  [ -f "$f" ] || {
    echo "error: $f is missing, run ./build.sh first" >&2
    exit 1
  }
done
for f in "$ALPINE_MINIROOTFS" "$ALPINE_APK" "$SRC/scripts/kconfig/merge_config.sh"; do
  [ -f "$f" ] || {
    echo "error: $f is missing, run ./build.sh first" >&2
    exit 1
  }
done
if ! command -v "$XORRISO" >/dev/null 2>&1; then
  echo "error: xorriso is required to build the installer ISO" >&2
  echo "       apt-get install xorriso" >&2
  exit 1
fi
mkdir -p "$INSTALLER_CACHE" "$INSTALLER_OUT" "$INSTALLER_ROOT"

step "build the installer rootfs"
ROOTFS_STAMP=$(printf '%s\n' "$ALPINE_VERSION" "$INSTALLER_PACKAGES" |
  sha256sum | cut -d' ' -f1)
if [ ! -f "$INSTALLER_CACHE/.rootfs-$ROOTFS_STAMP" ]; then
  rm -rf "$INSTALLER_ROOT"
  mkdir -p "$INSTALLER_ROOT"
  tar -xzf "$ALPINE_MINIROOTFS" -C "$INSTALLER_ROOT" --exclude='./dev/*'
  "$ALPINE_APK" add --root "$INSTALLER_ROOT" --usermode --no-chown --no-progress \
    --repository "$ALPINE_MIRROR/main" --repository "$ALPINE_MIRROR/community" \
    $INSTALLER_PACKAGES
  rm -f "$INSTALLER_CACHE/.rootfs-"*
  touch "$INSTALLER_CACHE/.rootfs-$ROOTFS_STAMP"
fi

mkdir -p "$INSTALLER_ROOT/installer"
cp installer/init "$INSTALLER_ROOT/init"
cp installer/tui.sh installer/install.sh "$INSTALLER_ROOT/installer/"
chmod +x "$INSTALLER_ROOT/init" "$INSTALLER_ROOT/installer/tui.sh"

step "build the installer initramfs"
GEN="$INSTALLER_CACHE/gen_init_cpio"
if [ ! -x "$GEN" ]; then
  gcc -O2 -o "$GEN" "$SRC/usr/gen_init_cpio.c"
fi
LIST="$INSTALLER_CACHE/initramfs.list"
: > "$LIST"
(
  cd "$INSTALLER_ROOT" || exit 1
  find . -mindepth 1 -print | while IFS= read -r p; do
    rel=${p#./}
    if [ -L "$p" ]; then
      printf 'slink /%s %s %s 0 0\n' "$rel" "$(readlink "$p")" "$(stat -c %a "$p")" >> "$LIST"
    elif [ -d "$p" ]; then
      printf 'dir /%s %s 0 0\n' "$rel" "$(stat -c %a "$p")" >> "$LIST"
    else
      printf 'file /%s %s/%s %s 0 0\n' "$rel" "$INSTALLER_ROOT" "$rel" "$(stat -c %a "$p")" >> "$LIST"
    fi
  done
) >> "$LIST"
printf 'nod /dev/console 0600 0 0 c 5 1\n' >> "$LIST"
printf 'nod /dev/null 0666 0 0 c 1 3\n' >> "$LIST"
printf 'nod /dev/tty 0666 0 0 c 5 0\n' >> "$LIST"
"$GEN" "$LIST" | gzip -9 -n > "$INSTALLER_CACHE/initramfs.cpio.gz"
ls -l "$INSTALLER_CACHE/initramfs.cpio.gz"

step "build the installer kernel"
cp configs/linux-baytrail.fragment "$INSTALLER_CACHE/baytrail.fragment"
cat > "$INSTALLER_CACHE/installer.fragment" <<EOF
CONFIG_CMDLINE_BOOL=y
CONFIG_CMDLINE="console=tty0 console=ttyS0 rdinit=/init"
CONFIG_INITRAMFS_SOURCE="$INSTALLER_CACHE/initramfs.cpio.gz"
EOF
mkdir -p "$OBJ"
make -C "$SRC" O="$OBJ" defconfig >/dev/null
"$SRC/scripts/kconfig/merge_config.sh" -m -Q -O "$OBJ" "$OBJ/.config" \
  "$INSTALLER_CACHE/baytrail.fragment" "$INSTALLER_CACHE/installer.fragment" >/dev/null
make -C "$SRC" O="$OBJ" olddefconfig >/dev/null
make -C "$SRC" O="$OBJ" -j"$(nproc)" bzImage
cp "$OBJ/arch/x86/boot/bzImage" "$INSTALLER_CACHE/installer-bzImage"

step "assemble the payload"
PAYLOAD="$INSTALLER_OUT/payload"
rm -rf "$PAYLOAD"
mkdir -p "$PAYLOAD/esp/EFI/BOOT"
cp data/HELLO.TXT "$PAYLOAD/esp/HELLO.TXT"
cp build/USER.ELF "$PAYLOAD/esp/USER.ELF"
cp build/bzImage "$PAYLOAD/esp/LINUX.BIN"
cp build/INITRD "$PAYLOAD/esp/INITRD.BIN"
cp build/uefi/BOOTX64.EFI "$PAYLOAD/esp/EFI/BOOT/BOOTX64.EFI"
cp build/kernel.bin "$PAYLOAD/kernel.bin"
cp build/stage1.bin "$PAYLOAD/stage1.bin"
cp build/stage2.bin "$PAYLOAD/stage2.bin"
tar -czf "$PAYLOAD/rootfs.tar.gz" -C "$ALPINE_ROOT" .
offset=$(grep -abo '424d5836-02' build/kernel.bin | cut -d: -f1)
[ "$(printf '%s\n' "$offset" | grep -c .)" = 1 ] || {
  echo "error: expected one PARTUUID template in build/kernel.bin" >&2
  exit 1
}
printf '%s\n' "$offset" > "$PAYLOAD/kernel.uuid-offset"
ls -lh "$PAYLOAD/rootfs.tar.gz"

step "build the ISO"
ISO_DIR="$INSTALLER_OUT/iso"
rm -rf "$ISO_DIR"
mkdir -p "$ISO_DIR"
cp -a "$PAYLOAD" "$ISO_DIR/payload"
EFI_IMG="$INSTALLER_OUT/efi.img"
rm -f "$EFI_IMG"
dd if=/dev/zero of="$EFI_IMG" bs=1M count=48 status=none
mformat -i "$EFI_IMG" -F ::
mmd -i "$EFI_IMG" ::/EFI ::/EFI/BOOT
mcopy -i "$EFI_IMG" "$INSTALLER_CACHE/installer-bzImage" ::/EFI/BOOT/BOOTX64.EFI
cp "$EFI_IMG" "$ISO_DIR/efi.img"
"$XORRISO" -as mkisofs -iso-level 3 -J -R -V BMXINSTALL \
  -e efi.img -no-emul-boot -eltorito-platform efi \
  -isohybrid-gpt-basdat \
  -o "$INSTALLER_OUT/bmx-installer.iso" "$ISO_DIR"
ls -lh "$INSTALLER_OUT/bmx-installer.iso"
