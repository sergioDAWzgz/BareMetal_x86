# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 3b** - 64-bit long mode, higher-half kernel, E820 memory map,
frame allocator, PIC/PIT, exception diagnostics, PCI enumeration, ACPI table
discovery (RSDP, MADT, MCFG), a 1024x768x32 framebuffer driven through VBE (BIOS)
or GOP (UEFI) with an 8x8 text console on it, and a PS/2 keyboard with a line echo
loop. Both boot paths run the same kernel. See `docs/roadmap.md`.

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
   walks the ACPI tables, enumerates PCI, then initialises the 8042 keyboard and
   echoes typed lines.

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
