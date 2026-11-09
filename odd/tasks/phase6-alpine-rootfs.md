# Phase 6 - Alpine root filesystem

Feature: replace the phase 5 demo initramfs with a real Alpine root on the same
disk. Branch: `main`. Delivery: work-unit commits. Gate: `./test.sh` on the BIOS,
q35 and UEFI paths.

## Specs

- S1: `build.sh` writes `build/disk.img` with the FAT32 volume at LBA 2048 and an
  ext4 partition at LBA 133120 (type 0x83, 512 MiB) holding the Alpine root.
- S2: the MBR disk signature is `0x424D5836`, so the payload can name the root as
  `PARTUUID=424d5836-02`.
- S3: the payload command line is
  `root=PARTUUID=424d5836-02 rw console=ttyS0,115200` and no initramfs is passed.
- S4: the rootfs is Alpine `3.24.4`, built from the pinned minirootfs and
  `apk-tools-static`, with `alpine-base`, `libva`, `libva-intel-driver` and
  `libva-utils`; the serial console gives a root shell at 115200 8N1.
- S5: `./test.sh` asserts `VFS: Mounted root (ext4 filesystem)`,
  `Run /sbin/init as init process` and `OpenRC` on all three paths.
- S6: the board boots to the Alpine shell on COM1; `vainfo` is the next hardware
  check, not a QEMU assertion.

## Tasks

- T1 build the cached Alpine rootfs and the ext4 image - inline - pending
- T2 add the disk signature and partition 2 - inline - pending
- T3 boot the root without an initramfs - inline - pending
- T4 update the gate assertions and timing - inline - pending
- T5 update README and roadmap - inline - pending

## Log

L1: "Alpine" - the user chose Alpine for the real rootfs over Debian.

L2: A direct-kernel QEMU test mounted the Alpine ext4 image and ran OpenRC with a
root shell on ttyS0. `root=PARTUUID=424d5836-01` resolved to `sda1`, which confirms
the signature format before wiring partition 2.
