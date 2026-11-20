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
- S11: the console uses Debian's font family and size, Terminus 8x16
  (`ter-116n.psf.gz`), loaded by `bmx-console-font` on tty1.
- S12: the rootfs carries the Alpine `ca-certificates` bundle at
  `/etc/ssl/certs/ca-certificates.crt`.
- S13: `reboot` and `poweroff` show OpenRC's shutdown and the kernel shutdown
  messages, with their timestamps, on tty1.
- S14: the clock is corrected at boot: after DHCP, `bmx-clock` syncs over NTP,
  writes the time back to the RTC, and falls back to `/etc/bmx-build-date` while
  the clock is implausible, so TLS can validate certificates on a board with a
  dead RTC battery.
- S15: `reboot`, `poweroff` and `shutdown` clear the local screen as soon as the
  request is made, and Ctrl+Alt+Del runs the same reboot wrapper before OpenRC's
  shutdown log and the kernel messages fill the console.

## Tasks

- T1 build the cached Alpine rootfs and the ext4 image - inline - done (1c92cd9)
- T2 add the disk signature and partition 2 - inline - done (1c92cd9)
- T3 boot the root without an initramfs - inline - done (1c92cd9)
- T4 update the gate assertions and timing - inline - done (1c92cd9)
- T5 update README and roadmap - inline - done (1c92cd9)
- T6 local console and instant boot - inline - done (8dd6bc8)
- T7 console home and network - inline - done (00c8ea3)
- T8 second NIC and firmware - inline - done (this commit)
- T9 Debian-sized font, TLS bundle, visible shutdown - inline - done (this commit)
- T10 NTP clock and build-date fallback - inline - done (this commit)
- T11 clear the screen at the start of shutdown - inline - done (this commit)

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

L15: "Otra cosa que me gustaría añadir al sistema: al arrancar, debería mostrar
el nombre del sistema operativo y la versión del kernel de Linux en ejecución.
Por último, la terminal debería usar líneas con colores de resalto importantes,
como ocurre en Debian. Quizá haya que reemplazar SH por Bash: no pasa nada por
hacerlo"

The target rootfs now installs bash and `bmx-shell` runs `/bin/bash -l`.
`/etc/profile.d/bmx-color.sh` sets a green `root@host:~#` prompt plus `ls` and
`grep` colour. `bmx-banner` runs after `bmx-clear` and prints `BareMetal_x86`
and the running `uname -r` on tty1 (coloured) and COM1 (plain). The gate asserts
the `bmx: bmx-banner ready` marker and the `Kernel: 6.12.` banner line.

L16: "Introduce las mejoras que has dicho. Además, si puedes, haz que el contador
TUI no haga parpadear toda la pantalla cada vez que cambie el número. Me
gustaría, también, tocar el espacio que hay entre el nombre de la distribución,
la versión del kernel de Linux y el prompt para que sea parecido al de Debian.
Modifica la fuente y el tamaño de esta para que sea como la de Debian también."

The target rootfs adds GNU grep, because the BusyBox applet accepted and ignored
`--color`, and the Terminus console fonts. `bmx-console-font` sets `ter-132n`
(16x32) on tty1 before `bmx-clear`, so the monitor uses the Debian console font
family at a larger size. The banner now leaves a blank line before the prompt.
The q35 and UEFI gate paths wait for the banner instead of the OpenRC line, so
`Kernel: 6.12.` is asserted on all three paths. The installer countdown is a
hand-drawn box that rewrites only its number line (installer log L11).

L17: "Tercero, las fuentes son muy grandes: las querría del tamaño de las de
Debian. Cuarto, debe incluir certificados TLS de serie. Quinto, en el apagado y
reinicio ha de mostrar toda la salida de apagado por la pantalla, con los
segundos (igual que pasa con el arranque)"

