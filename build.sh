#!/usr/bin/env bash
# Builds the BareMetal_x86 boot artefacts.
#
# Requires: nasm, gcc with 64-bit freestanding support, binutils, coreutils.
# Optional: gnu-efi and mtools for the UEFI loader and its ESP image.
# Runs on Linux or inside WSL; the build host does not need to be the target.
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR=build
KERNEL_LIMIT=$((64 * 1024))    # stage 2 reads two 32 KiB chunks
STAGE2_SECTORS=16

CC=${CC:-gcc}
LD=${LD:-ld}
NASM=${NASM:-nasm}
OBJCOPY=${OBJCOPY:-objcopy}
MFORMAT=${MFORMAT:-mformat}
MMD=${MMD:-mmd}
MCOPY=${MCOPY:-mcopy}

CFLAGS=(
  -m64 -std=c11 -O2 -Wall -Wextra
  -mcmodel=kernel -mno-red-zone
  -ffreestanding -fno-builtin -fno-pie -fno-stack-protector
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -mno-sse -mno-sse2 -mno-mmx -msoft-float
  -Ikernel
)

step() { printf '\n==> %s\n' "$1"; }

step "clean"
rm -rf "$BUILD_DIR"
mkdir -p "$BUILD_DIR/kernel"

step "compile kernel sources"
objects=()
for source in kernel/*.c; do
  object="$BUILD_DIR/kernel/$(basename "${source%.c}").o"
  "$CC" "${CFLAGS[@]}" -c "$source" -o "$object"
  objects+=("$object")
done

step "assemble kernel entry and interrupt stubs"
"$NASM" -f elf64 kernel/entry.asm -o "$BUILD_DIR/kernel/entry.o"
"$NASM" -f elf64 kernel/isr.asm -o "$BUILD_DIR/kernel/isr.o"
objects=("$BUILD_DIR/kernel/entry.o" "$BUILD_DIR/kernel/isr.o" "${objects[@]}")

step "link kernel"
"$LD" -m elf_x86_64 -T kernel/kernel.ld -o "$BUILD_DIR/kernel.elf" "${objects[@]}"
"$OBJCOPY" -O binary "$BUILD_DIR/kernel.elf" "$BUILD_DIR/kernel.bin"

kernel_size=$(stat -c %s "$BUILD_DIR/kernel.bin")
if [ "$kernel_size" -gt "$KERNEL_LIMIT" ]; then
  echo "error: kernel.bin is $kernel_size bytes, limit is $KERNEL_LIMIT" >&2
  exit 1
fi

step "assemble stage 1 and stage 2"
"$NASM" -f bin boot/stage1.asm -o "$BUILD_DIR/stage1.bin"
"$NASM" -f bin boot/stage2.asm -o "$BUILD_DIR/stage2.bin"

stage1_size=$(stat -c %s "$BUILD_DIR/stage1.bin")
stage2_size=$(stat -c %s "$BUILD_DIR/stage2.bin")
if [ "$stage1_size" -ne 512 ]; then
  echo "error: stage1.bin is $stage1_size bytes, must be 512" >&2
  exit 1
fi
if [ "$stage2_size" -ne $((STAGE2_SECTORS * 512)) ]; then
  echo "error: stage2.bin is $stage2_size bytes, must be $((STAGE2_SECTORS * 512))" >&2
  exit 1
fi

step "assemble BIOS disk image"
cat "$BUILD_DIR/stage1.bin" "$BUILD_DIR/stage2.bin" "$BUILD_DIR/kernel.bin" \
  > "$BUILD_DIR/baremetal.img"
truncate -s 1M "$BUILD_DIR/baremetal.img"

if [ -f /usr/lib/crt0-efi-x86_64.o ] && [ -d /usr/include/efi ]; then
  step "build UEFI loader"
  mkdir -p "$BUILD_DIR/uefi"
  UEFI_CFLAGS=(
    -m64 -std=c11 -O2 -Wall -Wextra
    -fpic -fshort-wchar -fno-stack-protector -mno-red-zone
    -mno-sse -mno-mmx -fno-asynchronous-unwind-tables
    -maccumulate-outgoing-args -DEFI_FUNCTION_WRAPPER
    -I/usr/include/efi -I/usr/include/efi/x86_64 -Ikernel
  )
  "$CC" "${UEFI_CFLAGS[@]}" -c boot/uefi/loader.c -o "$BUILD_DIR/uefi/loader.o"
  "$LD" -shared -Bsymbolic -nostdlib \
    -T /usr/lib/elf_x86_64_efi.lds \
    /usr/lib/crt0-efi-x86_64.o "$BUILD_DIR/uefi/loader.o" \
    -o "$BUILD_DIR/uefi/loader.so" -L/usr/lib -lefi -lgnuefi
  "$OBJCOPY" --target=efi-app-x86_64 \
    -j .text -j .sdata -j .data -j .dynamic -j .dynsym \
    -j .rel -j .rela -j .reloc \
    "$BUILD_DIR/uefi/loader.so" "$BUILD_DIR/uefi/BOOTX64.EFI"
else
  echo "skip: gnu-efi is not installed, UEFI loader not built" >&2
fi

if [ -f "$BUILD_DIR/uefi/BOOTX64.EFI" ] && command -v "$MFORMAT" >/dev/null 2>&1; then
  step "build UEFI system partition image"
  rm -f "$BUILD_DIR/esp.img"
  "$MFORMAT" -i "$BUILD_DIR/esp.img" -C -T 32768 -h 16 -s 32 ::
  "$MMD" -i "$BUILD_DIR/esp.img" ::/EFI
  "$MMD" -i "$BUILD_DIR/esp.img" ::/EFI/BOOT
  "$MCOPY" -i "$BUILD_DIR/esp.img" "$BUILD_DIR/uefi/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
  "$MCOPY" -i "$BUILD_DIR/esp.img" "$BUILD_DIR/kernel.bin" ::/EFI/BOOT/KERNEL.BIN
else
  echo "skip: mtools or the UEFI loader is missing, ESP image not built" >&2
fi

# The data volume the kernel mounts in phase 4. It is a real FAT32 filesystem
# with a known label, so the kernel's read-only driver is checked against an
# image any other system can inspect, and the label lets the kernel pick the
# volume by content instead of by controller or port position.
step "build data volume"
DATA_SECTORS=$((64 * 2048))            # 64 MiB: FAT32 wants more clusters than 16-bit FAT can address
DATA_IMAGE="$BUILD_DIR/data.img"
if ! command -v "$MFORMAT" >/dev/null 2>&1 || ! command -v "$MCOPY" >/dev/null 2>&1; then
  echo "error: mtools is required to build the data volume" >&2
  exit 1
fi
rm -f "$DATA_IMAGE"
"$MFORMAT" -i "$DATA_IMAGE" -C -F -T "$DATA_SECTORS" -v BMXDATA ::

printf '\nkernel.bin    %s bytes (limit %s)\n' "$kernel_size" "$KERNEL_LIMIT"
printf 'baremetal.img %s bytes\n' "$(stat -c %s "$BUILD_DIR/baremetal.img")"
printf 'data.img      %s bytes\n' "$(stat -c %s "$DATA_IMAGE")"
if [ -f "$BUILD_DIR/uefi/BOOTX64.EFI" ]; then
  printf 'BOOTX64.EFI   %s bytes\n' "$(stat -c %s "$BUILD_DIR/uefi/BOOTX64.EFI")"
fi
if [ -f "$BUILD_DIR/esp.img" ]; then
  printf 'esp.img       %s bytes\n' "$(stat -c %s "$BUILD_DIR/esp.img")"
fi
