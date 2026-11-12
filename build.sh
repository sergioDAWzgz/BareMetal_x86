#!/usr/bin/env bash
# Builds the BareMetal_x86 boot artefacts.
#
# Requires: nasm, gcc with 64-bit freestanding support, binutils, coreutils.
# Optional: gnu-efi for the UEFI loader and mtools for the disk image.
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
MKE2FS=${MKE2FS:-mke2fs}

CFLAGS=(
  -m64 -std=c11 -O2 -Wall -Wextra
  -mcmodel=kernel -mno-red-zone
  -ffreestanding -fno-builtin -fno-pie -fno-stack-protector
  -fno-asynchronous-unwind-tables -fno-unwind-tables
  -mno-sse -mno-sse2 -mno-mmx -msoft-float
  -Ikernel
)

step() { printf '\n==> %s\n' "$1"; }

# Writes one 32-bit value to stdout as four little-endian bytes. The byte values
# are formatted as octal text first because command substitution cannot carry a
# NUL byte, and a partition start or length is mostly zero bytes.
le32() {
  local value=$1
  printf "\\$(printf '%03o' $((  value        & 0xFF)))"
  printf "\\$(printf '%03o' $((( value >>  8) & 0xFF)))"
  printf "\\$(printf '%03o' $((( value >> 16) & 0xFF)))"
  printf "\\$(printf '%03o' $((( value >> 24) & 0xFF)))"
}

# Emits one 16-byte MBR partition entry. The CHS triples are zeroed because every
# reader in this project addresses by LBA, and a second addressing scheme that
# disagrees with the first is only a way to boot the wrong sector.
mbr_entry() {
  local type=$1 start=$2 sectors=$3
  printf '\000'                              # status: no partition is active
  printf '\000\000\000'                      # first CHS address, unused
  printf "\\$(printf '%03o' "$type")"
  printf '\000\000\000'                      # last CHS address, unused
  le32 "$start"
  le32 "$sectors"
}

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

# The initramfs the Linux payload mounts as its root filesystem. It is a cpio
# "newc" archive with three entries: /dev, the console device, and /init.
#
# The device node is why the archive is written field by field instead of calling
# cpio: mknod needs a privilege the build must not assume, and the newc header
# carries the device numbers directly. The format is fixed by
# Documentation/driver-api/early-userspace/buffer-format.rst: the magic "070701",
# thirteen 8-digit hex fields, the name padded to four bytes, the data padded to
# four bytes.
#
# /dev/console is not decoration. With rdinit= pointing into the initramfs the
# kernel skips prepare_namespace(), so devtmpfs is never mounted; console_on_rootfs()
# therefore opens /dev/console out of the archive itself, and without that node PID
# 1 starts with no file descriptors at all.
step "build initramfs"

if ! command -v gzip >/dev/null 2>&1; then
  echo "error: the initramfs step needs gzip to compress the archive" >&2
  echo "       apt-get install gzip" >&2
  exit 1
fi

"$NASM" -f elf64 initramfs/init.asm -o "$BUILD_DIR/init.o"
"$LD" -m elf_x86_64 -nostdlib -static -o "$BUILD_DIR/init" "$BUILD_DIR/init.o"

# Zero bytes that bring a length up to the next multiple of four.
cpio_pad4() {
  local padding=$(( (4 - $1 % 4) % 4 ))
  while [ "$padding" -gt 0 ]; do
    printf '\000'
    padding=$((padding - 1))
  done
}

