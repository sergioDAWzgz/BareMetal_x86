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
  `root=PARTUUID=424d5836-02 rw console=tty0 console=ttyS0,115200` and no
  initramfs is passed; ttyS0 is last so `/dev/console` stays on COM1.
- S4: the rootfs is Alpine `3.24.4`, built from the pinned minirootfs and
  `apk-tools-static`, with `alpine-base`, `gpm`, `libva`, `libva-intel-driver`
  and `libva-utils`; tty1 gives a root shell on the monitor and ttyS0 a root
  shell on COM1 at 115200 8N1.
- S5: `./test.sh` asserts `VFS: Mounted root (ext4 filesystem)`,
  `Run /sbin/init as init process` and `OpenRC` on all three paths.
- S6: the board boots to the Alpine shell on COM1; `vainfo` is the next hardware
  check, not a QEMU assertion.
- S7: the kernel enters the payload without waiting for input, and Linux builds
  the local console: i915 + fbcon on tty0, USB keyboard and mouse through
  `usbhid`/`evdev`/`mousedev`, and `gpm` on the text console.
- S8: both console shells start in `/root`, with `HOME=/root`, instead of the
  filesystem root.
- S9: `eth0` and `eth1` are configured by DHCP from `/etc/network/interfaces`,
  applied by ifupdown-ng and busybox `udhcpc`; the network start never blocks the
  console, and the gate asserts that both interfaces exist.
- S10: both DHCP leases are reported per interface, and `bmx: network online`
  appears only when both are up; the BIOS gate waits for it.

## Tasks

- T1 build the cached Alpine rootfs and the ext4 image - inline - done (1c92cd9)
- T2 add the disk signature and partition 2 - inline - done (1c92cd9)
- T3 boot the root without an initramfs - inline - done (1c92cd9)
- T4 update the gate assertions and timing - inline - done (1c92cd9)
- T5 update README and roadmap - inline - done (1c92cd9)
- T6 local console and instant boot - inline - done (8dd6bc8)
- T7 console home and network - inline - done (00c8ea3)
- T8 second NIC and firmware - inline - done (this commit)

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

L8: The three follow-ups are addressed. `bmx-play` now uses `--profile=fast`,
`--osd-level=0` and `--hwdec-software-fallback=no`, so the display path is cheaper
and a VA-API failure stops playback instead of falling back to software; the
60 fps board re-test is pending. The internal COM headers are documented from the
Jetway `G03-NF3A-F` manual page 18: 9-pin block, pin 1 DCD, 2 RXD, 3 TXD, 4 DTR,
5 GND, 6 DSR, 7 RTS, 8 CTS, 9 RI, with RS-422/485 on the COM1 header only.
`docs/recovery.md` documents the known-good image and the reflash procedure.

L9: `bmx-check` now runs `mpv --version` instead of a PATH lookup and parses both
`bmx-decode` and `bmx-play` with `sh -n`; the gate asserts the three new markers.
That covers the static part of the `R3-play-untested` escalation. Real playback
still needs the board, because QEMU has no i915.

L10: "Perfecto. ¿Podrías hacer que la consola se pudiera utilizar desde la propia
placa conectada a un monitor con teclado y ratón, en vez de a través del puerto
debug? Elimina también los 30 segundos de espera para recibir input a través del
puerto debug: que el arranque sea instantáneo"

The payload command line opens tty0 and keeps ttyS0 last, so the kernel also
paints the monitor while `/dev/console` and the gate stay on COM1. The fragment
enables `DRM_FBDEV_EMULATION`/`FRAMEBUFFER_CONSOLE` and pins
evdev/mousedev/atkbd/usbhid, so i915 + fbcon give a local text console and a USB
keyboard and mouse work. The rootfs adds `gpm`, tty1 is a root shell on the
monitor, ttyS0 remains the COM1 debug shell, and `bmx-check` reports
`local console ready` and `console mouse ready`. The 30 s echo loop and the
serial receive counters are gone: the kernel enters the payload as soon as the
demos finish. The gate drops the keystroke injection and asserts the `tty0`
string in the handoff plus the two new boot markers.

L11: "Vale, perfecto. Si no supone demasiada molestia, me gustaría que el sistema
operativo iniciara en el directorio personal del usuario «root», en lugar de la
raíz del sistema de archivos. Además, quiero que este sistema operativo cuente
con soporte de red en la placa."

