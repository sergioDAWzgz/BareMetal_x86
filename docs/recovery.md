# Recovery

The bare-metal kernel drives AHCI and the serial port, but it has no USB or
network stack. If the image on the disk is wrong, the board cannot download a new
one. Recovery therefore means rewriting the disk from another machine, or
repairing one of its filesystems from a host that can mount it.

Before Linux is up, COM1 is the only channel that reports a failure. Once Alpine
starts, the board's own monitor shows the console too, so a screen with the tty1
root shell means the image and the root filesystem are healthy; a black screen
with COM1 still printing points at i915 or the panel.

## Keep a known-good image

Before flashing a new build, keep the previous `build/disk.img` and its `sha256`.
The loader, kernel and root filesystem are written at fixed offsets; a bad image is
replaced, never repaired in place.

## If the board does not boot

1. Connect the external COM1 with the **null-modem** cable at 115200 8N1 and
   capture the log. Before Linux is up this is the only channel that reports a
   failure.
2. If `stage1:`, `stage2:` or `uefi:` lines appear, the firmware is executing the
   disk and the failure is later: the Linux payload, the root filesystem or the
   display path. The loader's last `linux:` line says how far it got.
3. If nothing appears, the firmware is not booting the disk. Check the boot order,
   UEFI versus legacy/CSM, SATA mode (AHCI), and that the disk is detected. Load
   the BIOS defaults if the settings were changed.
4. Power off, remove the SATA or mSATA disk and connect it to another Linux machine
   through a USB-SATA adapter. The image is raw, not an ISO:

   ```sh
   sha256sum known-good.img
   sudo dd if=known-good.img of=/dev/sdX bs=1M conv=fsync
   ```

   Re-check the digest and put the disk back.

## Repairing one part

- **Payload only.** The FAT32 volume starts at LBA 2048 (offset 1 MiB). From any
  OS with mtools, replace `KERNEL.BIN` or `LINUX.BIN` without rewriting the whole
  disk:

  ```sh
  mcopy -o -i disk.img@@1048576 new-kernel.bin ::/EFI/BOOT/KERNEL.BIN
  ```

- **Root filesystem only.** The ext4 partition starts at LBA 133120 (64 MiB after
  the FAT32 volume). Mount it read-write on a Linux host, repair it, and unmount
  cleanly.
- **Boot code only.** The MBR at LBA 0 and stage 2 at LBA 1-16 come from
  `build/stage1.bin` and `build/stage2.bin`; a full reflash is simpler than patching
  them by hand.

## What the board cannot do

The Linux payload has USB and network drivers once its root filesystem is up, but
the bare-metal kernel does not. If the root filesystem fails to mount, there is no
shell, no network and no USB: the disk has to be rewritten from another machine.
Keep the null-modem cable and a known-good image with the board.
