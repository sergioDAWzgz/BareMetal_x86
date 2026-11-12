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
      control, an IRQ1-driven character queue and a line echo loop (the loop was
      removed in phase 6, because it waited 30 s for input before booting Linux)
- [x] ACPI table discovery: RSDP from the EBDA/ROM scan on BIOS and from the UEFI
      configuration table, then the XSDT/RSDT walk, MADT (local APIC, IO APICs,
      interrupt source overrides) and MCFG (PCIe ECAM window)
- [ ] SMBus/`i2c` for the ALC888S codec, if it turns out to be needed in 5b

Gate: the kernel lists the JNF3A's RTL8111H and ALC888S and draws on the real
display. PCI enumeration, drawing and typing were proven in QEMU on both paths
while the echo loop existed; since phase 6 the gate keeps the pure scancode
self-test, the 8042 init and the assertion that the Linux handoff opens tty0. ACPI
is proven three ways: the BIOS scan, the UEFI configuration table, and a q35 boot
that publishes MCFG so the ECAM parse is exercised instead of skipped.

## Phase 4 - storage and userspace (done)

- [x] AHCI driver, read sectors from the SATA port
- [x] read-only FAT32 and a ramdisk
- [x] ELF loader and a ring-3 process with syscalls
- [x] minimal libc

Gate: a user program loaded from disk prints through a syscall.

The data volume is built by `build.sh` as a FAT32 partition inside
`build/disk.img` and the kernel picks it by its `BMXDATA` label, so the same gate
runs on the `pc` machine and on q35, where the boot record and the volume share
one controller and one disk. The gate asserts the mounted geometry and its base
LBA, the file content read back, the staged image, the loaded entry point, a
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

The board reaches storage through AHCI, so the boot medium is a SATA disk or an
mSATA module, not a USB stick: the kernel reads the payload through AHCI and has no
USB driver at all. The image carries both boot paths, so the firmware choice stays
open until the board itself says which one it uses.

- [x] one bootable image with a partition table: `build/disk.img` holds the MBR,
      stage 2, the kernel and the FAT32 volume at LBA 2048, and the kernel follows
      the table instead of assuming LBA 0; the gate boots that single file on the
      BIOS, q35 and UEFI paths
- [x] write `build/disk.img` to a SATA or mSATA disk and boot the JNF3A from it
- [x] re-test the board after the spurious IRQ7 fix: the second boot printed
      `irq: spurious IRQ7 ignored`, both exceptions resumed and the payload jumped
- [x] read typed input from COM1 as well as the 8042, so bring-up does not depend
      on a PS/2 keyboard the board does not list; the UART loopback self-test runs
      on every gate path
- [x] confirm on the board that typing into COM1 reaches the echo loop, with a
      null-modem cable: a straight-through cable carries the board's output but
      never delivers input, because both ends are DTE
- [x] confirm the firmware and port settings the boot relies on: UEFI boot, AHCI
      SATA mode and COM1 at 115200 8N1
- [x] document the internal serial headers from the Jetway manual (`G03-NF3A-F`,
      page 18): 9-pin block, DCD/RXD/TXD/DTR/GND/DSR/RTS/CTS/RI, with RS-422/485 on
      the COM1 header only; confirm pin 1 on the silkscreen before wiring
- [x] document the recovery path in `docs/recovery.md`: keep a known-good image,
      read COM1 first, and reflash the raw disk from another machine
- [x] provide the real root filesystem: Alpine 3.24 on a 512 MiB ext4 partition,
      built from a pinned minirootfs with `libva`, `libva-intel-driver` and
      `libva-utils`; the payload mounts it by PARTUUID and the gate reaches OpenRC
      on the BIOS, q35 and UEFI paths
- [x] run `vainfo` on the board: the i965 driver (Intel Bay Trail, 2.4.1) reports
      H.264 High, Main, ConstrainedBaseline and StereoHigh with `VAEntrypointVLD`;
      libva needs `/sys` mounted, which the inittab now does before OpenRC
- [x] put `ffmpeg` with VA-API and a pinned 1080p H.264 clip in the rootfs, with
      `bmx-decode` comparing hardware and software decode; the gate asserts both
      are present
- [x] prove the Gen7 block decodes 1080p: `bmx-decode` measured 12.97 CPU-s
      (5.04 user + 7.93 sys) through VA-API against 44.24 CPU-s (43.65 + 0.59)
      in software on the N2807, 3.4x less CPU time and 2.8x less average load
