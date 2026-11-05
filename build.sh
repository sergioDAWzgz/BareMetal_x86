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
"$NASM" -f elf64 kernel/usermode.asm -o "$BUILD_DIR/kernel/usermode.o"
"$NASM" -f elf64 kernel/linuxjump.asm -o "$BUILD_DIR/kernel/linuxjump.o"
objects=("$BUILD_DIR/kernel/entry.o" "$BUILD_DIR/kernel/isr.o" \
         "$BUILD_DIR/kernel/usermode.o" "$BUILD_DIR/kernel/linuxjump.o" \
         "${objects[@]}")

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

# The user program. It is a separate link with its own flags: the kernel is
# linked for the higher half with the kernel code model, and this one is linked
# into the user window at 0x0000008000000000, which only the large code model
# can address with a 32-bit displacement-free sequence.
step "build user program"
USER_CFLAGS=(
  -m64 -std=c11 -O2 -Wall -Wextra
  -mcmodel=large -mno-red-zone
  -ffreestanding -fno-builtin -fno-pie -fno-stack-protector
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -mno-sse -mno-sse2 -mno-mmx -msoft-float
  # The syscall numbers are shared, so the user library sees the header the
  # kernel decodes the register block with instead of its own copy.
  -Ikernel
)
"$NASM" -f elf64 user/crt0.asm -o "$BUILD_DIR/crt0.o"
"$CC" "${USER_CFLAGS[@]}" -c user/libc.c -o "$BUILD_DIR/user-libc.o"
"$CC" "${USER_CFLAGS[@]}" -c user/main.c -o "$BUILD_DIR/user-main.o"
"$LD" -m elf_x86_64 -nostdlib -T user/link.ld \
  -o "$BUILD_DIR/USER.ELF" "$BUILD_DIR/crt0.o" "$BUILD_DIR/user-libc.o" \
  "$BUILD_DIR/user-main.o"

