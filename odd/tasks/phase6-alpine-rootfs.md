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

- T1 build the cached Alpine rootfs and the ext4 image - inline - done (1c92cd9)
- T2 add the disk signature and partition 2 - inline - done (1c92cd9)
- T3 boot the root without an initramfs - inline - done (1c92cd9)
- T4 update the gate assertions and timing - inline - done (1c92cd9)
- T5 update README and roadmap - inline - done (1c92cd9)

## Log

L1: "Alpine" - the user chose Alpine for the real rootfs over Debian.

L2: A direct-kernel QEMU test mounted the Alpine ext4 image and ran OpenRC with a
root shell on ttyS0. `root=PARTUUID=424d5836-01` resolved to `sda1`, which confirms
the signature format before wiring partition 2.

L3: On the board, `vainfo` failed with "failed to initialize display" until
`mount -t sysfs sysfs /sys` was run by hand. The rootfs's OpenRC runlevels are
empty, so nothing mounted sysfs; libva's DRM display calls `drmGetNodeTypeFromFd`,
which reads `/sys/dev/char/226:128/device/drm`. The inittab now mounts sysfs before
OpenRC. With sysfs mounted, the i965 driver reports H.264 High/Main/
ConstrainedBaseline/StereoHigh `VAEntrypointVLD` on the Bay Trail GPU.

L4: The rootfs now carries `ffmpeg` with VA-API, a pinned 1080p H.264 clip at
`/root/bmx-1080p.mp4`, and `bmx-decode`, which times a VA-API decode and a software
decode of the same clip. `bmx-check` prints whether ffmpeg has VA-API and whether
the clip is present; the gate asserts both lines. The CPU-load measurement itself
is the remaining board test.

L5: Board measurement on the N2807: VA-API used 5.04 user + 7.93 sys = 12.97
CPU-seconds over 19.02 s wall; software used 43.65 + 0.59 = 44.24 CPU-seconds over
22.94 s. That is 3.4x less CPU time and 2.8x less average load, so the Gen7 block
did the decode. The 7.93 s of system time and the 19 s wall time for a 10 s clip
mean the frame path is not zero-copy and not real-time; a DRM player with
`-hwaccel_output_format vaapi` is the next step.
