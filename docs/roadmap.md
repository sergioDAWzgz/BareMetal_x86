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
      source, framebuffer address, mode, pitch, channel masks, RSDP address)
- [x] VBE mode selection in stage 2 by scanning the controller's mode list for
      1024x768x32 with a linear framebuffer (`0x118` is 24 bpp, not 32)
- [x] GOP mode selection in the UEFI loader, with the pixel format translated to
      the same mask description
- [x] direct map of the first 4 GiB at `0xFFFF800000000000`, so the framebuffer
      is writable as data no matter where the firmware put it
- [x] pixel pack/readback self-test plus colour bars
- [x] PCI configuration space enumeration through `0xCF8`/`0xCFC`

### 3b - input and firmware tables

- [x] 8x8 font rendering on the framebuffer, with the serial and VGA console
      mirrored onto it, cursor, wrapping and scrolling
- [x] PS/2 keyboard: 8042 init, scancode set 1 decoding with shift, caps lock and
      control, an IRQ1-driven character queue and a line echo loop
- [x] ACPI table discovery: RSDP from the EBDA/ROM scan on BIOS and from the UEFI
      configuration table, then the XSDT/RSDT walk, MADT (local APIC, IO APICs,
      interrupt source overrides) and MCFG (PCIe ECAM window)
- [ ] SMBus/`i2c` for the ALC888S codec, if it turns out to be needed in 5b

Gate: the kernel lists the JNF3A's RTL8111H and ALC888S and draws on the real
display. PCI enumeration, drawing and typing are already proven in QEMU on both
paths; the gate injects real keystrokes through the emulated 8042 and asserts the
line the kernel echoes back. ACPI is proven three ways: the BIOS scan, the UEFI
configuration table, and a q35 boot that publishes MCFG so the ECAM parse is
exercised instead of skipped.

## Phase 4 - storage and userspace (done)

- [x] AHCI driver, read sectors from the SATA port
- [x] read-only FAT32 and a ramdisk
- [x] ELF loader and a ring-3 process with syscalls
- [x] minimal libc

Gate: a user program loaded from disk prints through a syscall.

The data volume is built by `build.sh` as `build/data.img` and the kernel picks it
by its `BMXDATA` label, so the same gate runs on the `pc` machine and on q35, where
the boot disk and the data disk share one controller. The gate asserts the mounted
geometry, the file content read back, the staged image, the loaded entry point, a
syscall observed with `cs = 0x1b`, the program's own output and its exit status, and
that no mapping outside the user window is reachable from ring 3.

Two defects are recorded in `odd/tasks/phase4-storage-userspace.md` and worth
keeping in mind for phase 5: an AHCI command header that put the PRDT length in the
wrong dword let a read "succeed" while moving no bytes, and returning from the
syscall with `ret` instead of restoring the flags left the kernel with interrupts
disabled after the first ring-3 process.

## Phase 5 - the Linux payload (done)

The Gen7 hardware decode block is only reachable through a Linux driver, so a
from-scratch GPU driver is out of scope. **Decision taken: reuse the open source
Intel driver instead of writing one.** That makes 5b the target; 5a - software
decode inside our own kernel, realistic at 720p on the N2930 and not at all on the
N2807 - was discarded.

The boot chain stops being the destination. Our kernel loads a Linux kernel from
the same FAT32 volume and jumps into it, so all three boot paths run one handoff
through one code path.

- [x] pinned kernel source: `linux-6.12.115.tar.xz` from `cdn.kernel.org`, sha256
      checked against a constant, configured with `defconfig` plus the auditable
      `configs/linux-baytrail.fragment`
- [x] the setup-header handoff: relocation onto the first free `kernel_alignment`
      window, a reservation sized by `init_size` rounded up to that alignment, a
      zero page with the E820 map, the command line and `type_of_loader`, and a
      jump through a flat GDT to the 64-bit entry point
- [x] a gzip initramfs holding `/dev`, `/dev/console` and a freestanding `/init`
      that writes a marker to COM1 and powers the machine off
- [x] the gate asserts the handoff, Linux's own banner and the marker on the BIOS,
      q35 and UEFI paths

Gate: `test.sh` prints `PASS` on all three paths, and the serial log reaches
`bmx: initramfs marker` followed by `reboot: Power down`.

**The `vainfo` line is not a QEMU assertion.** `i915` and VA-API need Gen7 silicon
and QEMU has no such device, so "`vainfo` reports the H.264 decode entrypoint" moved
to phase 6, where there is a board to run it on. Phase 5 closes when the payload
reaches a userspace process on all three emulated paths; nothing here claims QEMU
decoded video, and the initramfs is the proof of the handoff rather than the final
root filesystem.

Two defects the gate found are worth keeping, because both failed silently: an
initramfs without `/dev/console` leaves PID 1 with no file descriptors, and a zero
`type_of_loader` makes the kernel ignore an initrd that is already in memory. In
both cases the only symptom is a panic about a missing root device. Both are
recorded in `odd/tasks/phase5-linux-payload.md`.

## Phase 6 - real board bring-up

- [ ] write the image to a USB stick and boot the JNF3A from it
- [ ] confirm BIOS settings: legacy vs UEFI boot, SATA mode, COM1 transmission mode
- [ ] confirm the serial cable pinout for the internal COM headers
- [ ] document the recovery path for a machine that fails to boot
- [ ] provide the real root filesystem, since the initramfs only proves the handoff:
      a distribution on the stick carrying `libva` and the Intel driver
- [ ] run `vainfo` on the board and confirm it reports the H.264 decode entrypoint
- [ ] play 1080p H.264 and measure the CPU load to prove the block is doing the work