`bmx-console-font` now loads `ter-116n` (Terminus 8x16), the family and cell size
Debian's console-setup uses, instead of `ter-132n` (16x32). `build.sh` adds
`ca-certificates` and fails the build if `/etc/ssl/certs/ca-certificates.crt` or
the font file is missing, because neither is visible to the serial gate. The
kernel fragment adds `CONFIG_FB_EFI` and `CONFIG_DRM_BOCHS`, so the UEFI
installer ISO has a console on the firmware's framebuffer and QEMU's stdvga has
one too; that is what made the countdown and shutdown screenshots possible. The
new `bmx-shutdown` inittab action raises the kernel console log level and runs
OpenRC's shutdown on tty1; `/usr/local/sbin/reboot` and `/usr/local/sbin/poweroff`
do the same before signalling init, so the monitor shows the stopping services
and the kernel messages with their `[ seconds ]`. `bmx-check` reports
`bmx: tls certificates ready`, `bmx: bmx-shutdown ready`, `bmx: reboot ready` and
`bmx: poweroff ready`, and the gate asserts all four. A controlled
`echo '<0>BMX-KERNEL-CONSOLE-TEST' > /dev/kmsg` proved the kernel console paints
tty1; the shutdown messages appear there until the video driver's shutdown
handler releases the display, exactly as on a Debian console.

L18: "Tercero, los certificados siguen fallando"

The failure was reproduced in QEMU with the installed disk: `wget` and `apk`
succeed against dl-cdn.alpinelinux.org with the correct clock, and fail with
"certificate verify failed" / "TLS: server certificate not trusted" after
`date -s '2000-01-01'`. A dead RTC battery is the cause, so `bmx-clock` runs once
after `bmx-net`, waits up to 20 s for a lease, tries `ntpd -q` against two pool
hosts with a 15 s timeout, writes the time back with `hwclock -w`, and when the
clock is still before 2025 sets it from `/etc/bmx-build-date` (written by
build.sh). It reports `bmx: clock synced`, `clock plausible`, `clock set to build
date` or `clock not set (TLS may fail)` on COM1. `build.sh` also checks
`/etc/ssl/cert.pem` (the OpenSSL default path, a symlink in the Alpine bundle)
and `/etc/profile.d/bmx-tls.sh` exports `SSL_CERT_FILE` and `SSL_CERT_DIR`.

Verified on the installed disk in QEMU: with the clock at 2000, `wget` fails with
"certificate verify failed" and `apk` with "TLS: server certificate not trusted";
after `bmx-clock` (NTP reachable through slirp) the clock reads 2026 and both
clients succeed, and setting the clock to `/etc/bmx-build-date` with `date -s`
makes wget succeed as well, which is the fallback branch.

L19: "cuando reinicie el equipo, debería limpiarse la pantalla nada más pulsarse
Ctrl+Alt+Supr, o escribir el comando correspondiente"

`/usr/local/sbin/reboot`, `poweroff` and a new `shutdown` wrapper set the console
log level, clear tty1 (`ESC[H ESC[2J ESC[3J`) and then print their one-line
message before `exec`ing the BusyBox applet. The inittab Ctrl+Alt+Del action now
runs `/usr/local/sbin/reboot` instead of `/sbin/reboot`, so the key chord takes the
cleared-screen path too. `bmx-check` reports `bmx: shutdown ready` as well.

Verified with QEMU screendumps on the installed disk: before the request the
console showed the banner and the prompt; 0.5 s after `reboot` on the serial
shell and after `sendkey ctrl-alt-delete`, tty1 held only "Reiniciando el
sistema..." and COM1 logged "The system is going down NOW!".

L20: "Procede con los tres."

The SMBus/i2c question is closed as not needed. The ALC888S is an HDA codec and
phase 5b drives it through `snd-hda-intel` on the HDA link; SMBus is not part of
that path, so no `i2c` work is pending for it. The built config has
`CONFIG_SND_HDA_INTEL=y` but every codec parser off, including
`CONFIG_SND_HDA_CODEC_REALTEK`; that parser, not i2c, is the missing piece if
audio is ever in scope, and audio is outside the current phases.

