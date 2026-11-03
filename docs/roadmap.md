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

## Phase 2 - long mode and memory

- [ ] CPUID feature detection, identity paging, enter IA-32e long mode
- [ ] own GDT in C, IDT with exception handlers, exception messages on the console
- [ ] PIC remap, PIT tick, interrupt-driven console
- [ ] E820 memory map collection and a physical frame allocator
- [ ] kernel mapped at a higher-half address

Gate: deliberate `int 3` and divide-by-zero produce readable diagnostics, and the
memory map matches QEMU's `info mtree`.

## Phase 3 - devices

- [ ] PCI configuration space enumeration, print the device tree
- [ ] framebuffer through VBE (legacy) and GOP (UEFI), text and pixel drawing
- [ ] PS/2 keyboard input
- [ ] ACPI table discovery (RSDP, MADT, MCFG)

Gate: the kernel lists the JNF3A's RTL8111H and ALC888S and draws on the real
display.

## Phase 4 - storage and userspace

- [ ] AHCI driver, read sectors from the SATA port
- [ ] read-only FAT32 (or a small own filesystem) and a ramdisk
- [ ] ELF loader and a ring-3 process with syscalls
- [ ] minimal libc

Gate: a user program loaded from disk prints through a syscall.

## Phase 5 - video, the decision point

This is where the project forks, because of the Gen7 hardware decode constraint:

- **5a - own kernel only.** Software decode (H.264 baseline) into the framebuffer.
  Realistic at 720p on the N2930, not at 1080p, and not on the N2807.
- **5b - Linux payload.** Keep the loader and boot a Linux kernel plus initramfs;
  `i915` and VA-API then provide full hardware decode. This is the only path to
  real 1080p playback.

Gate: either a video plays at the documented resolution, or 5b boots and `vainfo`
reports the H.264 decode entrypoint.

## Phase 6 - real board bring-up

- [ ] write the image to a USB stick and boot the JNF3A from it
- [ ] confirm BIOS settings: legacy vs UEFI boot, SATA mode, COM1 transmission mode
- [ ] confirm the serial cable pinout for the internal COM headers
- [ ] document the recovery path for a machine that fails to boot