- [x] re-measure and confirm real-time playback: the zero-copy pass measured 2.24
      CPU-s (1.21 user + 1.03 sys) over 2.40 s against 44.32 CPU-s (43.68 + 0.64)
      over 22.98 s in software, 19.8x less CPU and 4.2x faster than real time;
      `bmx-play` decodes with VA-API on KMS/DRM
- [x] make the board's own console the normal one: i915 and fbcon paint tty0 on
      the monitor, the payload builds the input stack (`evdev`, `mousedev`,
      `atkbd`, `usbhid`), tty1 is a root shell and `gpm` gives the text console
      its mouse; COM1 stays the debug channel
- [x] remove the 30 s keyboard/serial echo loop: the kernel enters the payload as
      soon as the demos finish, so the boot waits for nothing but the payload's
      own decompression; the gate keeps the scancode self-test, the 8042 init and
      a check that the handoff opens tty0
- [x] start the console shells in root's home: `bmx-shell` sets `HOME=/root` and
      enters it before the login shell, on tty1 and on ttyS0
- [x] give the board network support: `r8169` (RTL8111H) and `e1000` (the QEMU
      gate NIC) are pinned in the payload, `/etc/network/interfaces` asks for
      DHCP on `eth0`, and `bmx-net` runs `ifup -a` in a background subshell so
      the console never waits for a cable; every gate path attaches an `e1000`
      and asserts that `eth0` exists. The rootfs carries
      `linux-firmware-rtl_nic` and the kernel loads its zstd file
      (`FW_LOADER_COMPRESS_ZSTD`) when r8169 opens the interface
- [ ] re-check the 1080p60 display path: `bmx-play` now uses `--profile=fast` and
      refuses the software fallback; the first run dropped 177 of 600 frames, so
      the display/render path is the limit, not the decode

The first boot on the board used the UEFI path and reached the exception demo:
ACPI found its ten tables, PCI enumerated fifteen devices and the PIT ticked. It
then halted on vector 39, an interrupt QEMU never raises, so the emulated gate
could not have caught it. The second boot confirmed the fix: the spurious line was
ignored, both exceptions resumed, AHCI found the volume on port 1 and the payload
jumped. A USB keyboard produced nothing in the bare-metal stage, because the
firmware stops emulating it at `ExitBootServices` and the kernel has no USB stack
of its own; that is why the boot-time echo loop also read COM1.
`irq_self_test` raises vector 39 on every path.

COM1 reports the firmware, our kernel and the Linux payload, and it is the only
channel that shows a failure before Linux is up, so it remains the debug console.
Input there needs a null-modem cable, because the board and a USB-serial adapter
are both DTE, so TX and RX have to cross. A straight-through cable is enough to
watch the boot and never enough to type into it.

Phase 6 removed the boot-time echo loop. The kernel now enters the payload as soon
as the storage demos finish instead of waiting 30 seconds for a keystroke, and the
interactive console is Linux's: i915 modesets the panel, fbcon paints tty0, and a
USB keyboard and mouse work through `usbhid`/`evdev`/`mousedev`, with `gpm` for
console selection. tty1 is a root shell on the monitor and ttyS0 is still a root
shell on COM1 for debugging; `bmx-shell` starts both in `/root`.

The image now has network support. `r8169` drives the board's RTL8111H and `e1000`
covers the QEMU gate; `/etc/network/interfaces` asks for DHCP on `eth0`, and
`bmx-net` runs `ifup -a` in a background subshell, so a board with no cable still
reaches its console immediately. `udhcpc` applies the lease (address, route,
`/etc/resolv.conf`) and keeps retrying in the background. Every gate path attaches
an `e1000` and asserts that `eth0` exists; the DHCP lease itself is a board check.

The image boots Alpine: the payload mounts the ext4 root by PARTUUID and OpenRC
starts on the BIOS, q35 and UEFI paths. The gate asserts that the handoff names
`tty0`, that the running kernel created `/dev/tty0`, `/dev/input/mice` and
`eth0`, and that `bmx-shell`/`bmx-net` parse; the BIOS path additionally waits for
the `bmx:~#` prompt and the DHCP lease, so `/root` and the network are asserted,
not only sampled by hand. `vainfo` reports the i965 driver and
the H.264 decode entrypoints, and the zero-copy `bmx-decode` proved the Gen7 block
does the 1080p decode: 2.24 CPU-seconds against 44.32 in software, with mpv
playing 1080p60 through KMS/DRM. The remaining phase 6 work is the 60 fps display
path.