The 1080p60 re-check is the one remaining item that needs the board: QEMU has no
i965 and no VA-API, so the dropped-frame count can only be measured on the N2807
with `bmx-play` and `--profile=fast`. Its first board run dropped 177 of 600
frames; the display/render path, not the decode, is the limit.

L21: "Añade también soporte para audio."

The ALC888S is a Realtek HDA codec on the Intel HDA controller, so the support is
the Linux codec stack, not SMBus. The kernel fragment pins `SND_HDA_INTEL`, the
Realtek parser for the board, the analog parser for generic codecs (QEMU's
hda-duplex in the gate) and the HDMI parser for i915 audio. `alsa-utils` adds
`aplay`, `amixer` and `speaker-test`; `bmx-check` reports `audio core ready`,
`audio tools ready` and `audio card ready`.

A real defect surfaced while wiring the marker: `bmx-check` runs before OpenRC,
and `/proc` was not mounted yet, so `/proc/asound` did not exist. The inittab now
mounts proc next to sysfs before the check.

Verified in QEMU on the three gate paths with `-device intel-hda -device
hda-duplex`: `snd_hda_codec_generic` autoconfigures the codec, card #0 is HDA
Intel, and all three audio markers appear; the gate asserts them. The Realtek
parser and `aplay -l` on the ALC888S need the board.

Late review advisories applied on top: the gate now pins QEMU's audio backend
(`-audiodev none,id=bmxaudio`) so it never probes the host, `bmx-check` also runs
`aplay -l` (`audio pcm ready`) to prove the libasound/dev/snd path, and `build.sh`
fails if Kconfig drops `SND_HDA_INTEL`, `SND_HDA_CODEC_REALTEK`,
`SND_HDA_CODEC_ANALOG` or `SND_HDA_CODEC_HDMI`. The README now presents `aplay -l`
on the board as the pending board check, not as an observed result.
`bmx-audio` reports `audio mixer ready` only when at least one control was set,
and the gate asserts the `bmx-audio`/`bmx-rtsp` readiness lines.

L22: "El rendimiento en la prueba bmx-play es bueno. Sin embargo, rechaza muchos
fotogramas cuando ejecuto la prueba de RTSP del vídeo de Big Buck Bunny en 1080p
con el codec H264. No se llega a ver fluido nunca, sino con continuos cortes e
interrupciones. El aplay -l muestra el dispositivo, pero no reproduce audio,
supongo que por el RTSP. El cfdisk funciona"

Board report: the local 1080p60 path is smooth now, cfdisk works, `aplay -l` lists
the ALC888S but playback is silent, and the 1080p RTSP stream drops frames. Two
new helpers:

- `bmx-audio` (inittab wait action before the banner) unmutes Master, PCM,
  Headphone, Front and Speaker at 80 % and prints `bmx: audio mixer ready` only
  when at least one control was actually set (`audio mixer missing` otherwise, so
  the gate assertion can fail); the card was already probed because proc is
  mounted before bmx-check.
- `bmx-rtsp` plays `rtsp://` URLs with VA-API and KMS/DRM, `--ao=alsa` so the
  audio clock times the video, `--demuxer-readahead-secs=10` and
  `--demuxer-max-bytes=256MiB` instead of mpv's 1 s default;
  `BMX_RTSP_TRANSPORT=udp` switches transport. The socket buffer is left to the
  kernel: FFmpeg applies its `buffer_size` only to the UDP handles, and Linux
  clamps `SO_RCVBUF` to `net.core.rmem_max` (208 KiB by default), so the earlier
  4 MiB request and the `rmem_max` raise were removed rather than documented as
  a fix they did not deliver. L23 supersedes this socket-buffer decision: with
  UDP as the default, the 4 MiB request and the session-scoped `rmem_max` raise
  do take effect.

