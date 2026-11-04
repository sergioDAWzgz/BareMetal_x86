# Code Review Rules

Bare-metal x86 kernel. No operating system, no libc, no runtime below this code.

## C (kernel/)

- Freestanding C11 (`-ffreestanding -fno-stack-protector -mno-red-zone`). Never
  assume a libc symbol exists; the kernel links only against itself.
- No dynamic allocation and no VLAs. Every buffer has a compile-time bound and
  every copy is length-checked.
- No floating point anywhere in kernel code: the FPU/SSE state is never saved.
- Use the fixed-width types from `stdint.h` for anything that touches hardware,
  MMIO, page tables or the boot info block. `int` is fine for loop counters.
- Physical memory is only reachable through the direct map at
  `0xFFFF800000000000`. Never dereference a physical address as if it were a
  virtual one, and never hide that translation in a cast.
- Volatile is required for MMIO, the framebuffer and anything an interrupt
  handler writes. If you remove it, say why.
- Keep byte-level ownership in one place: `fb.c` touches framebuffer bytes,
  `fbtext.c` owns glyphs and cursor state. Do not reach around a module for its
  internals.
- Device decoding that can be tested without hardware must be a pure function
  (see `kbd_translate`), so a self-test can replay a recorded byte stream.
- `static` for everything a module does not export, and a module prefix on
  exported symbols (`fb_`, `fbtext_`, `kbd_`, `pmm_`, `pci_`).

## Assembly (boot/, kernel/*.asm)

- NASM syntax. State the mode explicitly (`BITS 16`, `BITS 32`, `BITS 64`) at the
  top of each section; a wrong mode is a silent corruption, not a build error.
- Comment every port I/O, every control-register write and every stack switch
  with the reason it is needed, not the fact that it happens.
- Structures handed to the kernel (boot info block, memory map) are a published
  contract: changing a field offset or the magic value breaks both boot paths.

## Boot paths

- The BIOS (`boot/stage2.asm`) and UEFI (`boot/uefi/loader.c`) loaders must
  publish the same boot info block. A change on one path without the other is a
  defect, not an omission.
- After `ExitBootServices` only file-scope globals and constants are readable.
  A stack switch invalidates the current frame's locals.

## Scripts (build.sh, test.sh, run.sh, scripts/)

- `set -euo pipefail` in every shell script.
- These scripts are the gate, so they must fail loudly: no `|| true` around an
  assertion, and no test that can pass by matching an empty log.
- When a change adds output the gate depends on, assert on it in `test.sh` for
  both boot paths.

## Everything

- All artifacts are in English: code, identifiers, comments, commit messages,
  documentation.
- Conventional commits, no AI attribution trailers.
- Every phase must pass `./test.sh` on both the BIOS and the UEFI path before it
  is considered done.

## Review output

- Emit exactly one line, either `STATUS: PASSED` or `STATUS: FAILED`, as the very
  first line of the answer, with the characters `STATUS:` starting the line. No
  markdown emphasis, no bold, no bullet, no trailing text.
- Never repeat that line anywhere else in the answer, not even inside the
  reasoning or as a closing summary. The pre-commit hook counts the matching
  lines and rejects the commit when it does not find exactly one.
- Put the reasoning after that line, never before it.
