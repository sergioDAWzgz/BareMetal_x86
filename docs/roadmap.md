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

Note for phase 3: the identity mapping of the first GiB is still present. It exists
so stage 2 can run before the higher half switch and will be dropped once the
kernel no longer needs to touch physical addresses before the switch.


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