`bmx-check` reports both scripts and includes amixer in `audio tools ready`. QEMU
cannot play the RTSP stream (no i965/VA-API) or produce sound (no backend), so the
scripts are verified for syntax only; smoothness and audible output remain board
checks. The roadmap closes the 1080p60 and cfdisk board items from this report.

L23: "Perfecto. Ahora necesito dos cambios: que bmx-rtsp lea por defecto los
paquetes UDP en vez de ser TCP la opción por defecto, y que termines de mejorar
la transmisión por RTSP: sigue teniendo fallos de transmisión de imagen, aunque
el audio funciona sin problemas"

Board report: the analog output is audible now, so the board-audio roadmap item
closes. The RTSP video still fails while the audio runs clean. `bmx-rtsp` now
defaults to RTP/UDP (`BMX_RTSP_TRANSPORT=tcp` keeps the interleaved path, and
other FFmpeg transports such as `udp_multicast` or `http` pass through
unchanged), asks for a 4 MiB RTP socket buffer and raises `net.core.rmem_max`
for the playback only, restores it on a normal exit, INT, TERM or HUP and warns
when procfs is read-only; a SIGKILL cannot be caught, so the raised ceiling then
lasts until the next reboot. Video is paced by the display
(`--video-sync=display-resample`, `BMX_RTSP_SYNC=audio` reverts) because with
the audio clock as master a jittery ALSA device can drop video frames while the
sound stays clean. The player primes three seconds of cache before the first
frame (the same value is the rebuffer threshold after an underrun) and keeps the
10 s readahead and the 256 MiB cap. `BMX_RTSP_STATS=1` prints the cache duration
and the decoder/output drop counters on the serial console. UDP can still lose
packets on a congested link, so `BMX_RTSP_TRANSPORT=tcp` remains the lossless
transport when a client requires artifact-free video.

QEMU still cannot play the stream (no i965/VA-API), so the gate proves the
script parses and that the shipped helper still carries the UDP default and the
4 MiB buffer (`bmx: rtsp udp default ready`); it never runs mpv, so option
acceptance and smoothness over UDP remain board checks.

L24: "Sólo una cosa más: cuando haya un stream reproduciendo un vídeo en bucle,
no debería cerrar el stream cuando termine el vídeo, sino que debe mantener la
conexión, y reproducir el vídeo otra vez si procede. Sobre los artifacts, al
final forzando TCP en VLC los ha eliminado"

Board report: TCP forced in VLC removes the artifacts, so the remaining image
faults were UDP packet loss. L24 supersedes the L23 transport default:
`bmx-rtsp` now defaults to RTSP/TCP (`BMX_RTSP_TRANSPORT=udp` keeps the
low-latency path with its 4 MiB buffer and the session-scoped `rmem_max` raise)
and adds `--loop-file=inf`: at EOF mpv seeks back to the start, which keeps the
RTSP session when the server allows seeking and no longer exits. A live camera
that never ends sees no change. The gate marker now asserts the TCP default, the
loop and the UDP buffer together (`bmx: rtsp defaults ready`); playback and loop
stability remain board checks.

L25: "Un cambio más: he visto que con las opciones --profile=fast y --hwdec=auto
no hay pérdida de frames. ¿Podrías hacer que estuvieran por defecto en los
streams, en el comando, sin tener que añadirlos específicamente?"

The board found that `--profile=fast` with `--hwdec=auto` loses no frames.
`bmx-rtsp` already carried `--profile=fast`; it now uses `--hwdec=auto` instead
of `--hwdec=vaapi` plus `--hwdec-software-fallback=no`, so mpv picks the VA-API
path and keeps the three-frame software fallback available. The gate marker also
pins the fast profile and the auto hwdec (`bmx: rtsp defaults ready`).

L26: "Dos problemas: el stream no se reinicia por la falta del parámetro
--force-seekable=true; debería venir incluido y cargar automáticamente. Luego,
bmx-rtsp ya no funciona como antes: sería mejor reestablecer los parámetros de
transporte que pusiste en la anterior versión, con UDP por defecto, creo
recordar"

