# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 4** - 64-bit long mode, higher-half kernel, E820 memory map,
frame allocator, PIC/PIT, exception diagnostics, PCI enumeration, ACPI table
discovery (RSDP, MADT, MCFG), a 1024x768x32 framebuffer driven through VBE (BIOS)
or GOP (UEFI) with an 8x8 text console on it, a PS/2 keyboard with a line echo
loop, an AHCI driver, a read-only FAT32 volume, an ELF64 loader, and a user program
that runs in ring 3 and prints through a syscall. Both boot paths run the same
kernel. See `docs/roadmap.md`.

## Requirements

Build host (Linux, or WSL on Windows):

- `nasm`
- `gcc` (any x86_64 host compiler; no multilib needed)
- `binutils`, `coreutils`
- `mtools` (the data volume is built with `mformat`/`mcopy`)
- `qemu-system-x86_64` for the phase gate
- optional, for the UEFI path: `gnu-efi` and `ovmf` for the gate

```sh
sudo apt-get install nasm binutils qemu-system-x86 gnu-efi mtools ovmf
```

## Build, test, run

```sh
./build.sh        # writes build/baremetal.img, build/data.img and the UEFI ESP
./test.sh         # boots both paths in QEMU and asserts the serial output
./run.sh          # interactive boot, serial on stdout (BIOS path)
./run.sh --uefi   # same, through OVMF on the EFI path
```

`run.sh` and `test.sh` need QEMU. `build.sh` alone needs no emulator, and it skips
the UEFI artefacts with a warning when `gnu-efi` is missing.

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
   ring-3 process, then initialises the 8042 keyboard and echoes typed lines.

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
controller or port position, because q35 exposes the raw boot disk on the same
controller as the data disk.

`build/data.img` is that volume: 64 MiB, FAT32, built by `build.sh` with `mformat`
and filled with `HELLO.TXT` and the user program. `test.sh` attaches it to a
dedicated `ich9-ahci` controller on the `pc` machine and to the built-in AHCI
controller on q35.

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
