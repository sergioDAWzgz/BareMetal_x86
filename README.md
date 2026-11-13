# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 6** - real board bring-up; the single disk image boots on
the JNF3A. 64-bit long mode, higher-half kernel, E820 memory map,
frame allocator, PIC/PIT, exception diagnostics, PCI enumeration, ACPI table
discovery (RSDP, MADT, MCFG), a 1024x768x32 framebuffer driven through VBE (BIOS)
or GOP (UEFI) with an 8x8 text console on it, PS/2 and COM1 input, an AHCI driver,
a read-only FAT32 volume, an ELF64 loader, and a user program that runs in ring 3
and prints through a syscall. Both boot paths run the same kernel. The boot chain
hands the machine to a Linux kernel built from pinned source, which mounts an
Alpine ext4 root and runs OpenRC. After that handoff the console is the board's
own monitor (i915 and fbcon on tty0) with a USB keyboard and mouse; COM1 remains
the debug channel. Phase 7 adds a UEFI installer ISO with keyboard selection and
default or manual MBR partitioning. See `docs/roadmap.md`.

## Requirements

Build host (Linux, or WSL on Windows):

- `nasm`
- `gcc` (any x86_64 host compiler; no multilib needed)
- `binutils`, `coreutils`
- `mtools` (the data volume is built with `mformat`/`mcopy`)
- `gzip` (the initramfs is compressed with it)
- `bc`, `flex`, `bison`, `libelf-dev`, `libssl-dev` to build the Linux payload
- `qemu-system-x86_64` for the phase gate
- optional, for the UEFI path: `gnu-efi` and `ovmf` for the gate

```sh
sudo apt-get install nasm binutils qemu-system-x86 gnu-efi mtools ovmf \
                     gzip bc flex bison libelf-dev libssl-dev
```

`build.sh` probes every kernel dependency before it starts. The `libelf` and
`openssl` probes compile *and link*, because a missing shared library fails at
link time rather than at the include.

## Build, test, run

```sh
./build.sh        # writes build/disk.img, the one image every path boots
./test.sh         # boots all three paths in QEMU and asserts the serial output
./run.sh          # interactive boot, serial on stdout (BIOS path)
./run.sh --uefi   # same, through OVMF on the EFI path
```

`run.sh` and `test.sh` need QEMU. `build.sh` alone needs no emulator, and it skips
the UEFI artefacts with a warning when `gnu-efi` is missing.

`build.sh` also builds the Linux payload, so the first build downloads and compiles
a kernel and takes a while. After that the build is incremental: the tarball, the
source tree and the object tree live in a cache and only what changed is rebuilt.
See "The Linux payload" below for where that cache is and why it is not in the
repository.

The gate boots every path to the Linux console: each one has to mount the ext4
root and print the OpenRC banner on COM1. It also asserts that the payload names
`tty0` in its command line and that the running kernel created `/dev/tty0` and
`/dev/input/mice`, so the local-console path cannot silently disappear.

## Boot chain

There are two boot paths to the same kernel binary.

**BIOS legacy** (MBR disk image):

1. **stage 1** (`boot/stage1.asm`, 512 bytes at LBA 0): sets up segments and stack,
   stores the boot drive at `0x0500`, reads stage 2 from LBA 1 through the BIOS
   extended disk read and jumps to it.
2. **stage 2** (`boot/stage2.asm`, 8 KiB at LBA 1): initialises COM1 at 115200 8N1,
   collects the BIOS E820 memory map at `0x0504`, verifies long mode support, scans
   the VBE mode list for 1024x768x32 and enables it with a linear framebuffer,
   publishes the boot info block at `0x0C00`, loads the kernel to physical `0x10000`
   in two 32 KiB reads, enables A20, enters 32-bit protected mode, builds 4-level
   page tables mapping the first 4 GiB both identity and at
   `0xFFFF800000000000` plus the kernel at `0xFFFFFFFF80000000`, then enables IA-32e
   and jumps to the kernel.
