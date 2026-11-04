# Phase 4 - storage and userspace

Bare-metal x86 kernel on the Jetway JNF3A (Intel Bay Trail N2807/N2930). This
document is the specification every task reads. Objective: prove the machine can
find its SATA controller, read a real FAT32 volume, load an ELF64 executable from
it and run that program in ring 3, printing through a syscall.

Branch: `main`. Test runner: `./test.sh` (BIOS, q35 and UEFI paths, QEMU).
Delivery strategy: work-unit commits on `main`, one reviewable slice per task,
`ask-on-risk` for any pull-request chaining.

## Specs

- **S1** - AHCI driver, read sectors from the SATA port.
  The kernel finds the SATA controller through PCI (class `01`, subclass `06`,
  programming interface `01`), initialises the port that carries an attached
  drive, and reads sectors through a command list, FIS receive area and one PRDT
  entry per transfer. A read must fail loudly (error status printed, non-zero
  return) instead of returning stale buffer contents. Observable:
  `ahci: abar 0x... ports N`, `ahci: port N attached`, and a real sector read.
- **S2** - read-only FAT32 and a ramdisk.
  The kernel mounts a FAT32 volume read-only: it parses the BPB, refuses anything
  that is not a consistent FAT32 volume, walks the FAT chain and reads a file by
  its 8.3 name from the root directory. It never writes. A file is staged in RAM
  (the ramdisk) before being consumed, with every size and cluster bound checked.
  Observable: `fat32: volume ... sectors ...`, `fat32: USER.ELF ... bytes`,
  `ramdisk: staged USER.ELF ... bytes`.
- **S3** - ELF loader and a ring-3 process with syscalls.
  The kernel loads an ELF64 executable's `PT_LOAD` segments into user pages at
  `0x0000008000000000`, rejects a bad magic, class, machine, type, segment type,
  out-of-range virtual address or a segment that does not fit the file, builds
  the page tables for it, and enters ring 3 on a user stack. Kernel memory stays
  supervisor-only: every table level for the user range carries `U/S = 1` and no
  other mapping in the tree does. Syscalls arrive on `int 0x80` with a validated
  user buffer.
- **S4** - minimal libc.
  A user program links against `write`, `puts`, `strlen`, `memset`, `memcpy` and
  `exit` on top of the syscall interface, with a `crt0` that calls `main` and
  exits with its return value.
- **S5** - gate.
  `test.sh` asserts the phase outcome on the BIOS path, the q35 path and the UEFI
  path: `Phase 4 complete`, the AHCI controller and port, the mounted FAT32
  volume, the staged file, the loaded ELF entry point, a syscall observed from
  `cs = 0x1b` (ring 3), the user program's own line of output and its exit
  status 0.
- **S6** - documentation.
  `README.md` and `docs/roadmap.md` describe what phase 4 added and how the
  storage and userspace path works, in English, like every other artifact.

Exact strings the gate asserts (they are the contract):

```
ahci: abar 0x<hex> ports <dec>
ahci: port <dec> attached
ahci: read <dec> bytes from port <dec> lba <dec>
fat32: volume BMXDATA sectors <dec> clusters <dec>
fat32: HELLO.TXT <dec> bytes
fat32: content read from the fat32 volume
fat32: USER.ELF <dec> bytes
ramdisk: staged USER.ELF <dec> bytes at 0x<hex>
elf: USER.ELF entry 0x<hex> segments <dec> pages <dec>
sys: first syscall from cs 0x1b
user: hello from ring 3
user: running in ring 3
user: exited with status 0
Phase 4 complete
```

## Tasks

- **T1** - AHCI. `kernel/ahci.c`, `kernel/ahci.h`, contiguous frame allocation in
  `kernel/memory.c`, the data disk on an `ich9-ahci` controller in `test.sh`, the
  kernel printing the controller and doing a real sector read. Covers S1, part of
  S5. Route: inline. Done in `96ec0a1`.
- **T2** - FAT32. `kernel/fat32.c`, `kernel/fat32.h`, `build.sh` building
  `build/data.img` with the volume label `BMXDATA`, the kernel mounting it and
  reading `HELLO.TXT`. Covers S2 (read side), S5. Route: inline. Done in
  `a91b86b`.
