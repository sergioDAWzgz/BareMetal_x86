# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 2** - 64-bit long mode, higher-half kernel, E820 memory map,
frame allocator, PIC/PIT and exception diagnostics. See `docs/roadmap.md`.

## Requirements

Build host (Linux, or WSL on Windows):

- `nasm`
- `gcc` (any x86_64 host compiler; no multilib needed)
- `binutils`, `coreutils`
- `qemu-system-x86_64` for the phase gate

```sh
sudo apt-get install nasm binutils qemu-system-x86
```

## Build, test, run

```sh
./build.sh        # writes build/baremetal.img (1 MiB raw disk image)
./test.sh         # boots it in QEMU and asserts the serial output
./run.sh          # interactive boot, serial on stdout
```

`run.sh` and `test.sh` need QEMU. `build.sh` alone needs no emulator.

## Boot chain

1. **stage 1** (`boot/stage1.asm`, 512 bytes at LBA 0): sets up segments and stack,
   stores the boot drive at `0x0500`, reads stage 2 from LBA 1 through the BIOS
   extended disk read and jumps to it.
2. **stage 2** (`boot/stage2.asm`, 8 KiB at LBA 1): initialises COM1 at 115200 8N1,
   collects the BIOS E820 memory map at `0x0504`, verifies long mode support, loads
   the kernel to physical `0x10000` in two 32 KiB reads, enables A20, enters 32-bit
   protected mode, builds 4-level page tables mapping the first GiB both identity
   and at `0xFFFFFFFF80000000`, then enables IA-32e and jumps to the kernel.
3. **kernel** (`kernel/`, linked at `0xFFFFFFFF80010000`): clears `.bss`, installs
   its own GDT with a TSS and a 48-vector IDT, remaps the PIC, starts the PIT, prints
   the memory map, initialises the frame allocator and demonstrates fault handling.

Disk layout of `build/baremetal.img`:

| LBA | Content |
| --- | --- |
| 0 | stage 1, MBR |
| 1-16 | stage 2 |
| 17+ | kernel binary (up to 64 KiB) |

## Writing to a USB stick

The image boots from BIOS legacy mode as a hard disk. Writing it destroys the
target device:

```sh
sudo dd if=build/baremetal.img of=/dev/sdX bs=1M conv=fsync
```

Then boot the JNF3A from the stick with legacy/CSM boot enabled.

## Documentation

- `docs/hardware.md` - verified JNF3A and Bay Trail-M specifications, and what they
  imply for drivers
- `docs/roadmap.md` - phases, with the video decoding constraint on Gen7 graphics

## License

Not chosen yet.
