# Phase 5 - Linux payload

Bare-metal x86 kernel on the Jetway JNF3A (Intel Bay Trail N2807/N2930). This
document is the specification every task reads. Objective: the boot chain stops
being the destination and becomes the loader for a real Linux kernel, so the
Gen7 block that only `i915` and VA-API can reach becomes usable.

Branch: `main`. Test runner: `./test.sh` (BIOS, q35 and UEFI paths, QEMU).
Delivery strategy: work-unit commits on `main`, one reviewable slice per task,
`ask-on-risk` for any pull-request chaining.

## Specs

- **S1** - the kernel is built from pinned source.
  `build.sh` fetches `linux-6.12.115.tar.xz` from `cdn.kernel.org`, verifies
  `sha256 0ec90fe4b20422b1b664b77e6d2dde22b40bd41e7eb2cad8ac8739a4749cb1bb`,
  applies `configs/linux-baytrail.fragment` on top of `defconfig`, builds
  `bzImage` and copies it to `build/bzImage`. Both the version and the hash are
  constants in `build.sh`; an unpinned download would make the image depend on
  whatever the mirror serves that day.

  The download, the extracted tree and the build tree live in
  `${BMX_LINUX_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/bmx}`, not under `build/`
  and not under `build-cache/`: the kernel's top-level `Makefile` refuses a
  source path containing a space or a colon (`*** source directory cannot
  contain spaces or colons.`), and this repository is checked out under
  `Linux distro for playing video`. The cache survives the `build/` clean, so a
  rebuild only happens when the version or the fragment changes.
- **S2** - the loader hands the machine over.
  Our kernel reads `LINUX.BIN` from the mounted FAT32 volume straight into the
  load region the image header asks for, parses the setup header, fills a zeroed
  `boot_params` (header copied from the image, `code32_start`, the E820 map the
  loader already validated, `cmd_line_ptr`) and jumps to the 64-bit entry point
  with the CPU in long mode and interrupts off. It never returns.

  The header address is a preference, not a constraint: the kernel is relocatable,
  and firmware owns memory the header cannot know about — on the UEFI path a
  reserved block sits immediately above 16 MiB, exactly where `pref_address`
  points. The loader therefore walks up from the preferred address to the first
  window that is actually free, reserves
  `[load, runtime_start + align_up(init_size, kernel_alignment))`, and lets
  `code32_start` name where the image really landed.
- **S3** - the payload reaches userspace.
  A gzip initramfs is loaded below the direct map and handed over through
  `ramdisk_image`/`ramdisk_size`; the kernel boots with `console=ttyS0` and
  `rdinit=/init`, and `/init` prints a marker on COM1.
- **S4** - the gate.
  `test.sh` asserts, on the BIOS path, the q35 path and the UEFI path, the
  handoff summary our kernel prints, Linux's own version banner, and the
  initramfs marker.
- **S5** - the hardware caveat is recorded, not hidden.
  `i915` and VA-API need Gen7 silicon; QEMU cannot provide it. The `vainfo`
  line of the roadmap gate is therefore a phase 6 hardware assertion. Phase 5 is
  done when the payload boots to a userspace process on all three QEMU paths, and
  `docs/roadmap.md` says exactly that instead of implying QEMU proved video.
- **S6** - documentation.
  `README.md` and `docs/roadmap.md` describe the payload, the pinned kernel, the
  new build cache and the caching rule, in English.

Exact strings the gate asserts (they are the contract):

```
kbd: idle, leaving the echo loop
linux: LINUX.BIN <dec> bytes protocol 2.15 setup <dec> sectors
linux: load 0x<hex> init_size 0x<hex> runtime 0x<hex>
linux: boot_params 0x<hex> cmdline 0x<hex>
linux: e820 <dec> entries
linux: jumping to the 64-bit entry at 0x<hex>
Linux version 6.12.115
```

T2 adds, once the initramfs exists:

```
linux: initrd 0x<hex> <dec> bytes
bmx: initramfs marker
```

`kbd: idle, leaving the echo loop` is in the contract because the payload runs
after the echo loop: the two gate paths that inject no keystrokes can only reach
Linux because the loop yields on its own after `5 * TIMER_HZ`.

## Tasks

- **T1** - kernel source, build and the handoff. `configs/linux-baytrail.fragment`,
  the kernel section of `build.sh`, `kernel/linuxboot.c`, `kernel/linuxboot.h`,
  `kernel/linuxjump.asm`, `pmm_reserve_range` in `kernel/memory.c`,
  `fat32_read_at` in `kernel/fat32.c`, `acpi_rsdp_physical` in `kernel/acpi.c`,
  the `LINUX.BIN` copy into `build/data.img`, the `kbd: idle` yield in
  `kernel/kmain.c`, and the gate assertions. One unit because a built kernel
  nobody boots proves nothing. Covers S1, S2, S4. Route: inline.