- **T3** - ramdisk, address space and ELF loader. `kernel/ramdisk.c`,
  `kernel/ramdisk.h`, `kernel/vmm.c`, `kernel/vmm.h`, `kernel/elf.c`,
  `kernel/elf.h`, plus the loader reading the staged file into user pages and
  reading one byte back through the new mapping. Covers S2 (ramdisk), S3 (loader
  half), S5. Route: inline.
- **T4** - ring 3 and syscalls. User selectors in `kernel/gdt.c`, `TSS.rsp0` and
  its kernel stack, the `int 0x80` gate in `kernel/idt.c`, `kernel/usermode.asm`,
  `kernel/syscall.c`, `user/` (crt0, libc, program, linker script) wired into
  `build.sh`, and the gate assertions. One unit because a ring-3 entry that never
  returns through a syscall proves nothing: the loader, the transition and the
  syscall contract are one behaviour. Covers S3, S4, S5. Route: inline.
- **T5** - documentation and final gate. `README.md`, `docs/roadmap.md`. Covers
  S6, S5. Route: inline.

## Log

### L1 (user, verbatim)

> Procede con la siguiente fase

### L2 (analysis, 2026-11-04)

Baseline confirmed green before any change: `./build.sh` then `./test.sh` printed
the three `PASS` lines (bios, q35, uefi). Kernel image 16688 bytes against the
64 KiB stage-2 limit.

Storage topology decision. The proven boot path stays untouched: the boot disk
keeps its IDE/ESP wiring. A second disk, `build/data.img`, is attached to an
`ich9-ahci` controller on every QEMU invocation (`-device ich9-ahci,id=sata` plus
`-device ide-hd,...,bus=sata.0`; on q35 the built-in AHCI takes it on `ide.1`).
The kernel therefore sees one AHCI controller on the `pc` machine and the q35
controller everywhere else, and q35 additionally exposes the raw boot disk on the
same controller. Positional port numbers are therefore not a stable contract, so
the volume is selected by content: a candidate must be a consistent FAT32 volume
carrying the label `BMXDATA`. That keeps AHCI ignorant of filesystems and
filesystems ignorant of ports.

User address space decision. Both loaders point `PML4[0]` and `PML4[256]` at the
same PDPT and share `PD0` with the kernel window's PDPT, so any `U/S` bit set on
a 2 MiB entry would open the identity map or the direct map to ring 3. User space
therefore gets a fresh `PML4[1]` subtree, which no loader touches, and the user
program links at `0x0000008000000000` with `-mcmodel=large`. The user range is
the only part of the tree with `U/S = 1`.

DMA buffer decision. The interrupt frame, the kernel image and every static
buffer live below 1 MiB, which `pmm_init` already keeps out of the free pool, but
multi-frame allocations must be physically contiguous for a PRDT and for the
direct map to see a staged file as one block. `pmm_alloc_frames(count)` adds that
run finder; the AHCI buffers, the staging area, the TSS stack and the user stack
all come from it instead of growing `.bss`.

Ordering decision. Phase 4 runs before the interactive keyboard echo loop so the
q35 and UEFI paths, which get no keystrokes, still reach the gate. `Phase 3
complete` keeps its position in the log.

### L3 (evidence, 2026-11-04)

T1 gate was weaker than it looked: `ahci: read 512 bytes from port N lba 0`
passed while the buffer held zeros, because the command header was built with
`header[0] = CFL` and `header[1] = PRDTL` instead of `header[0] = CFL | PRDTL <<
16` and `header[1] = PRDBC`. The HBA read PRDTL as zero, completed the command
with a D2H Register FIS and no error, and never touched the bounce buffer.

The second defect was ours in the FIS: READ DMA EXT counts real sectors and
reserves zero for 65536, while the PRDT byte count is `n - 1`. Encoding
`sectors - 1` asked the device for a 65536-sector transfer against a 512-byte
PRDT, so it again completed without moving data. Both defects produced the same
symptom, which is why the FAT32 mount reported "the volume has no boot
signature" for a volume `mdir` reads perfectly.

The gate assertion was what let the first defect through, so it now checks
content instead of success: `fat32: HELLO.TXT 27 bytes` and
`fat32: content read from the fat32 volume` cannot be satisfied by zeros. A
successful AHCI read is therefore proven by data, not by a return code.
