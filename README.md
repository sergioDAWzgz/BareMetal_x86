# BareMetal_x86

A bare-metal x86 system built from scratch for the Jetway JNF3A (Intel Bay Trail-M
Celeron N2807 / N2930) 3.5" single board computer: own bootloader, own kernel, and
eventually video playback on real hardware.

Current state: **phase 1** - stage 1 and stage 2 bootloaders, a 32-bit kernel and a
serial/VGA console. See `docs/roadmap.md`.

## Requirements

Build host (Linux, or WSL on Windows):

- `nasm`
- `gcc` with 32-bit support (`gcc-multilib` on Debian/Ubuntu)
- `binutils`, `coreutils`
- `qemu-system-i386` for the phase gate

```sh
sudo apt-get install nasm gcc-multilib binutils qemu-system-x86
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
2. **stage 2** (`boot/stage2.asm`, 4 KiB at LBA 1): initialises COM1 at 115200 8N1,
   loads the kernel to `0x10000` in two 32 KiB reads, enables A20, installs a flat
   GDT and enters 32-bit protected mode.
3. **kernel** (`kernel/`, linked at `0x10000`): clears `.bss`, prints to COM1 and the
   VGA text buffer, reports the CPUID brand string and the boot drive.

Disk layout of `build/baremetal.img`:

| LBA | Content |
| --- | --- |
| 0 | stage 1, MBR |
| 1-8 | stage 2 |
| 9+ | kernel binary (up to 64 KiB) |

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
