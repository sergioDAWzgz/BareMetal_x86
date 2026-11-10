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

L6: The rootfs now installs mpv 0.41 and `mesa-dri-gallium` and ships `bmx-play`,
which runs mpv on KMS/DRM with `--hwdec=vaapi` and no X11 or Wayland. `bmx-decode`
keeps the hardware frames in GPU memory with `-hwaccel_output_format vaapi`, and
`bmx-check` also asserts that mpv is present. The rootfs is 419 MB inside the
512 MB partition. The board re-measurement is pending.

L7: Board re-measurement with zero-copy: hardware 1.21 user + 1.03 sys = 2.24
CPU-seconds over 2.40 s; software 43.68 + 0.64 = 44.32 CPU-seconds over 22.98 s.
That is 19.8x less CPU time and 4.2x faster than real time. `bmx-play` reports
`Using hardware decoding (vaapi)` and `VO: [gpu] 1920x1080 vaapi[nv12]`; it drops
177 of 600 frames on the 60 fps clip, so the DRM/GL display path is the remaining
bottleneck. `VT_GETMODE failed: Not a tty` and the fontconfig warning are cosmetic
on a COM1 console.
