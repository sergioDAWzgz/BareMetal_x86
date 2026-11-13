# Installer ISO

Feature: a bootable installer ISO that installs this system onto a chosen disk,
with keyboard layout selection, default or manual partitioning, and a one-button
default install behind a 10 second countdown. Branch: `main`. Delivery:
work-unit commits. Gate: `./test.sh` plus an install-and-boot QEMU check.

## Specs

- S1: the installer is a UEFI-bootable ISO (El Torito) carrying the same Linux
  `bzImage` with an installer initramfs; the board boots it from a USB stick.
- S2: the installer offers a keyboard layout (default `es`), a destination disk,
  and a partitioning mode.
- S3: the default mode is one button: it installs to the first internal disk with
  the default layout and `es`; if no key is pressed within 10 seconds, the same
  action starts automatically from a countdown screen drawn in the TUI's own
  dialog theme (blue screen, white panel, blue title), with only the number line
  rewritten in place.
- S4: the default layout is the current contract: MBR signature generated per
  install, stage 1 at LBA 0, stage 2 at LBA 1, the kernel at LBA 17, a FAT32 ESP
  from LBA 2048 and an ext4 root filling the rest.
- S5: manual partitioning allows up to four primary MBR partitions (cfdisk or
  parted). The installer validates that one is a FAT32 ESP and one is the ext4
  root, that the first partition starts after LBA 144, and then patches the
  compiled `root=PARTUUID=<disk>-<part>` string in the kernel binary (both the
  LBA 17 copy and the ESP copy) and writes the boot chain. GPT is out of scope.
- S6: the installer writes the boot chain, the FAT32 ESP (`BOOTX64.EFI`,
  `KERNEL.BIN`, `LINUX.BIN`) and the ext4 root from the payload, and the
  installed disk must boot on the existing BIOS, q35 and UEFI gate.
- S7: the chosen keymap is loaded for the installer console and stored in the
  installed rootfs as a binary `/etc/keymap`; the target rootfs carries
  `kbd-bkeymaps` and the inittab loads it at boot. The default is `es`.
- S8: the installer needs no network; the payload travels on the ISO.
- S9: the destructive write starts only from the explicit default action, the
  manual confirmation, or the 10 second countdown that stands in for the default
  action.
- S10: the default action asks no further questions when the target disk already
  holds data: the old signatures are wiped, the MBR is rewritten and both
  filesystems are formatted with the tools' force flags.

## Tasks

- T1 installer kernel: the main kernel already has EFI_STUB, USB_STORAGE,
  ISO9660 and VFAT built in, so a second out-of-tree build only adds
  CONFIG_CMDLINE_BOOL/CMDLINE and an embedded initramfs - done
- T2 installer initramfs and TUI - done
- T3 payload and ISO assembly - done
- T4 install engine: default and manual MBR layouts - done (manual path
  implemented, not exercised end to end)
- T5 QEMU install-and-boot check - done
- T6 docs - done
- T7 exercise the manual cfdisk path and the USB boot on the board - pending
- T8 dialog-theme countdown and unconditional default overwrite - inline - done (this commit)

## Log

L1: "Perfecto. Un pequeño cambio, y otro grande: el pequeño es que no indique en
pantalla siempre que una interfaz de red se desconecte o se conecte. El grande es
que me gustaría crear un instalador para este SO, con su imagen ISO y capacidad
de elegir distribución de teclado y disco duro de destino (con opciones de
particionado por defecto y capacidad de configurarlo manualmente según quiera el
usuario), y un botón inicial de por defecto para que la instalación sea de un
botón si así lo quiere el usuario, con un contador de 20 segundos como un trigger
que haga lo mismo que el botón de por defecto si no se pulsa ninguna tecla"

L2: "Casi se me olvida: en la instalación por defecto, la distribución de teclado
debe ser española" - the default layout is `es`.

L3: Open decision: the manual mode needs a scope. Fixed two-partition layout
(chosen ESP and root sizes, root stays partition 2) keeps the current boot chain.
Arbitrary MBR layouts require the installer to patch the compiled
`root=PARTUUID=...` command line in the kernel and to validate the ESP/root
discovery rules.

L4: The user chose the full MBR manual scope. The installer will accept up to four
primary partitions, require a FAT32 ESP and an ext4 root, keep the first 144
sectors free for the boot chain, patch the disk signature and partition number in
the kernel command line, and write stage 1/stage 2/kernel at LBA 0/1/17. GPT,
logical partitions and more than four primaries stay out of scope.

L5: Spike decisions. The main kernel already builds EFI_STUB, USB_STORAGE,
ISO9660 and VFAT in, so the installer kernel is a second out-of-tree build that
only adds `CONFIG_CMDLINE_BOOL`, `CONFIG_CMDLINE` and an embedded initramfs
(`CONFIG_INITRAMFS_SOURCE`); the bzImage is the El Torito `BOOTX64.EFI`. The host
has no xorriso, so the build extracts the Ubuntu jammy debs into
`~/.cache/bmx/iso-tools` and runs it with `LD_LIBRARY_PATH`; the ISO is
`xorriso -as mkisofs -e efi.img -no-emul-boot -eltorito-platform efi
-isohybrid-gpt-basdat`. The initramfs init mounts proc/sys/dev, scans block
devices for the `BMXINSTALL` label, mounts the ISO and runs the installer.