- **T2** - initramfs. `initramfs/init` and a `cpio` step in `build.sh`, the
  ramdisk handoff, `rdinit=/init`. Covers S3, S4. Route: inline.
- **T3** - userspace stack and documentation. Busybox, libva and `vainfo` in the
  initramfs, `README.md`, `docs/roadmap.md`, and the S5 caveat. Covers S5, S6.
  Route: inline.

## Log

### L1 (user, verbatim)

> Procede con la siguiente fase

### L2 (analysis, 2026-11-04)

Payload decision. The roadmap already chose 5b: boot a Linux kernel and reuse
`i915` plus VA-API instead of writing a Gen7 driver. The kernel source question
was put to the user and answered: build from source, not a downloaded distro
package. That is why `build.sh` gains a pinned tarball and a config fragment
instead of an apt dependency.

Version decision. 6.12.115 is the current 6.12 longterm. Longterm over latest
stable because the target is a 2013-era SoC and the point of the payload is that
its drivers are boring.

Loader or bypass decision. The Linux kernel is loaded by our kernel, not by
stage2 or by the UEFI loader. Both boot paths keep running the same code, which
is this project's strongest invariant; our kernel already has the AHCI driver,
the FAT32 driver, a contiguous frame allocator and an E820 map it has validated,
so the handoff is the smallest new surface. The cost is that the payload runs
after everything phase 4 proved, which is also what makes the gate meaningful.

Gate defect found while reading the roadmap. "5b boots and `vainfo` reports the
H.264 decode entrypoint" cannot be asserted in QEMU: `i915` needs Gen7 hardware
and QEMU has no such device. The QEMU-verifiable part is the boot to userspace;
the `vainfo` line belongs to the phase 6 board bring-up. Recorded as S5 rather
than quietly redefined.

### L3 (build defect, 2026-11-05)

`kernel/linuxboot.c` and `kernel/linuxboot.asm` both produced
`build/kernel/linuxboot.o`, so NASM's object overwrote the C one and the link
failed with `multiple definition of linux_jump`. Renamed the assembly file to
`kernel/linuxjump.asm`: the file is a jump trampoline, not a module, and giving
it its own name makes the collision impossible instead of hiding it behind a
renamed output path in the script.

### L4 (host environment, 2026-11-05)

The kernel work area, the pinned tarball and the bootstrapped `flex`/`bison`/`m4`
moved to `${XDG_CACHE_HOME:-$HOME/.cache}/bmx`. Two host facts forced the move
and both are worth writing down because they cost time:

1. The kernel `Makefile` aborts with `*** source directory cannot contain spaces
   or colons.` when the source path has a space, and this checkout is under
   `Linux distro for playing video`.
2. Ubuntu's `bison` is compiled with `M4=/usr/bin/m4` and does not search `PATH`.
   With no system `m4` it fails as `m4 subprocess failed: No such file or
   directory`, and the fix is exporting `M4` to the bootstrapped binary, not
   editing `PATH`.

`build-cache/` therefore no longer exists and the `.gitignore` entry for it was
dropped. This is host setup, not repository content: `build.sh` keeps printing
the `apt-get install build-essential bc flex bison` hint for a host that can
install them normally.

### L5 (scope, 2026-11-05)

The echo loop yields after five seconds without a key so the q35 and UEFI paths,
which inject no keystrokes, still reach the payload. The payload runs after the
loop rather than before it, so the BIOS path keeps proving the 8042 driver end to
end in the same run that proves the handoff.

### L6 (build defects, 2026-11-05)

Three host-side traps, all found by building rather than by reading:

1. `tools/objtool` cannot be avoided on x86_64 in this series. `arch/x86/Kconfig`
   does `select HAVE_OBJTOOL if X86_64`, and `HAVE_UACCESS_VALIDATION` (also
   selected from `HAVE_OBJTOOL`) does `select OBJTOOL` unconditionally. Turning
   off the ORC unwinder, the frame-pointer validator, retpoline, IBT, DEBUG_ENTRY
   and JUMP_LABEL changes nothing; objtool still builds. So the config no longer
   tries, and `libelf-dev` is a build dependency like `bc`.
2. `tools/scripts/Makefile.include` runs `cd $(PWD)` whenever `O=` comes from the
   command line, and `PWD` is the *shell's* variable, not make's: running make from
   a checkout whose path contains a space truncates it at the space
   (`cd: can't cd to /mnt/c/Users/pvinyas/Documents/Linux`). The build now runs
   make from inside the cache directory, which fixes it for any checkout path.