Board report: the loop needed `--force-seekable=yes`, which is now part of the
helper; `--loop-file` can seek inside the cache even when the demuxer calls the
stream unseekable. The transport default returns to RTP/UDP with the 4 MiB
socket buffer and the session-scoped `rmem_max` raise; `BMX_RTSP_TRANSPORT=tcp`
remains the lossless path for clients that cannot accept packet loss. The gate
marker checks the UDP default, the loop, force-seekable, the fast profile, auto
hwdec and the buffer together.

L27: "Error 405 when trying to loop back in the player"

The server rejects the RTSP seek used by `--loop-file`/`--force-seekable` with
405 Method Not Allowed. `bmx-rtsp` now loops with `--loop-playlist=inf`, which
reloads the URL at EOF and reopens the session; keeping the original session is
not possible when the server refuses the seek. A server that loops without
ending the stream never triggers the reload. The gate marker checks the playlist
loop instead of `--loop-file`/`--force-seekable`.

L28: "Dos cosas: ¿se puede hacer algo más para reducir al absoluto mínimo los
paquetes perdidos por UDP? ¿se puede reproducir el stream de tal manera que el
cliente siempre reproduzca el timestamp correcto, en vez de quedarse guardado
en caché con el actual método de loop? Es decir, si el vídeo en VLC se cambia
al minuto 01:17, incluido repetido, que la reproducción se mueva a ese minuto.
Todo con el mínimo retardo de la reproducción posible"

The demuxer cache is now one second (`BMX_RTSP_CACHE_SECS` raises it), with no
initial pause, so playback follows the server's current position within about a
second and a source seek or loop is not delayed by a large local backlog. The
4 MiB RTP socket buffer and the session-scoped `rmem_max` raise stay as the UDP
burst-absorbing layer; a larger `BMX_RTSP_CACHE_SECS` trades latency for
resilience on a lossy link, and TCP remains the only lossless transport. The
gate marker also pins the one-second cache default.

L29: "O mejor dicho, reducir los cortes visibles al mínimo posible"

The cache default moves from one to two seconds and the rebuffer wait stays at
one second: the larger cache absorbs more network jitter before it becomes a
visible freeze, while a source seek or loop is still followed within about two
seconds. Lower it to 1 for the lowest latency or raise it to 3-5 for a lossy
link; TCP remains the lossless path for packet-loss artifacts. The loop reload
still has an inherent reconnect gap when the server rejects the seek with 405.

L30: "BMX_RTSP_STATS=1 no imprime los datos que dices"

The stats mode was silent because `--msg-level=all=warn` suppresses mpv's
terminal status line; the status level sits above info. `BMX_RTSP_STATS=1` now
also sets `--msg-level=all=status`, so the cache/drop line prints while normal
playback stays quiet at warn.

L31: board stats with `BMX_RTSP_SYNC=audio`: "cache=2s dec-drops=0 out-drops=0
mistimed=(unavailable) fps=24"

The A/B settled it: display-resample produced 12 output drops and 15 mistimed
frames on the 24p stream; the audio clock produced none (mistimed is only
reported by the display-sync modes, so it is unavailable in audio mode). The
default sync is now audio; `BMX_RTSP_SYNC=display-resample` restores the old
mode. The 24p stream on a 60 Hz output still has 3:2 cadence, which no drop
counter shows; a native 24 Hz mode is the next step if the panel supports it.

L32: "Vale, me he dado cuenta de que activando la transcodificación en VLC, los
artefactos en la transmisión desaparecen por completo, aunque uses UDP"

Board report: server-side transcoding removes the artifacts over UDP, so the
source stream was the cause: its bursts exceeded what the socket/NIC could
absorb, and transcoding normalizes them. The client-side buffer, two-second
cache, audio clock and playlist loop are not at fault; TCP remains the lossless
client transport and transcoding the source-side fix. The remaining 24p
cadence and the loop reload gap are separate.