L6: The spike boots in QEMU/OVMF: the installer kernel starts from the El Torito
ISO, the initramfs finds `/dev/sr0` by the `BMXINSTALL` label, mounts it, reads
the payload and reaches a shell. Two fixes were needed: `/dev/console` must be a
static node in the initramfs (devtmpfs is not mounted before init), added as a
512-byte aligned devnodes cpio before the rootfs archive, and the El Torito boot
image must live inside the ISO tree for xorriso. The interactive shell warns
"can't access tty" because init does not open a controlling tty; the real `/init`
will exec the TUI directly.

L7: The full installer is in `installer/`. `build-iso.sh` builds the installer
rootfs (e2fsprogs, dosfstools, util-linux, cfdisk, dialog, kbd-bkeymaps,
ncurses-terminfo-base), packs it with the scripts through `gen_init_cpio`, builds
the second kernel with the embedded initramfs, and writes
`build/installer/bmx-installer.iso` (~214 MiB). `test-iso.sh` installed into a
blank 2 GiB QEMU disk: the default 10 s path chose `/dev/sda`, wrote a 64 MiB ESP
at LBA 2048 (type 0x0c) and an ext4 root at LBA 133120 with size 4061184 sectors
(the rest of the disk), patched the PARTUUID to `75a022da-02`, and the installed
disk booted through OVMF to OpenRC (`VFS: Mounted root ... 8:2`). The chosen
keymap is written to `/etc/keymap` and loaded by `bmx-keymap` in the installed
rootfs. The manual `cfdisk` path is implemented but not yet exercised end to
end.

L8: The first USB boot on hardware failed the same way it did in QEMU: the
initramfs init scanned block devices once, immediately after mounting devtmpfs,
and the USB stick enumerated a second later, so it printed "ISO BMXINSTALL not
found" and dropped to a shell without a tty. The init now retries the scan for
up to 30 seconds and keeps PID 1 alive with a shell if the TUI exits.
`installer/test-iso.sh` gained `BMX_TEST_MEDIA=usb`, and the end-to-end USB run
found the stick on `/dev/sdb`, installed to the SATA disk `/dev/sda` and booted
the result through OVMF to OpenRC. The test also removes the target image before
truncating it, so "blank disk" is true across runs.

L9: The first hardware attempt with Rufus in ISO Image mode failed with
"ISOFS: Unable to identify CD-ROM format". Rufus extracts the ISO tree to a
FAT32 volume and copies the BMXINSTALL label, so the init found the stick but
mounted it as iso9660. The init now reads TYPE from blkid and mounts vfat or
iso9660 accordingly. `BMX_TEST_MEDIA=fat` reproduces the extracted-FAT layout:
the test mounted `/dev/sdb (vfat)`, installed to `/dev/sda` and booted the
installed disk to OpenRC. Writing the ISO in Rufus DD mode keeps the ISO9660
filesystem and also works.

L10: "Perfecto. Ahora, un cambio: el instalador debería esperar 10 segundos en
vez de 20 para cargar la configuración por defecto. También debería ser una
pantalla del mismo estilo que el menú de configuración manual, en vez de ser una
línea de comandos. Al terminar la instalación, el indicador de instalación
exitosa está en español de Argentina, cuando debería usar español de España."

The countdown is now 10 seconds and is drawn as a `dialog --infobox` box, one
second per iteration, while a non-canonical `dd` on tty0 cancels it on any key.
The TUI copy uses Spain Spanish and the success box reads "Instalación
completada. Retira el medio de instalación y reinicia."

L11: "Además, si puedes, haz que el contador TUI no haga parpadear toda la
pantalla cada vez que cambie el número."

The countdown no longer uses `dialog` (which repaints the whole box every
second). It draws one bordered box with absolute cursor positioning and rewrites
only the countdown line in place; the box is centred from `stty size` and fits
the 80-column installer console.

L12: "Ok. Una serie de cambios: el contador sigue un estilo plano, no sigue el
diseño de la interfaz TUI del resto del instalador; me gustaría que ese fuera el
caso. Segundo, si el disco de destino ya tiene datos, y cargas la instalación por
defecto, o el contador llega a cero, no debería exigir confirmación adicional,
sino que debe proceder y hacer override a todo lo que haya dentro del disco."

The countdown now copies dialog's compiled-in theme instead of the terminal
default: `\033[44m` clear for the blue screen, a white panel (`47`) with black
body text (`30`), a white frame and a blue title (`34;1`) in the top rule, plus a
black shadow. The frame uses Unicode box glyphs: the console does not apply the
`ESC(0` graphics set that dialog's ncurses emits, and ncurses itself draws ACS as
UTF-8 on this terminal. It still paints once and rewrites only the countdown
line, so the style change does not bring back the flicker. `dialog` itself
cannot draw this screen: it works on the alternate screen, so an infobox is gone
by the time the countdown resumes.

The one-button path also stops depending on a blank disk. `write_mbr_default`
runs `wipefs -a` and then `sfdisk --force --wipe always --wipe-partitions
always`, `mkfs.fat` gets `-I` and `mkfs.ext4` gets `-F`, so a previous
installation, including its filesystem signatures, is overwritten without any
question. `installer/test-iso.sh` gains `BMX_TEST_KEEP=1` to install over the
target it installed on the previous run.

Verified after the change: `./test.sh` 3 PASS / 0 FAIL on the three paths;
`installer/test-iso.sh` completes from a blank 2 GiB disk and from the disk it
installed on the previous run (`BMX_TEST_KEEP=1`), and both boots reach OpenRC
and the banner. QEMU screendumps of the ISO show the countdown box in the same
blue/white/blue-title theme as the dialog menu, with the number changing from 7
to 5 in place, and the menu opens on any key.