3. **kernel** (`kernel/`, linked at `0xFFFFFFFF80010000`): clears `.bss`, installs
   its own GDT with a TSS and a 48-vector IDT, remaps the PIC, starts the PIT,
   enables interrupts, prints the memory map, initialises the frame allocator,
   probes the framebuffer, switches to an 8x8 text console drawn straight onto it,
   walks the ACPI tables, enumerates PCI, runs the storage path from AHCI to a
   ring-3 process, then initialises the 8042 keyboard. It hands the machine to
   the Linux payload immediately; the interactive console is Linux's, not a demo
   stage in the kernel.

**UEFI** (`boot/uefi/loader.c`, `BOOTX64.EFI` on the FAT32 volume):

1. runs from `\EFI\BOOT\BOOTX64.EFI`, reports on COM1, picks a 1024x768 GOP mode and
   publishes the same boot info block at `0x0C00`
2. loads `\EFI\BOOT\KERNEL.BIN` to physical `0x10000`, synthesises an E820-style map
   from the UEFI memory map at `0x0504`
3. calls `ExitBootServices`, then builds the page tables and the GDT in fixed low
   memory (`0x70000` and `0x78000`) and jumps to the kernel virtual entry

Step 3 is deliberately after `ExitBootServices`: the firmware reclaims and reuses
the pages it handed out for `EfiLoaderData`, which silently overwrites page tables
built earlier. The post-exit phase also runs on its own stack, so it only reads
globals and constants.

The build writes one image, `build/disk.img`, and every path boots it. The board
has a single SATA port, so the boot record, the loader, the kernel and the
filesystem the kernel reads share one sector space:

| LBA | Content |
| --- | --- |
| 0 | stage 1, MBR with the partition table |
| 1-16 | stage 2 |
| 17-144 | kernel binary (up to 64 KiB) |
| 2048+ | FAT32 volume `BMXDATA`, 64 MiB |

The FAT32 partition is also the UEFI system partition: it holds
`\EFI\BOOT\BOOTX64.EFI` and `\EFI\BOOT\KERNEL.BIN`, and OVMF finds them by path,
not by partition type. No partition entry is marked active on purpose, so a BIOS
runs the MBR code instead of the volume's boot sector.

### Input

Before the handoff the kernel can read its own input two ways: the 8042 keyboard
through IRQ1 and COM1 through a polled UART. Neither is a boot gate: the payload
is entered as soon as the demos finish, so a board with nothing attached boots
just as fast. The UART receive path is still exercised by a loopback self-test on
every gate run.

After the handoff the console is the board's own. Linux's i915 driver modesets the
monitor and fbcon paints the virtual terminals on it, so a USB keyboard (or the
8042 one, when the board has it) types into tty1 and `gpm` gives the text console
its mouse. Both console shells start in root's home: `bmx-shell` sets `HOME=/root`
and enters it before starting the login shell. When the shells are about to start,
`bmx-clear` wipes the local VT (screen and scrollback), so tty1 opens on a blank
screen; `bmx-banner` then prints the OS name and the running kernel (`uname -r`).
The shells run bash with a Debian-like coloured prompt and `ls`/`grep` colours
from `/etc/profile.d/bmx-color.sh`; `bmx-console-font` sets Terminus 8x16 on the
monitor, the family and cell size Debian's console-setup uses, and the banner
leaves a blank line before the prompt. The kernel also carries `efifb` and
QEMU's `bochs-drm`: the first gives the UEFI installer a console on the
firmware's framebuffer, the second gives the emulated gate a screen, so the
console and the shutdown behaviour can be checked without the board. The rootfs
carries the Alpine `ca-certificates` bundle at
`/etc/ssl/certs/ca-certificates.crt`, so TLS clients work without any extra
package. `reboot` and `poweroff` are small wrappers in
`/usr/local/sbin`: they raise the kernel console log level and let OpenRC's
shutdown run on tty1, so the monitor shows the stopping services and the kernel
shutdown messages with their `[ seconds ]` timestamps instead of a silent
screen. COM1 keeps the boot log and a plain copy of the banner. COM1 remains the
debug channel at 115200 8N1, no flow control. A USB-serial adapter and the board
are both DTE, so the cable has to be **null-modem**: TX and RX cross. A
straight-through cable shows the boot log and never delivers input.