# cpio_entry name ino mode nlink rdev-major rdev-minor data-file
# The mode, inode and device numbers are the header's whole content; mtime is
# pinned to the epoch so two builds of the same tree produce the same archive.
cpio_entry() {
  local name=$1 ino=$2 mode=$3 nlink=$4 rdevmajor=$5 rdevminor=$6 data=$7
  local namesize=$(( ${#name} + 1 ))
  local filesize=0

  if [ -n "$data" ]; then
    filesize=$(stat -c %s "$data")
  fi

  printf '070701%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x' \
    "$ino" "$mode" 0 0 "$nlink" 0 "$filesize" 0 0 "$rdevmajor" "$rdevminor" \
    "$namesize" 0
  printf '%s\000' "$name"
  cpio_pad4 $(( 110 + namesize ))
  if [ -n "$data" ]; then
    cat "$data"
    cpio_pad4 "$filesize"
  fi
}

{
  cpio_entry dev         1 0040755 2 0 0 ""
  cpio_entry dev/console 2 0020600 1 5 1 ""
  cpio_entry init        3 0100755 1 0 0 "$BUILD_DIR/init"
  cpio_entry "TRAILER!!!" 0 0      1 0 0 ""
} | gzip -9 -n > "$BUILD_DIR/INITRD"

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
#
# The same filesystem is the UEFI system partition: the board has one disk, and
# one volume is enough for both paths. OVMF locates BOOTX64.EFI by path, not by
# partition type, and the kernel's FAT32 driver reads the BPB rather than the
# type byte, so 0x0C serves both. If a firmware ever refuses to boot from a
# non-0xEF partition, changing the type here is the whole fix.
step "build data volume"
DATA_SECTORS=$((64 * 2048))            # 64 MiB: FAT32 wants more clusters than 16-bit FAT can address
BOOT_SECTORS=2048                      # the FAT32 partition starts here, leaving room for the boot code
DATA_IMAGE="$BUILD_DIR/data.img"
if ! command -v "$MFORMAT" >/dev/null 2>&1 || \
   ! command -v "$MMD" >/dev/null 2>&1 || \
   ! command -v "$MCOPY" >/dev/null 2>&1; then
  echo "error: mtools is required to build the data volume" >&2
  exit 1
fi
rm -f "$DATA_IMAGE"
"$MFORMAT" -i "$DATA_IMAGE" -C -F -T "$DATA_SECTORS" -v BMXDATA ::
"$MCOPY" -i "$DATA_IMAGE" data/HELLO.TXT ::/HELLO.TXT
"$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/USER.ELF" ::/USER.ELF
"$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/bzImage" ::/LINUX.BIN
"$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/INITRD" ::/INITRD.BIN
if [ -f "$BUILD_DIR/uefi/BOOTX64.EFI" ]; then
  "$MMD" -i "$DATA_IMAGE" ::/EFI
  "$MMD" -i "$DATA_IMAGE" ::/EFI/BOOT
  "$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/uefi/BOOTX64.EFI" ::/EFI/BOOT/BOOTX64.EFI
  "$MCOPY" -i "$DATA_IMAGE" "$BUILD_DIR/kernel.bin" ::/EFI/BOOT/KERNEL.BIN
else
  echo "skip: the UEFI loader is missing, the volume has no EFI payload" >&2
fi

# The Alpine root filesystem the payload mounts as its real root. It is built
# from a pinned minirootfs and a pinned apk-tools-static binary, both checked
# against constants, and populated without root: apk's --usermode/--no-chown
# keeps the build host's uid out of the way, and mke2fs -d writes the tree into
# a filesystem image. The package list is what the phase 6 check needs: OpenRC
# to boot, libva and the Gen7 i965 driver for vainfo.
step "build Alpine root filesystem"
ALPINE_VERSION=3.24.4
ALPINE_BRANCH=v3.24
ALPINE_MIRROR="https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH"
ALPINE_MINIROOTFS_SHA256=cdaa38cb8de3537967ee5b6c9b6cd00cc962da2b018e6eea48696bb274923012
ALPINE_APK_TOOLS=apk-tools-static-3.0.9-r0.apk
ALPINE_APK_TOOLS_SHA256=0a1a0682375f66a83b7b903f6e4638ccfe312547d50d336b5f2be9fc391281be
ALPINE_PACKAGES="alpine-base ffmpeg gpm libva libva-intel-driver libva-utils linux-firmware-rtl_nic mesa-dri-gallium mpv"
ALPINE_CLIP_URL="https://test-videos.co.uk/vids/bigbuckbunny/mp4/h264/1080/Big_Buck_Bunny_1080_10s_1MB.mp4"
ALPINE_CLIP_SHA256=38d763af0161e28145e6300d9a5b3b1e4a387ed8f6c4ba7ac1a5f2948d8fac51
ALPINE_CACHE="$LINUX_CACHE/alpine-$ALPINE_VERSION"
ALPINE_CLIP="$ALPINE_CACHE/bmx-1080p.mp4"
ALPINE_ROOT="$ALPINE_CACHE/rootfs"
ALPINE_IMAGE="$BUILD_DIR/alpine.img"
ALPINE_SECTORS=$((512 * 2048))         # 512 MiB for the root filesystem
ROOT_START=$((BOOT_SECTORS + DATA_SECTORS))
if ! command -v "$MKE2FS" >/dev/null 2>&1; then
  echo "error: mke2fs (e2fsprogs) is required to build the Alpine rootfs" >&2
  echo "       apt-get install e2fsprogs" >&2
  exit 1
fi

mkdir -p "$ALPINE_CACHE"
ALPINE_MINIROOTFS="$ALPINE_CACHE/alpine-minirootfs-$ALPINE_VERSION-x86_64.tar.gz"
if [ ! -f "$ALPINE_MINIROOTFS" ]; then
  echo "downloading alpine-minirootfs-$ALPINE_VERSION into $ALPINE_CACHE" >&2
  curl -fsSL -o "$ALPINE_MINIROOTFS.part" \
    "https://dl-cdn.alpinelinux.org/alpine/$ALPINE_BRANCH/releases/x86_64/alpine-minirootfs-$ALPINE_VERSION-x86_64.tar.gz"
  mv "$ALPINE_MINIROOTFS.part" "$ALPINE_MINIROOTFS"
fi
echo "$ALPINE_MINIROOTFS_SHA256  $ALPINE_MINIROOTFS" | sha256sum -c --quiet -

ALPINE_APK_ARCHIVE="$ALPINE_CACHE/$ALPINE_APK_TOOLS"
if [ ! -f "$ALPINE_APK_ARCHIVE" ]; then
  curl -fsSL -o "$ALPINE_APK_ARCHIVE.part" \
    "$ALPINE_MIRROR/main/x86_64/$ALPINE_APK_TOOLS"
  mv "$ALPINE_APK_ARCHIVE.part" "$ALPINE_APK_ARCHIVE"
fi
echo "$ALPINE_APK_TOOLS_SHA256  $ALPINE_APK_ARCHIVE" | sha256sum -c --quiet -

if [ ! -x "$ALPINE_CACHE/apk.static" ]; then
  rm -rf "$ALPINE_CACHE/apk-extract"
  mkdir -p "$ALPINE_CACHE/apk-extract"
  tar -xzf "$ALPINE_APK_ARCHIVE" -C "$ALPINE_CACHE/apk-extract"
  cp "$ALPINE_CACHE/apk-extract/sbin/apk.static" "$ALPINE_CACHE/apk.static"
  chmod +x "$ALPINE_CACHE/apk.static"
fi

ROOTFS_STAMP=$(printf '%s\n' "$ALPINE_VERSION" "$ALPINE_PACKAGES" | \
  sha256sum | cut -d' ' -f1)
if [ ! -f "$ALPINE_CACHE/.rootfs-$ROOTFS_STAMP" ]; then
  echo "populating the Alpine rootfs" >&2
  rm -rf "$ALPINE_ROOT"
  mkdir -p "$ALPINE_ROOT"
  tar -xzf "$ALPINE_MINIROOTFS" -C "$ALPINE_ROOT" --exclude='./dev/*'
  "$ALPINE_CACHE/apk.static" add --root "$ALPINE_ROOT" --usermode --no-chown \
    --no-progress \
    --repository "$ALPINE_MIRROR/main" \
    --repository "$ALPINE_MIRROR/community" \
    $ALPINE_PACKAGES
  rm -f "$ALPINE_CACHE/.rootfs-"*
  touch "$ALPINE_CACHE/.rootfs-$ROOTFS_STAMP"
fi

# Two root shells, both deliberate on a bench image: tty1 is the board's own
# console (monitor, USB or PS/2 keyboard) and ttyS0 is the debug channel on
# COM1, which drops straight to a shell without a login prompt. bmx-shell puts
# both in root's home, and bmx-net brings the network up without ever blocking
# the console. The hostname line makes /etc/hostname real; ifupdown-ng also uses
# the system hostname as the DHCP client name. Once OpenRC is up the console
# loglevel drops to warnings, so runtime kernel info (NIC link up/down, driver
# chatter) stays in dmesg instead of printing over the console. bmx-clear then
# wipes the local VT (screen and scrollback) so tty1 opens on an empty screen;
# COM1 keeps the whole boot log. Both are wait actions, so neither races the
# network helpers or the shells. The local console is the normal one; COM1 stays
# because it is the only channel that reports a failure before Linux is up.
# sysfs is mounted here because the empty OpenRC runlevels leave it to the
# services, and libva's DRM display cannot classify the render node without
# /sys.
cat > "$ALPINE_ROOT/etc/inittab" <<'EOF'
::sysinit:/bin/mount -t sysfs sysfs /sys
::sysinit:/bin/hostname -F /etc/hostname
::sysinit:/usr/local/bin/bmx-check
::sysinit:/sbin/openrc sysinit
::sysinit:/sbin/openrc boot
::wait:/sbin/openrc default
::wait:/bin/dmesg -n 5
::wait:/usr/local/bin/bmx-clear
::once:/usr/local/bin/bmx-net
::once:/usr/local/bin/bmx-mouse
tty1::respawn:/usr/local/bin/bmx-shell
ttyS0::respawn:/usr/local/bin/bmx-shell
::ctrlaltdel:/sbin/reboot
::shutdown:/sbin/openrc shutdown
EOF

# The board's two NICs are the RTL8111H behind r8169 and the QEMU gate emulates
# e1000s, so both drivers are already built in. DHCP applies the leases through
# udhcpc's default script (address, route, /etc/resolv.conf); ifup only starts
# it, so the interfaces file is the single place the policy lives.
cat > "$ALPINE_ROOT/etc/network/interfaces" <<'EOF'
auto lo
iface lo inet loopback

auto eth0
iface eth0 inet dhcp

auto eth1
iface eth1 inet dhcp
EOF

cat > "$ALPINE_ROOT/etc/fstab" <<'EOF'
proc      /proc proc      defaults 0 0
sysfs     /sys  sysfs     defaults 0 0
devtmpfs  /dev  devtmpfs  defaults 0 0
tmpfs     /tmp  tmpfs     defaults 0 0
EOF

echo bmx > "$ALPINE_ROOT/etc/hostname"
mkdir -p "$ALPINE_ROOT/etc/profile.d"
echo 'export LIBVA_DRIVER_NAME=i965' > "$ALPINE_ROOT/etc/profile.d/bmx-va.sh"

# The 1080p H.264 clip and the scripts that measure the decode. The clip is
# small, pinned by digest, and lives in the root filesystem so the board test
# does not need a network.
if [ ! -f "$ALPINE_CLIP" ]; then
  echo "downloading the 1080p H.264 clip into $ALPINE_CACHE" >&2
  curl -fsSL -o "$ALPINE_CLIP.part" "$ALPINE_CLIP_URL"
  mv "$ALPINE_CLIP.part" "$ALPINE_CLIP"
fi
echo "$ALPINE_CLIP_SHA256  $ALPINE_CLIP" | sha256sum -c --quiet -
mkdir -p "$ALPINE_ROOT/root" "$ALPINE_ROOT/usr/local/bin"
cp "$ALPINE_CLIP" "$ALPINE_ROOT/root/bmx-1080p.mp4"

cat > "$ALPINE_ROOT/usr/local/bin/bmx-check" <<'EOF'
#!/bin/sh
if ffmpeg -hide_banner -hwaccels 2>/dev/null | grep -q vaapi; then
    echo "bmx: vaapi ffmpeg ready"
else
    echo "bmx: ffmpeg lacks vaapi"
fi
if [ -f /root/bmx-1080p.mp4 ]; then
    echo "bmx: 1080p clip present"
else
    echo "bmx: 1080p clip missing"
fi
if [ -c /dev/tty0 ]; then
    echo "bmx: local console ready"
else
    echo "bmx: local console missing"
fi
if [ -c /dev/input/mice ] && [ -x /usr/sbin/gpm ]; then
    echo "bmx: console mouse ready"
else
    echo "bmx: console mouse missing"
fi
for iface in eth0 eth1; do
    if [ -d "/sys/class/net/$iface" ]; then
        echo "bmx: network $iface ready"
    else
        echo "bmx: network $iface missing"
    fi
done
if [ -f /lib/firmware/rtl_nic/rtl8168h-2.fw ] ||
   [ -f /lib/firmware/rtl_nic/rtl8168h-2.fw.zst ]; then
    echo "bmx: network firmware ready"
else
    echo "bmx: network firmware missing"
fi
if ! command -v mpv >/dev/null 2>&1; then
    echo "bmx: mpv missing"
elif mpv --version >/dev/null 2>&1; then
    echo "bmx: mpv ready"
else
    echo "bmx: mpv broken"
fi
for script in /usr/local/bin/bmx-shell /usr/local/bin/bmx-clear \
              /usr/local/bin/bmx-net /usr/local/bin/bmx-decode \
              /usr/local/bin/bmx-play; do
    name=${script##*/}
    if [ -x "$script" ] && sh -n "$script"; then
        echo "bmx: $name ready"
    else
        echo "bmx: $name broken"
    fi
done
EOF

cat > "$ALPINE_ROOT/usr/local/bin/bmx-mouse" <<'EOF'
#!/bin/sh
# gpm turns the text console's mouse into selection and paste. It is optional:
# a board with no mouse keeps its console, so a missing device is not an error.
# The init script starts it after OpenRC and it daemonizes on its own.
if [ ! -c /dev/input/mice ]; then
    exit 0
fi
exec /usr/sbin/gpm -m /dev/input/mice -t imps2
EOF

cat > "$ALPINE_ROOT/usr/local/bin/bmx-shell" <<'EOF'
#!/bin/sh
# The console opens as root in root's home, not at the filesystem root. busybox
# init exports PATH, SHELL, USER and TERM but not HOME, so set it here and let
# the login shell read the profile from the right place.
HOME=/root
export HOME
cd "$HOME" 2>/dev/null || cd /
exec /bin/sh -l
EOF

cat > "$ALPINE_ROOT/usr/local/bin/bmx-clear" <<'EOF'
#!/bin/sh
# The boot log has done its job once the shells are about to start: clear the
# local virtual console so tty1 opens on an empty screen. COM1 keeps the full
# log for debugging. CSI 3J flushes the VT scrollback as well as the screen.
printf '\033[H\033[3J' > /dev/tty1
EOF

cat > "$ALPINE_ROOT/usr/local/bin/bmx-net" <<'EOF'
#!/bin/sh
# The network is best-effort: the console never waits for a cable or a DHCP
# server. ifup configures lo and both NICs, and udhcpc backgrounds itself; this
# subshell keeps even the first discovery off the boot path. Every lease is
# reported once on the console (COM1), and the online line appears only when
# both interfaces have one.
( ifup -a ) &
(
    i=0
    while [ "$i" -lt 20 ]; do
        for iface in eth0 eth1; do
            [ -f "/run/bmx-net.$iface" ] && continue
            addr=$(ip addr show dev "$iface" 2>/dev/null | awk '/inet /{print $2; exit}')
            if [ -n "$addr" ]; then
                echo "bmx: network up $iface $addr"
                : > "/run/bmx-net.$iface"
            fi
        done
        if [ -f /run/bmx-net.eth0 ] && [ -f /run/bmx-net.eth1 ]; then
            echo "bmx: network online"
            exit 0
        fi
        sleep 1
        i=$((i + 1))
    done
    [ -f /run/bmx-net.eth0 ] || echo "bmx: network down eth0"
    [ -f /run/bmx-net.eth1 ] || echo "bmx: network down eth1"
) &
exit 0
EOF

cat > "$ALPINE_ROOT/usr/local/bin/bmx-decode" <<'EOF'
#!/bin/sh
set -e
# Decode the bundled 1080p H.264 clip twice: once through VA-API and once in
# software. The user and system times are the proof that the Gen7 block did the
# work instead of the CPU, so a failed hardware pass must stop the script rather
# than look like a fast one. The hardware pass keeps frames in GPU memory.
CLIP=/root/bmx-1080p.mp4
if [ ! -f "$CLIP" ]; then
    echo "bmx-decode: $CLIP is missing"
    exit 1
fi
export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-i965}
echo "== vainfo =="
if ! vainfo > /tmp/bmx-vainfo.txt 2>&1; then
    echo "bmx-decode: vainfo failed"
    cat /tmp/bmx-vainfo.txt
    exit 1
fi
if ! grep -E 'Driver version|VAProfileH264' /tmp/bmx-vainfo.txt; then
    echo "bmx-decode: vainfo did not report the i965 H.264 profiles"
    exit 1
fi
echo "== hardware decode (VA-API, zero copy) =="
time ffmpeg -hide_banner -loglevel error -hwaccel vaapi \
    -hwaccel_output_format vaapi -hwaccel_device /dev/dri/renderD128 \
    -i "$CLIP" -c:v wrapped_avframe -f null - || {
    echo "bmx-decode: hardware decode failed"
    exit 1
}
echo "== software decode =="
time ffmpeg -hide_banner -loglevel error -i "$CLIP" -f null - || {
    echo "bmx-decode: software decode failed"
    exit 1
}
EOF

cat > "$ALPINE_ROOT/usr/local/bin/bmx-play" <<'EOF'
#!/bin/sh
set -e
# Play the clip on the board's display through KMS/DRM, with VA-API decode and
# no X11 or Wayland in the path.
CLIP=/root/bmx-1080p.mp4
if [ ! -f "$CLIP" ]; then
    echo "bmx-play: $CLIP is missing"
    exit 1
fi
export LIBVA_DRIVER_NAME=${LIBVA_DRIVER_NAME:-i965}
exec mpv --no-config --profile=fast --vo=gpu --gpu-context=drm \
    --drm-device=/dev/dri/card0 --hwdec=vaapi --hwdec-software-fallback=no \
    --osd-level=0 --no-audio --fs --msg-level=all=warn "$CLIP"
EOF
chmod +x "$ALPINE_ROOT/usr/local/bin/bmx-check" \
         "$ALPINE_ROOT/usr/local/bin/bmx-shell" \
         "$ALPINE_ROOT/usr/local/bin/bmx-clear" \
         "$ALPINE_ROOT/usr/local/bin/bmx-net" \
         "$ALPINE_ROOT/usr/local/bin/bmx-mouse" \
         "$ALPINE_ROOT/usr/local/bin/bmx-decode" \
         "$ALPINE_ROOT/usr/local/bin/bmx-play"

# busybox-suid installs bbsuid execute-only, and mke2fs -d runs unprivileged, so
# it cannot read the file to copy it. The image drops into a root shell and never
# uses su, so a readable copy is enough.
chmod 0755 "$ALPINE_ROOT/bin/bbsuid" 2>/dev/null || true

rm -f "$ALPINE_IMAGE"
"$MKE2FS" -q -t ext4 -b 4096 -d "$ALPINE_ROOT" "$ALPINE_IMAGE" 512M

# One image for one disk. The board has a single SATA port, so the boot record,
# stage 2, the kernel and the filesystems the kernel reads all live in the same
# sector space. The gate attaches this file alone on every path, which is the
# same layout a real board sees.
step "assemble the disk image"
DISK_IMAGE="$BUILD_DIR/disk.img"
rm -f "$DISK_IMAGE"
truncate -s $(( (BOOT_SECTORS + DATA_SECTORS + ALPINE_SECTORS) * 512 )) "$DISK_IMAGE"

dd if="$BUILD_DIR/stage1.bin" of="$DISK_IMAGE" bs=512 seek=0 \
   conv=notrunc status=none
dd if="$BUILD_DIR/stage2.bin" of="$DISK_IMAGE" bs=512 seek=1 \
   conv=notrunc status=none
dd if="$BUILD_DIR/kernel.bin" of="$DISK_IMAGE" bs=512 \
   seek=$((1 + STAGE2_SECTORS)) conv=notrunc status=none
dd if="$DATA_IMAGE" of="$DISK_IMAGE" bs=512 seek="$BOOT_SECTORS" \
   conv=notrunc status=none
dd if="$ALPINE_IMAGE" of="$DISK_IMAGE" bs=512 seek="$ROOT_START" \
   conv=notrunc status=none

# The disk signature and the partition table are written into the boot sector,
# which build.sh owns: only the build knows the layout and the signature the
# kernel command line names. Entry 1 is the FAT32 volume, entry 2 the Alpine
# root; the other two stay empty. No slot is active on purpose, so a BIOS runs
# this boot code instead of a volume's boot sector.
le32 0x424D5836 | dd of="$DISK_IMAGE" bs=1 seek=440 conv=notrunc status=none
mbr_entry 0x0C "$BOOT_SECTORS" "$DATA_SECTORS" > "$BUILD_DIR/mbr-entry1.bin"
dd if="$BUILD_DIR/mbr-entry1.bin" of="$DISK_IMAGE" bs=1 seek=446 \
   conv=notrunc status=none
mbr_entry 0x83 "$ROOT_START" "$ALPINE_SECTORS" > "$BUILD_DIR/mbr-entry2.bin"
dd if="$BUILD_DIR/mbr-entry2.bin" of="$DISK_IMAGE" bs=1 seek=462 \
   conv=notrunc status=none

printf '\nkernel.bin    %s bytes (limit %s)\n' "$kernel_size" "$KERNEL_LIMIT"
printf 'init          %s bytes\n' "$(stat -c %s "$BUILD_DIR/init")"
printf 'INITRD        %s bytes\n' "$(stat -c %s "$BUILD_DIR/INITRD")"
printf 'bzImage       %s bytes\n' "$(stat -c %s "$BUILD_DIR/bzImage")"
printf 'alpine.img    %s bytes\n' "$(stat -c %s "$ALPINE_IMAGE")"
printf 'disk.img      %s bytes\n' "$(stat -c %s "$DISK_IMAGE")"
if [ -f "$BUILD_DIR/uefi/BOOTX64.EFI" ]; then
  printf 'BOOTX64.EFI   %s bytes\n' "$(stat -c %s "$BUILD_DIR/uefi/BOOTX64.EFI")"
fi