3. The fragment stamp used to `rm -rf` the build tree, which made every fragment
   edit a full rebuild. It now regenerates `.config` from `defconfig` in place and
   lets make rebuild only what changed, so an edit costs a short incremental build
   instead of fifteen minutes.

### L7 (build defect, 2026-11-05)

`certs/Makefile` declares `hostprogs := extract-cert` unconditionally and `certs/`
is part of `core-y`, so every x86 build compiles that helper and links it against
`libcrypto` no matter what the signing options say. Turning off
`CONFIG_SYSTEM_TRUSTED_KEYRING` therefore bought nothing: the build still died at
`certs/extract-cert.c:21: fatal error: openssl/bio.h`. The option was reverted and
OpenSSL is now an honest host dependency, probed by `build.sh` before the build
starts (compile and link, not just the header, because a missing shared library
fails at link time).

For the same reason the fragment no longer pretends `CONFIG_MODULES=n` removes a
host dependency; it is a payload decision, not a build simplification.

One detail worth recording: `libssl-dev` keeps `opensslconf.h` in the multiarch
include directory (`usr/include/x86_64-linux-gnu/openssl/`). A normal host has
that directory on gcc's default search path, so only a hand-unpacked prefix has to
add it explicitly.

### L8 (loader defect found by the gate, 2026-11-05)

The first real gate run booted Linux on the BIOS path and on q35 and then failed
the UEFI path *before* the jump:

```
linux: the load window overlaps memory the kernel already handed out
```

The UEFI memory map the loader publishes has `[0x810000, 0x1500000)` marked
reserved, and `pref_address` is `0x1000000`: the preferred window starts inside
that block. BIOS and q35 pass because SeaBIOS leaves the same range usable.

Two separate mistakes, both fixed in `kernel/linuxboot.c`:

1. **The preferred address was treated as mandatory.** The kernel is relocatable
   (a non-relocatable image is already refused), so the header address is only a
   hint. The loader now walks up in `kernel_alignment` steps from the preferred
   address and reserves the first window that is free. `code32_start` already
   named the real load address, so the handoff needed no other change.
   `head_64.S` derives the decompression target from `%rip` (`leaq startup_32(%rip)`
   rounded up to `kernel_alignment`), not from the compile-time constant, so
   relocation moves the output window with it.

2. **The window was one `align_up` short.** We reserved
   `runtime_start + init_size`. `extract_kernel` actually writes
   `needed_size = ALIGN(max(output_len, kernel_total_size), MIN_KERNEL_ALIGN)`
   bytes from `output`, which is `runtime_start` — and on this image that is
   `0x2a00000` against an `init_size` of `0x29ad000`. The shortfall is 332 KiB,
   and the frames it covers are handed out by `pmm_alloc_frame()` afterwards, so
   the zero page or the command line could have landed inside the decompression
   output. The reservation is now `align_up(init_size, kernel_alignment)`.

`pmm_limit()` was added to `memory.h` so the search can tell "this range is
taken" (retry higher) from "this range is past the end of the map" (give up)
without duplicating `PMM_LIMIT` in the loader.

### L9 (T1 closed, 2026-11-05)

The gate is green on the three paths after L8. The UEFI path now relocates to
`load 0x1600000` / `runtime 0x1800000` / entry `0x1605200` and reaches
`Linux version 6.12.115`, exactly like BIOS and q35:

```
PASS: bios path reaches long mode with a framebuffer, PCI and a PS/2 keyboard
PASS: q35 path parses the MCFG window
PASS: uefi path boots the same kernel through GOP
```

The `Kernel panic - not syncing: VFS: Unable to mount root fs` at ~9 s on every
path is **expected and is not a gate failure**: there is no initramfs yet, so
`prepare_namespace()` has nothing to mount. It disappears with T2, and no
assertion in `test.sh` mentions it.

Two things about reading this gate are worth writing down, because both cost
time:

1. **The `Exited with code 1` reported by the outer PowerShell/WSL wrapper is not
   the status of `test.sh`** once the output is large enough to be truncated by
   the tool. Running `./test.sh` with `echo "TEST_STATUS=$?"` behind it printed
   `TEST_STATUS=0` with the identical log. For a big run, assert on the final
   marker, never on the wrapper's exit code.

2. **`trap ... EXIT` under `set -e` can override a successful status.** Measured:
   `bash -c 'set -euo pipefail; trap "false" EXIT; exit 0'` returns 1, while
   `trap "false || true" EXIT` returns 0 and still returns 1 when the script
   genuinely exits 1. `test.sh` cleaned its scratch directory with a bare
   `rm -rf`, so a `/tmp` that could not be emptied would have turned a green gate
   red for a reason that says nothing about the image. The cleanup now reports a
   warning and keeps its hands off the verdict; the assertions stay the only
   thing that can fail that file.