### How ACPI is found

The two firmware families do not publish the root pointer the same way, so the
kernel takes both routes:

- **BIOS**: the kernel scans the first KiB of the EBDA and the `0xE0000`-`0xFFFFF`
  ROM area for the `RSD PTR ` signature, and validates the checksum before using
  anything it finds.
- **UEFI**: there is no RSDP anywhere in the legacy segment, so the loader reads
  it from the ACPI configuration table (GUID `8868e871-e4f1-11d3-bc22-0080c73c8881`)
  and publishes the physical address in the boot info block. The kernel revalidates
  it there, because the boot info block is data and not a trusted pointer.

Every table is checksum-checked and dropped if it fails, table addresses outside
the 4 GiB direct map are ignored, and the MADT is walked by its own entry lengths
rather than a fixed stride.

## Storage and userspace

Phase 4 is the path from a SATA port to a ring-3 process.

The kernel finds the AHCI controller through PCI class `01:06:01`, brings up the
port carrying an attached drive and reads sectors through a command list, a
received FIS area and one PRDT entry per transfer. Buffers come from the frame
allocator as a physically contiguous run, because a PRDT and the direct map both
describe memory by physical address.

On top of that sits a read-only FAT32 driver: it parses the BPB, rejects a volume
whose geometry is inconsistent, walks the FAT chain and the root directory, and
reads a file by its 8.3 name. It never writes a sector. The volume is selected by
content - a consistent FAT32 volume carrying the label `BMXDATA` - and never by
controller or port position. When LBA 0 is an MBR instead of a boot sector, the
driver follows each entry the table names, which is how the single image lets the
kernel find the volume at LBA 2048.

`build/disk.img` carries that volume plus the Alpine root: 64 MiB of FAT32 at LBA
2048, built with `mformat` and filled with `HELLO.TXT`, the user program, the
compressed Linux kernel and the UEFI loader, and a 512 MiB ext4 partition at LBA
133120 with the root filesystem. `test.sh` attaches the same file on all three
paths: on an added `ich9-ahci` controller for `pc`, and on the built-in AHCI
controller for q35.

A file is staged into RAM first (`kernel/ramdisk.c`), then `kernel/elf.c` maps its
`PT_LOAD` segments into user pages. User space lives in `PML4[1]`, a subtree no
loader touches: both loaders point `PML4[0]` and `PML4[256]` at the same page
directory as the kernel window, so a user bit anywhere there would open the
identity map, the direct map or the kernel itself to ring 3. The program links at
`0x0000008000000000` with the large code model, and `vmm_audit_supervisor` walks
the loader subtrees counting user-accessible entries; the gate requires zero.

The program runs in ring 3 on its own stack. `int 0x80` is the only way back in: an
interrupt gate with `DPL 3` whose handler checks that the buffer lies inside the
user window and that every page it touches is mapped before reading a byte. `write`
and `exit` are the whole syscall set for now, with `puts`, `strlen`, `memset` and
`memcpy` built on top of them in `user/libc.c`.

Leaving ring 3 cannot use `iretq` - the user frames are abandoned on purpose - so
the exit call restores the kernel stack and flags saved at entry and returns to the
caller that started the process.

## The Linux payload

Phase 5 stops treating the boot chain as the destination. After the demos, the
kernel loads a Linux kernel from the same FAT32 volume and jumps into it, so the
Gen7 block that only a real driver reaches becomes reachable.