# The Linux payload. It is a pinned upstream tarball built with a fragment on top
# of defconfig. Both the version and the digest are constants here: a mirror that
# served something else would silently change the payload, and an unpinned version
# would make two builds of the same commit produce different images.
#
# The work area cannot live under build/. The kernel build system refuses to run
# from a path with a space or a colon in it, and this repository is checked out
# under "Linux distro for playing video" on some machines. The download, the
# extracted tree and the build tree therefore go to the XDG cache, which also puts
# them on the native filesystem rather than a Windows mount. Set BMX_LINUX_CACHE
# to move them.
#
# Building a kernel takes minutes while everything else in this script takes
# seconds, so the work area survives rm -rf build/: a clean of the artefacts must
# not force a rebuild of a kernel nothing changed. The stamp ties the build tree
# to one revision of the fragment; changing the fragment or the version rebuilds.
LINUX_VERSION=6.12.115
LINUX_SHA256=0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb
LINUX_FRAGMENT=configs/linux-baytrail.fragment
LINUX_CACHE=${BMX_LINUX_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/bmx}
case "$LINUX_CACHE" in
  /*) ;;
  *) LINUX_CACHE="$PWD/$LINUX_CACHE" ;;
esac
# The kernel's top-level Makefile aborts on a source path with a space or a colon,
# so the cache location is validated here where the reason can be printed.
case "$LINUX_CACHE" in
  *[[:space:]:]*)
    echo "error: BMX_LINUX_CACHE cannot contain a space or a colon: $LINUX_CACHE" >&2
    echo "       the Linux build system refuses such a path" >&2
    exit 1
    ;;
esac
LINUX_SOURCE="$LINUX_CACHE/linux-$LINUX_VERSION"
LINUX_TREE="$LINUX_CACHE/build-$LINUX_VERSION"
LINUX_TARBALL="$LINUX_CACHE/linux-$LINUX_VERSION.tar.xz"

step "build the Linux payload"
for tool in make flex bison bc curl tar; do
  if ! command -v "$tool" >/dev/null 2>&1; then
    echo "error: building the Linux payload needs $tool on the build host" >&2
    echo "       apt-get install build-essential bc flex bison libelf-dev" >&2
    exit 1
  fi
done

# tools/objtool is mandatory on x86_64 and links against libelf, so its headers
# are a build dependency of any x86_64 kernel in this series. The probe uses the
# same host flags make will use, so a header installed anywhere on the search path
# counts.
if ! printf '#include <gelf.h>\n' | ${HOSTCC:-gcc} ${HOSTCFLAGS:-} -E - >/dev/null 2>&1; then
  echo "error: the kernel build needs the libelf headers (tools/objtool uses them)" >&2
  echo "       apt-get install libelf-dev" >&2
  exit 1
fi

# certs/Makefile lists "hostprogs := extract-cert" unconditionally and certs/ is
# part of core-y, so every x86 build compiles that helper and links it against
# libcrypto, even with no modules and no system keyring. The probe links instead of
# only preprocessing, because a missing shared library fails at link time and would
# otherwise surface minutes into the build.
if ! printf '#include <openssl/bio.h>\nint main(void){return 0;}\n' | \
     ${HOSTCC:-gcc} ${HOSTCFLAGS:-} -x c - -o "$BUILD_DIR/.openssl-probe" \
     ${HOSTLDFLAGS:-} -lcrypto >/dev/null 2>&1; then
  echo "error: the kernel build needs libcrypto (certs/extract-cert links it)" >&2
  echo "       apt-get install libssl-dev" >&2
  exit 1
fi
rm -f "$BUILD_DIR/.openssl-probe"

# tools/scripts/Makefile.include runs "cd $(PWD)" with O= on the command line, and
# PWD is the shell's variable rather than make's: run make from a directory with a
# space in it and that cd stops at the space. Making the cache the working
# directory keeps PWD honest wherever this repository is checked out.
kernel_make() {
  ( cd "$LINUX_CACHE" && make -C "$LINUX_SOURCE" O="$LINUX_TREE" "$@" )
}

mkdir -p "$LINUX_CACHE"
if [ ! -f "$LINUX_TARBALL" ]; then
  echo "downloading linux-$LINUX_VERSION.tar.xz into $LINUX_CACHE" >&2
  curl -fsSL -o "$LINUX_TARBALL.part" \
    "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$LINUX_VERSION.tar.xz"
  mv "$LINUX_TARBALL.part" "$LINUX_TARBALL"
fi
echo "$LINUX_SHA256  $LINUX_TARBALL" | sha256sum -c --quiet -

# merge_config.sh is near the end of the archive, so its presence is a cheap
# completeness check for an extraction that was interrupted.
if [ ! -f "$LINUX_SOURCE/scripts/kconfig/merge_config.sh" ]; then
  rm -rf "$LINUX_SOURCE"
  tar -xf "$LINUX_TARBALL" -C "$LINUX_CACHE"
fi

fragment_stamp=$(sha256sum "$LINUX_FRAGMENT" | cut -d' ' -f1)
if [ ! -f "$LINUX_TREE/.fragment-$fragment_stamp" ]; then
  mkdir -p "$LINUX_TREE"
  # The configuration is regenerated from defconfig every time, so a fragment that
  # drops an option really drops it instead of leaving it in a stale .config. The
  # objects stay: make rebuilds whatever the new configuration affects, which turns
  # a fragment edit into a short incremental build instead of a full one.
  rm -f "$LINUX_TREE/.fragment-"*
  cp "$LINUX_FRAGMENT" "$LINUX_TREE/.fragment"
  kernel_make defconfig
  "$LINUX_SOURCE/scripts/kconfig/merge_config.sh" -m -Q -O "$LINUX_TREE" \
    "$LINUX_TREE/.config" "$LINUX_TREE/.fragment"
  kernel_make olddefconfig
  touch "$LINUX_TREE/.fragment-$fragment_stamp"
fi
kernel_make -j"$(nproc)" bzImage
cp "$LINUX_TREE/arch/x86/boot/bzImage" "$BUILD_DIR/bzImage"

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
"$MCOPY" -i "$DATA_IMAGE" data/HELLO.TXT ::/HELLO.TXT
"$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/USER.ELF" ::/USER.ELF
"$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/bzImage" ::/LINUX.BIN

printf '\nkernel.bin    %s bytes (limit %s)\n' "$kernel_size" "$KERNEL_LIMIT"
printf 'bzImage       %s bytes\n' "$(stat -c %s "$BUILD_DIR/bzImage")"
printf 'baremetal.img %s bytes\n' "$(stat -c %s "$BUILD_DIR/baremetal.img")"
printf 'data.img      %s bytes\n' "$(stat -c %s "$DATA_IMAGE")"
if [ -f "$BUILD_DIR/uefi/BOOTX64.EFI" ]; then
  printf 'BOOTX64.EFI   %s bytes\n' "$(stat -c %s "$BUILD_DIR/uefi/BOOTX64.EFI")"
fi
if [ -f "$BUILD_DIR/esp.img" ]; then
  printf 'esp.img       %s bytes\n' "$(stat -c %s "$BUILD_DIR/esp.img")"
fi
