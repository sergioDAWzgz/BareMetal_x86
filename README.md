# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 3a** - 64-bit long mode, higher-half kernel, E820 memory map,
frame allocator, PIC/PIT, exception diagnostics, PCI enumeration and a 1024x768x32
framebuffer driven through VBE (BIOS) or GOP (UEFI). Both boot paths run the same
kernel. See `docs/roadmap.md`.

## Requirements

Build host (Linux, or WSL on Windows):

- `nasm`
- `gcc` (any x86_64 host compiler; no multilib needed)
- `binutils`, `coreutils`
- `qemu-system-x86_64` for the phase gate
- optional, for the UEFI path: `gnu-efi`, `mtools`, and `ovmf` for the gate

```sh
sudo apt-get install nasm binutils qemu-system-x86 gnu-efi mtools ovmf
```

## Build, test, run

```sh
./build.sh        # writes build/baremetal.img and the UEFI ESP image
./test.sh         # boots both paths in QEMU and asserts the serial output
./run.sh          # interactive boot, serial on stdout (BIOS path)
./run.sh --uefi   # same, through OVMF on the EFI path
```

`run.sh` and `test.sh` need QEMU. `build.sh` alone needs no emulator, and it skips
the UEFI artefacts with a warning when `gnu-efi` or `mtools` is missing.

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
   its own GDT with a TSS and a 48-vector IDT, remaps the PIC, starts the PIT, prints
   the memory map, initialises the frame allocator, probes the framebuffer and
   enumerates PCI.

**UEFI** (`boot/uefi/loader.c`, `BOOTX64.EFI` on an ESP):

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

Disk layout of `build/baremetal.img`:

| LBA | Content |
| --- | --- |
| 0 | stage 1, MBR |
| 1-16 | stage 2 |
| 17+ | kernel binary (up to 64 KiB) |

`build/esp.img` is a 16 MiB FAT image holding `\EFI\BOOT\BOOTX64.EFI` and
`\EFI\BOOT\KERNEL.BIN`.

## Writing to a USB stick

The BIOS image is a hard disk image and boots in legacy/CSM mode. Writing it
destroys the target device:

```sh
sudo dd if=build/baremetal.img of=/dev/sdX bs=1M conv=fsync
```

For the UEFI path, write `build/esp.img` to the stick and boot with UEFI enabled
and CSM off.

## Documentation

- `docs/hardware.md` - verified JNF3A and Bay Trail-M specifications, and what they
  imply for drivers
- `docs/roadmap.md` - phases, with the video decoding constraint on Gen7 graphics

## License

Not chosen yet.
