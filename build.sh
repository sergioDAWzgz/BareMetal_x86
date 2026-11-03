#!/usr/bin/env bash
# Builds the BareMetal_x86 boot image.
#
# Requires: nasm, gcc with 64-bit freestanding support, binutils, coreutils.
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

step "assemble disk image"
cat "$BUILD_DIR/stage1.bin" "$BUILD_DIR/stage2.bin" "$BUILD_DIR/kernel.bin" > "$BUILD_DIR/baremetal.img"
truncate -s 1M "$BUILD_DIR/baremetal.img"

printf '\nkernel.bin    %s bytes (limit %s)\n' "$kernel_size" "$KERNEL_LIMIT"
printf 'baremetal.img %s bytes\n' "$(stat -c %s "$BUILD_DIR/baremetal.img")"
