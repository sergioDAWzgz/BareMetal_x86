# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 6** - real board bring-up; the single disk image boots on
the JNF3A. 64-bit long mode, higher-half kernel, E820 memory map,
frame allocator, PIC/PIT, exception diagnostics, PCI enumeration, ACPI table
discovery (RSDP, MADT, MCFG), a 1024x768x32 framebuffer driven through VBE (BIOS)
or GOP (UEFI) with an 8x8 text console on it, a PS/2 keyboard or COM1 with a line
echo loop, an AHCI driver, a read-only FAT32 volume, an ELF64 loader, and a user
program that runs in ring 3 and prints through a syscall. Both boot paths run the
same kernel. The boot chain now ends by handing the machine to a Linux kernel
built from pinned source, which mounts a gzip initramfs and runs `/init` from it.
See `docs/roadmap.md`.

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

On the BIOS path `test.sh` also drives the keyboard: it boots QEMU with a monitor
socket and sends `hello` plus Enter through the emulated 8042, then asserts the
line the kernel echoes back. That needs `python3`; without it the keystroke check
is skipped and the rest still runs.

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
   ring-3 process, then initialises the 8042 keyboard and echoes typed lines read
   from the keyboard or COM1. After the echo loop yields it hands the machine to
   the Linux payload.

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

The kernel reads typed lines from two places: the 8042 keyboard through IRQ1 and
COM1 through a polled UART. The second is the practical one on the board: a USB
keyboard is owned by the firmware until `ExitBootServices` and by Linux after the
handoff, and the kernel has no USB stack of its own. The UART's loopback path is
exercised by a self-test on every gate run; typing into a terminal is the board
check.

COM1 is the debug console at 115200 8N1, no flow control. Connect it with a
**null-modem cable**: the board and a USB-serial adapter are both DTE, so TX and
RX have to cross. A straight-through cable shows the boot log and never delivers
input, which looks exactly like a kernel that ignores the keyboard. The echo loop
prints `kbd: serial bytes N` when it leaves, so a count of zero after typing says
the bytes never reached the UART, not that they were decoded wrong.

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
version they do not have. The fragment gives the payload an 8250 console,
devtmpfs/tmpfs/procfs/sysfs, ELF and script `binfmt`, the AHCI and PIIX storage
drivers, and `i915`; ext4 comes from `defconfig`. It also turns off modules (one
self-contained image) and KASLR (the loader reports where it put the kernel and
the gate asserts that address). Everything the root needs to mount is built in.

**The root filesystem.** Alpine 3.24, built from a pinned minirootfs and a pinned
`apk-tools-static`, both checked against constants. `apk` populates it without
root (`--usermode --no-chown`) with `alpine-base`, `libva`,
`libva-intel-driver` (the Gen7 i965 driver) and `libva-utils`, and `mke2fs -d`
writes the tree into a 512 MiB ext4 image. The serial console is an automatic
root shell (`ttyS0::respawn:/bin/sh -l`) and `/etc/profile.d/bmx-va.sh` selects
the i965 driver, so `vainfo` can be run as soon as the board reaches the prompt.
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
(`root=PARTUUID=424d5836-02 rw console=ttyS0,115200`), and enters the 64-bit entry
point through a flat GDT. The kernel mounts the root itself: ext4, AHCI and the
block layer are built in, so no initramfs is passed.

`type_of_loader` is still written as `0xFF` for a loader with no assigned id. It
no longer gates an initrd unpack, because the payload receives no initrd; it is
bookkeeping now.

**The gate.** All three paths must mount the root and start OpenRC:

```
VFS: Mounted root (ext4 filesystem)
Run /sbin/init as init process
OpenRC 0.63.2 is starting up
```

The gate waits for the OpenRC banner and then stops QEMU, so a slow OVMF boot does
not need a fixed timeout and a fast one does not waste it. The phase 5 initramfs
stays in the tree as the record of that phase; the image no longer passes it.

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
VA-API, the clip is present, `mpv --version` really starts, and both
`bmx-decode`/`bmx-play` are executable and pass `sh -n`. That catches a missing or
broken script without a display; the playback itself still needs the board.

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
