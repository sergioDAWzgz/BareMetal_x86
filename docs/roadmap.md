# Roadmap

Each phase is a gate: it is done when the image boots and `test.sh` asserts the
phase's observable outcome. Phases target the Jetway JNF3A (see
`docs/hardware.md`).

## Phase 1 - boot chain and kernel console (done)

- [x] stage 1 MBR reads stage 2 through the BIOS EDD interface
- [x] stage 2 initialises COM1, loads the kernel to `0x10000`, enables A20,
      installs a flat GDT and enters 32-bit protected mode
- [x] kernel prints to COM1 and VGA text, reports the CPUID brand string and the
      boot drive
- [x] `build.sh` builds a bootable 1 MiB raw image, `test.sh` asserts it in QEMU

Gate: `test.sh` prints `PASS`.

## Phase 2 - long mode and memory (done)

- [x] CPUID long mode check in stage 2, identity paging with 2 MiB pages, IA-32e
- [x] kernel linked at the higher half base `0xFFFFFFFF80010000`, first GiB mapped
      both identity and at that base
- [x] own GDT with a TSS in C, IDT with 48 vectors and exception diagnostics
- [x] PIC remap, PIT at 100 Hz, interrupt-driven tick counter
- [x] E820 memory map collection in stage 2, parsed by the kernel
- [x] physical frame bitmap allocator over the mapped GiB

Gate: `test.sh` prints `PASS`. A deliberate `int3` and a real divide error both
report vector, error code and RIP, then resume.

Note for phase 3: the identity mapping of the first GiB survived, and the direct map
of the first 4 GiB at `0xFFFF800000000000` was added next to it. Both loaders need
identity paging to keep executing their own code right after the CR3 switch, and the
direct map is what lets the kernel reach the framebuffer as data.


## Phase 3 - devices

Split in two: 3a is what the gate needs to prove the board's video and bus are
reachable, 3b is input and firmware tables.

### 3a - framebuffer and PCI (done)

- [x] boot info block published by both loaders at `0x0C00` (magic `BMX3`,
      source, framebuffer address, mode, pitch, channel masks)
- [x] VBE mode selection in stage 2 by scanning the controller's mode list for
      1024x768x32 with a linear framebuffer (`0x118` is 24 bpp, not 32)
- [x] GOP mode selection in the UEFI loader, with the pixel format translated to
      the same mask description
- [x] direct map of the first 4 GiB at `0xFFFF800000000000`, so the framebuffer
      is writable as data no matter where the firmware put it
- [x] pixel pack/readback self-test plus colour bars
- [x] PCI configuration space enumeration through `0xCF8`/`0xCFC`

### 3b - input and firmware tables (next)

- [ ] 8x8 font rendering on the framebuffer, console mirroring
- [ ] PS/2 keyboard input
- [ ] ACPI table discovery (RSDP, MADT, MCFG)
- [ ] SMBus/`i2c` for the ALC888S codec, if it turns out to be needed in 5b

Gate: the kernel lists the JNF3A's RTL8111H and ALC888S and draws on the real
display. PCI enumeration and drawing are already proven in QEMU on both paths.

## Phase 4 - storage and userspace

- [ ] AHCI driver, read sectors from the SATA port
- [ ] read-only FAT32 (or a small own filesystem) and a ramdisk
- [ ] ELF loader and a ring-3 process with syscalls
- [ ] minimal libc

Gate: a user program loaded from disk prints through a syscall.

## Phase 5 - video, the decision point

The Gen7 hardware decode block is only reachable through a Linux driver, so a
from-scratch GPU driver is out of scope. **Decision taken: reuse the open source
Intel driver instead of writing one.** That makes 5b the target.

- **5a - own kernel only** (fallback, not the goal). Software decode (H.264
  baseline) into the framebuffer. Realistic at 720p on the N2930, not at 1080p,
  and not on the N2807.
- **5b - Linux payload** (target). Keep the loader and boot a Linux kernel plus
  initramfs; the open source `i915` driver and VA-API then provide hardware
  decode. This is the only path to real 1080p playback without writing a driver.

Gate: 5b boots and `vainfo` reports the H.264 decode entrypoint.

## Phase 6 - real board bring-up

- [ ] write the image to a USB stick and boot the JNF3A from it
- [ ] confirm BIOS settings: legacy vs UEFI boot, SATA mode, COM1 transmission mode
- [ ] confirm the serial cable pinout for the internal COM headers
- [ ] document the recovery path for a machine that fails to boot