**What is built.** `build.sh` downloads `linux-6.12.115.tar.xz` (the 6.12 longterm)
from `cdn.kernel.org` and refuses it unless the sha256 matches the constant next to
the version. It configures with `defconfig` plus `configs/linux-baytrail.fragment`
through `merge_config.sh`, so the file lists every deviation from a generic x86_64
kernel and a reader can audit it without diffing a 5000-line `.config` against a
version they do not have. The fragment gives the payload an 8250 console, a
framebuffer console (`DRM_FBDEV_EMULATION`), devtmpfs/tmpfs/procfs/sysfs, ELF and
script `binfmt`, the AHCI and PIIX storage drivers, `i915`, the input stack
(`evdev`, `mousedev`, `atkbd`, `usbhid`) the local console needs, both network
drivers (`r8169` for the board's RTL8111H, `e1000` for the QEMU gate), and zstd
firmware loading for the `rtl_nic` package; ext4 comes from `defconfig`. It also turns off modules (one self-contained image) and KASLR (the
loader reports where it put the kernel and the gate asserts that address).
Everything the root needs to mount is built in.

**The root filesystem.** Alpine 3.24, built from a pinned minirootfs and a pinned
`apk-tools-static`, both checked against constants. `apk` populates it without
root (`--usermode --no-chown`) with `alpine-base`, `libva`,
`libva-intel-driver` (the Gen7 i965 driver) and `libva-utils`, and `mke2fs -d`
writes the tree into a 512 MiB ext4 image. Both consoles are automatic root
shells: tty1 on the monitor and ttyS0 on COM1, which preserves the debug channel,
both launched by `bmx-shell` in `/root`. `gpm` gives the text console its mouse,
`/etc/network/interfaces` asks for DHCP on `eth0` and `eth1`, the rootfs carries
`linux-firmware-rtl_nic` for the board's PHY patch, and
`/etc/profile.d/bmx-va.sh` selects the i965 driver, so `vainfo` can be run as soon
as the board reaches the prompt.
The inittab mounts sysfs before OpenRC: libva's DRM display asks the kernel for the
node type through `/sys`, and without it `vainfo` reports that no display could be
initialized. Because the build runs unprivileged, the files in the image belong to
the build user rather than root; init runs as root and the bench shell reads them
anyway.

**Where it is built.** Not in this checkout. The kernel `Makefile` aborts with
`*** source directory cannot contain spaces or colons.`, and this repository lives
under `Linux distro for playing video`. Everything is kept in
`${BMX_LINUX_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/bmx}` instead: the tarball, the
extracted source and an out-of-tree object directory. Nothing there is tracked by
git, and deleting it costs a rebuild and nothing else.

**The handoff** (`kernel/linuxboot.c`, `kernel/linuxjump.asm`). The loader parses
the setup header at offset `0x1f1`, checks the `HdrS` magic and the protocol
version, and reads `pref_address`, `kernel_alignment`, `init_size` and
`relocatable`. The preferred address is treated as a hint: the loader walks up in
`kernel_alignment` steps to the first window that is free and reserves
`align_up(init_size, kernel_alignment)` bytes past the decompression target, which
is what the decompressor actually writes rather than the smaller `init_size`. It
builds a zero page carrying the E820 map and the command line
(`root=PARTUUID=424d5836-02 rw console=tty0 console=ttyS0,115200`), and enters the
64-bit entry point through a flat GDT. The kernel mounts the root itself: ext4,
AHCI and the block layer are built in, so no initramfs is passed.

`type_of_loader` is still written as `0xFF` for a loader with no assigned id. It
no longer gates an initrd unpack, because the payload receives no initrd; it is
bookkeeping now.

**The gate.** All three paths must mount the root and start OpenRC:

```
VFS: Mounted root (ext4 filesystem)
Run /sbin/init as init process
OpenRC 0.63.2 is starting up
```

The BIOS path waits for the console prompt, both DHCP leases and the online
summary before stopping QEMU; q35 and UEFI stop at the OpenRC banner, so a slow
OVMF boot does not need a fixed timeout and a fast one does not waste it. The gate
asserts that the handoff names `tty0`, that the booted kernel created `/dev/tty0`
and `/dev/input/mice`, that `eth0` and `eth1` exist with the r8169 firmware, and
that `bmx-shell`/`bmx-net` parse, so the local console and the network path cannot
silently disappear from the image. The BIOS log also proves the shell starts in
root's home (`bmx:~#`) and that DHCP configured both interfaces
(`bmx: network up eth0/eth1`). Every path attaches two `e1000` NICs on different
subnets, which is what makes `eth0` and `eth1` exist under QEMU. The gate also
checks that `bmx-clear` parses, though the clear itself writes to tty1 and is a
board-visible behaviour, not a serial assertion. The phase 5 initramfs stays in
the tree as the record of that phase; the image no longer passes it.

**What QEMU cannot prove.** `i915` and VA-API need Gen7 silicon and QEMU has no such
device, so the roadmap line "`vainfo` reports the H.264 decode entrypoint" is a
phase 6 hardware assertion. Phase 5 is done when the payload reaches a userspace
process on the BIOS, q35 and UEFI paths, and nothing here claims QEMU decoded video.

## Video decode on the board

The root filesystem carries `ffmpeg` built with VA-API and a pinned 1080p H.264
clip at `/root/bmx-1080p.mp4`. Run:

```sh
bmx-decode
```

It prints `vainfo`, then decodes the clip twice: once through
`/dev/dri/renderD128` and once in software, with the shell's `time` around each.
The hardware pass keeps frames in GPU memory (`-hwaccel_output_format vaapi`), so
the system time is the frame path, not a download. The difference in user+sys time
is the proof that the Gen7 block did the decode. The script needs `/sys` mounted,
which the inittab already does. The clip is downloaded and checked against a
sha256 at build time, so booting the image needs no network.

Visual playback uses the same clip on the board's display through KMS/DRM:

```sh
bmx-play
```

That runs `mpv --vo=gpu --gpu-context=drm --hwdec=vaapi`; the rootfs carries
`mpv` and `mesa-dri-gallium` for the display path, with no X11 or Wayland. The
decode measurement does not need a display; `bmx-play` is the visual check.

On the JNF3A the zero-copy decode measured 2.24 CPU-seconds against 44.32 in
software (19.8x less) and ran 4.2x faster than real time. `mpv` plays 1080p60
through KMS/DRM with VA-API but drops about 30% of frames at 60 fps, so the
display path, not the decode, is the remaining limit.

`bmx-check` runs before OpenRC and the gate asserts its lines: ffmpeg advertises
VA-API, the clip is present, `mpv --version` really starts, the local console and
mouse devices exist, `eth0` was created, and the `bmx-*` helpers are executable
and pass `sh -n`. That catches a missing or broken script without a display; the
playback itself still needs the board.

## Networking

The board has two RTL8111H NICs, driven by the built-in `r8169`; the QEMU gate
attaches two `e1000` devices on different subnets. Both drivers are pinned in the
kernel fragment, and the kernel already carries the IPv4 stack. The root
filesystem configures `lo`, `eth0` and `eth1` from `/etc/network/interfaces`:

```
auto eth0
iface eth0 inet dhcp

auto eth1
iface eth1 inet dhcp
```

At boot `bmx-net` runs `ifup -a` in a background subshell, so a missing cable or
DHCP server never delays the console. ifupdown-ng starts busybox `udhcpc`, which
applies each lease through its default script (address, route and
`/etc/resolv.conf`) and keeps retrying in the background. The inittab also applies
`/etc/hostname` before OpenRC, so the system hostname is `bmx` and udhcpc sends it
as the client name. Run:

```sh
ip addr show eth0        # the leased address on the first NIC
ip addr show eth1        # the second NIC gets its own lease
ip route                 # the default routes from DHCP
ping -c1 1.1.1.1         # link and route
wget -qO- http://example.com/
```

`bmx-net` prints `bmx: network up <iface> <addr>` on COM1 for each lease as it
arrives, `bmx: network down <iface>` after twenty seconds without one, and
`bmx: network online` once both interfaces are up; none of it blocks boot. After
OpenRC the inittab drops the console loglevel to warnings (`dmesg -n 5`), so
runtime kernel info such as NIC link up/down chatter goes to `dmesg` instead of
being printed over the console; `dmesg | tail` still shows it. The rootfs carries
`linux-firmware-rtl_nic`: r8169 requests `rtl8168h-2.fw` when each interface
opens, and the kernel loads the package's `.fw.zst` file through
`FW_LOADER_COMPRESS_ZSTD`. r8169 tolerates the file being absent, but the 8168H
PHY patch is what the board's RTL8111H asks for.

## Installer ISO

`installer/build-iso.sh` builds `build/installer/bmx-installer.iso` (about 214 MiB),
a UEFI El Torito image that carries an installer kernel, an installer initramfs
and the OS payload. Build the disk image first and have `xorriso` on the host:

```sh
./build.sh
sudo apt-get install xorriso
./installer/build-iso.sh
```

Write the ISO to a USB stick or attach it as a CD. The board boots it through
UEFI; the installer itself has no BIOS path (the installed system still boots
both paths).

The first screen is drawn in the same visual style as the rest of the TUI: a
blue screen with a white panel and a blue title, the theme `dialog` uses, but
painted directly on tty0 because `dialog` draws on the alternate screen and
would repaint everything on exit. Only the countdown line is rewritten, so the
number changes in place without a full-screen flicker. If no key is pressed, the
default install runs: first internal disk, Spanish console keymap, one MBR with a
64 MiB FAT32 ESP and an ext4 root that fills the rest of the disk, and the same
boot chain as the prebuilt image. The default path asks no questions even when
the target already holds an installation: it wipes the old signatures, rewrites
the table and reformats both filesystems. Any key opens the menu, where the
keyboard, the target disk and `cfdisk` manual partitioning can be chosen. Manual
mode requires one FAT32 partition (type `c`) and one ext4 root (type `83`), with
the first partition starting after sector 144; the installer generates the disk
signature and patches `root=PARTUUID=<disk>-<part>` into the kernel before
writing it.

The chosen keymap is stored as `/etc/keymap` in the installed rootfs and loaded
by `bmx-keymap` at boot; the default is `es`. `installer/test-iso.sh` installs
into a blank 2 GiB QEMU disk and boots the result through OVMF; it needs `qemu`,
`ovmf`, `mtools` and `python3`, and `BMX_TEST_MEDIA=usb` or `BMX_TEST_MEDIA=fat`
boots the ISO as a raw USB image or as a Rufus-style extracted FAT stick instead
of a CD-ROM. `BMX_TEST_KEEP=1` reuses the target disk, so running the script a
second time exercises the default install over an existing installation.

The installer is destructive: it rewrites the target disk's MBR and both
filesystems.

## Writing the image to a disk

`build/disk.img` is a complete disk: the MBR, the loader, the kernel, a 64 MiB
FAT32 volume that serves both boot paths, and a 512 MiB ext4 partition with the
Alpine root (about 577 MiB in total). Writing it destroys the target device:

```sh
sudo dd if=build/disk.img of=/dev/sdX bs=1M conv=fsync
```

The board has one SATA II port and one mSATA slot sharing the full-size mPCIe
connector, so either medium works. A USB stick is not a supported boot medium: the
kernel reads the volume through AHCI, and a pendrive hangs off EHCI or xHCI, which
the kernel does not drive. On Windows, write the same file with a raw disk imager;
it is not an ISO.

The image boots on the BIOS path directly and on the UEFI path through the same
FAT32 volume, so a board can be tried in either mode without rebuilding anything.

## Documentation

- `docs/hardware.md` - verified JNF3A and Bay Trail-M specifications, the serial
  header pinout, and what they imply for drivers
- `docs/recovery.md` - how to recover a board whose disk no longer boots
- `docs/roadmap.md` - phases, with the video decoding constraint on Gen7 graphics

## License

Not chosen yet.