`bmx-shell` sets `HOME=/root`, enters it and execs the login shell, and both tty1
and ttyS0 respawn through it. The NIC drivers needed no new option: `R8169` and
`E1000` were already `=y` from defconfig, so the fragment pins them; the only new
kernel options are the zstd firmware-loader ones. The rootfs
configures `lo` and `eth0` with DHCP in `/etc/network/interfaces`; `bmx-net` runs
`ifup -a` in a background subshell and prints `bmx: network up <addr>` (or
`network down` after ten seconds) on COM1, so a missing cable cannot hold the
console. The gate attaches `-nic user,model=e1000` on every path and asserts
`bmx: network ready` plus `bmx: bmx-shell ready`, `bmx: bmx-net ready` and
`bmx: network firmware ready`. The BIOS path waits for `bmx:~#` and
`bmx: network up` before stopping, so the console home and the DHCP lease are
gate assertions, not only manual checks.

The optional `rtl_nic` firmware turned out to matter: r8169 calls
`rtl_request_firmware` from `rtl_open`, not from probe, so the file can load after
the root is mounted when `ifup` opens the interface. The rootfs now carries
`linux-firmware-rtl_nic`, the fragment enables `FW_LOADER_COMPRESS_ZSTD` because
Alpine ships the file as `.fw.zst`, and `bmx-check` proves
`rtl8168h-2.fw.zst` is in the image.

L12: "Perfecto. Me gustaría que eth1 también estuviera implementado y funcionando
por defecto, como eth0"

The board has two RTL8111H NICs, so `/etc/network/interfaces` now configures
`eth1` with DHCP exactly like `eth0`. `bmx-net` reports each lease as
`bmx: network up <iface> <addr>`, prints `bmx: network online` only when both are
up, and names the interface in the down line after twenty seconds. Every QEMU
path attaches a second `e1000` on 10.0.3.0/24 (the first stays on QEMU's default
10.0.2.0/24), so the BIOS gate waits for both leases and the summary, and every
path asserts `bmx: network eth0 ready` and `bmx: network eth1 ready`.

L13: "Perfecto. Un pequeño cambio, y otro grande: el pequeño es que no indique en
pantalla siempre que una interfaz de red se desconecte o se conecte. El grande es
que me gustaría crear un instalador para este SO, con su imagen ISO y capacidad
de elegir distribución de teclado y disco duro de destino (con opciones de
particionado por defecto y capacidad de configurarlo manualmente según quiera el
usuario), y un botón inicial de por defecto para que la instalación sea de un
botón si así lo quiere el usuario, con un contador de 20 segundos como un trigger
que haga lo mismo que el botón de por defecto si no se pulsa ninguna tecla"

The small change is in: the inittab now runs `dmesg -n 5` after OpenRC default and
before the network helpers, so console_loglevel is 5. Warnings and errors still
print; KERN_INFO, which carries the NIC link up/down chatter, stays in the kernel
ring buffer (`dmesg`). The q35 and UEFI paths stop at the OpenRC banner and the
BIOS path runs past it to the userspace prompt/online markers; none of the gate
assertions depend on KERN_INFO after the switch, and the BIOS path now also
asserts that no link line appears after the prompt. The installer is a separate
feature document.

L14: "Adelante, procede con ello. Otro cambio al sistema, además: Quiero que
cuando termine de arrancar, la salida de arranque se limpie."

`bmx-clear` runs as a `::wait:` action after `dmesg -n 5` and before the once
actions, and writes CSI 3J to `/dev/tty1`, so the VT screen and scrollback are
empty when the shell prompt appears. `dmesg -n 5` moved from `::once:` to
`::wait:` at the same time, which removes the scheduling race the review flagged.
COM1 keeps the boot log. The gate asserts that `bmx-clear` parses.

The first manual run exposed a pre-existing gap: nothing applied `/etc/hostname`,
so the system was `(none)`, the prompt read `(none):/#` (busybox init leaves HOME
unset), and ifupdown-ng used `(none)` as the DHCP hostname, which made its
executor run `eval echo (none)` and print syntax errors (the lease still worked).
The inittab now runs `hostname -F /etc/hostname` before OpenRC; the prompt is
`bmx:~#` and the eval noise is gone.
