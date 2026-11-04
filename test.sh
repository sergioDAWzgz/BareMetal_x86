#!/usr/bin/env bash
# Boots the built artefacts in QEMU and asserts the expected serial output.
# This is the phase gate: both the BIOS and the UEFI path must pass before the
# image is written to a stick.
set -euo pipefail

cd "$(dirname "$0")"

QEMU=${QEMU:-qemu-system-x86_64}
BOOT_SECONDS=${BOOT_SECONDS:-20}
UEFI_SECONDS=${UEFI_SECONDS:-25}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

# The gate is only worth anything if both boot paths and the keyboard really ran.
# BMX_ALLOW_SKIPS=1 exists for a machine that cannot run them yet (no OVMF
# package, no python3); it downgrades those to a loud warning instead of a pass.
ALLOW_SKIPS=${BMX_ALLOW_SKIPS:-0}

if ! command -v "$QEMU" >/dev/null 2>&1; then
  echo "FAIL: $QEMU is not installed, the gate cannot run" >&2
  exit 1
fi

# Both artefacts come from one build, so a missing ESP must not leave a stale
# disk image in place while the UEFI path quietly disappears.
if [ ! -f build/baremetal.img ] || [ ! -f build/esp.img ] || [ ! -f build/data.img ]; then
  ./build.sh
fi

# A second disk on its own AHCI controller. The boot disk keeps its proven wiring,
# and the kernel's SATA path is exercised identically on all three machines: the
# volume is found by its label, never by controller or port position.
DATA_DISK=(
  -device ich9-ahci,id=sata
  -drive file=build/data.img,format=raw,if=none,id=data0
  -device ide-hd,drive=data0,bus=sata.0
)

# q35 has one AHCI controller and no IDE controller, so both disks live on it.
# The boot index keeps the firmware on the boot disk: data.img carries a valid
# FAT32 boot sector, and SeaBIOS would otherwise try to execute it.
DATA_DISK_Q35=(
  -drive file=build/data.img,format=raw,if=none,id=data0
  -device ide-hd,drive=data0,bus=ide.1,bootindex=2
)

SCRATCH=$(mktemp -d)
trap 'rm -rf "$SCRATCH"' EXIT

fail() { echo "FAIL: $1" >&2; exit 1; }

common_assertions() {
  local log="$1"
  local label="$2"

  grep -q 'BareMetal_x86 - phase 3'             "$log" || fail "$label: kernel banner missing"
  grep -q 'mode:      64-bit long mode'         "$log" || fail "$label: long mode not reached"
  grep -q 'cpu: *[A-Za-z]'                      "$log" || fail "$label: cpu brand string missing"
  grep -q 'mem: usable [0-9]'                   "$log" || fail "$label: memory map summary missing"
  grep -q 'pmm: allocated 0x0000000000100000'   "$log" || fail "$label: first frame is not 0x100000"
  grep -q 'pmm: free frames [0-9]'              "$log" || fail "$label: frame allocator test missing"
  grep -q 'fb: [0-9]*x[0-9]* bpp=32'            "$log" || fail "$label: framebuffer not initialised"
  grep -q 'fb: pixel readback self-test passed' "$log" || fail "$label: framebuffer readback failed"
  grep -q 'fb: text console [0-9]*x[0-9]* cells' "$log" || fail "$label: framebuffer text console did not start"
  grep -q 'pci: [0-9]* devices enumerated'      "$log" || fail "$label: PCI enumeration did not run"
  grep -q 'pci: intel devices [1-9]'            "$log" || fail "$label: no Intel PCI device found"
  grep -q 'acpi: rsdp at 0x[0-9a-f]* revision [0-9] oem "' "$log" || fail "$label: ACPI RSDP was not found"
  grep -q 'acpi: [1-9][0-9]* tables via [rx]sdt' "$log" || fail "$label: the ACPI table walk found nothing"
  grep -q 'acpi: madt local apic 0x[0-9a-f]* ioapics [1-9]' "$log" || fail "$label: the MADT declares no IO APIC"
  grep -q 'ahci: abar 0x[0-9a-f]* ports [1-9][0-9]*' "$log" || fail "$label: the AHCI controller was not found"
  grep -q 'ahci: port [0-9][0-9]* attached'     "$log" || fail "$label: no SATA port with a drive"
  grep -q 'ahci: read 512 bytes from port [0-9][0-9]* lba 0' "$log" || fail "$label: no sector could be read from the SATA port"
  grep -q 'pit: *[0-9]* ticks observed'         "$log" || fail "$label: PIT ticks not observed"
  grep -q 'exc: resumed after breakpoint'       "$log" || fail "$label: breakpoint did not resume"
  grep -q 'exc: resumed after divide error'     "$log" || fail "$label: divide error did not resume"
  grep -q 'kbd: scancode translation self-test passed' "$log" || fail "$label: scancode translation is wrong"
  grep -q 'kbd: controller ready'               "$log" || fail "$label: 8042 controller was not initialised"
  grep -q 'Phase 3 complete'                    "$log" || fail "$label: phase 3 did not complete"
  grep -q 'Phase 4 complete'                    "$log" || fail "$label: phase 4 did not complete"
}

# --- BIOS / legacy path ----------------------------------------------------
LOG=build/serial-bios.log
MONITOR="$SCRATCH/monitor.sock"
KEYS_INJECTED=0
rm -f "$LOG"

HAVE_PYTHON=0
if command -v python3 >/dev/null 2>&1; then
  HAVE_PYTHON=1
elif [ "$ALLOW_SKIPS" != 1 ]; then
  fail 'bios: python3 is missing, the keyboard path cannot be exercised'
fi

if [ "$HAVE_PYTHON" -eq 1 ]; then
  # Run with a monitor socket so real keystrokes can be injected through the
  # emulated 8042, then asserted from the serial log.
  "$QEMU" \
    -machine pc -cpu qemu64 \
    -drive file=build/baremetal.img,format=raw,if=ide,index=0 \
    "${DATA_DISK[@]}" \
    -m 256 -boot c -display none -serial "file:$LOG" -no-reboot \
    -monitor "unix:$MONITOR,server,nowait" \
    >/dev/null 2>&1 &
  QEMU_PID=$!

  if ! python3 scripts/send-keys.py "$MONITOR" h e l l o ret --quit >/dev/null 2>&1; then
    fail 'bios: the keystroke injector never reached the QEMU monitor'
  fi
  KEYS_INJECTED=1

  for _ in $(seq 1 20); do
    kill -0 "$QEMU_PID" 2>/dev/null || break
    sleep 0.5
  done
  kill "$QEMU_PID" 2>/dev/null || true
  wait "$QEMU_PID" 2>/dev/null || true
else
  timeout "$BOOT_SECONDS" "$QEMU" \
    -machine pc -cpu qemu64 \
    -drive file=build/baremetal.img,format=raw,if=ide,index=0 \
    "${DATA_DISK[@]}" \
    -m 256 -boot c -display none -serial "file:$LOG" -no-reboot \
    >/dev/null 2>&1 || true
fi

echo '--- serial output (BIOS) ---'
cat "$LOG"
echo '---------------------------'

grep -q 'stage2: real mode up'                 "$LOG" || fail 'bios: stage 2 never reached real mode'
grep -q 'stage2: BIOS memory map collected'    "$LOG" || fail 'bios: E820 collection did not run'
grep -q 'stage2: VBE linear framebuffer active' "$LOG" || fail 'bios: VBE mode was not set'
grep -q 'stage2: kernel loaded at 0x10000'     "$LOG" || fail 'bios: kernel was not loaded'
grep -q 'boot: source=bios framebuffer=0x'     "$LOG" || fail 'bios: boot info block not published'
grep -q 'mode=1024x768x32'                     "$LOG" || fail 'bios: VBE mode is not 1024x768x32'
grep -Eq 'acpi: rsdp at .* via (ebda|bios rom)' "$LOG" || fail 'bios: the RSDP was not found by scanning'
common_assertions "$LOG" bios

if [ "$KEYS_INJECTED" -eq 1 ]; then
  grep -q 'kbd: typed "hello"'                 "$LOG" || fail 'bios: injected keystrokes never reached the kernel'
else
  echo 'WARNING: python3 is missing, keystrokes were not injected' >&2
fi

echo 'PASS: bios path reaches long mode with a framebuffer, PCI and a PS/2 keyboard'

# --- Q35 path: the MCFG table only exists on a platform that publishes one ---
LOG=build/serial-q35.log
rm -f "$LOG"
timeout "$BOOT_SECONDS" "$QEMU" \
  -machine q35 -cpu qemu64 -m 256 \
  -drive file=build/baremetal.img,format=raw,if=none,id=disk0 \
  -device ide-hd,drive=disk0,bootindex=1 \
  "${DATA_DISK_Q35[@]}" \
  -boot c -display none -serial "file:$LOG" -no-reboot \
  >/dev/null 2>&1 || true

echo '--- serial output (q35) ---'
cat "$LOG"
echo '-------------------------'

common_assertions "$LOG" q35
grep -q 'acpi: mcfg ecam 0x[0-9a-f]* buses [0-9a-f][0-9a-f]-[0-9a-f][0-9a-f]' "$LOG" \
  || fail 'q35: the MCFG table did not describe an ECAM window'

echo 'PASS: q35 path parses the MCFG window'

# --- UEFI path -------------------------------------------------------------
if [ ! -f build/esp.img ]; then
  if [ "$ALLOW_SKIPS" = 1 ]; then
    echo 'WARNING: build/esp.img is missing, the UEFI path did not run' >&2
    exit 0
  fi
  fail 'uefi: build/esp.img is missing, the UEFI path did not run'
fi
if [ ! -f "$OVMF_CODE" ] || [ ! -f "$OVMF_VARS" ]; then
  if [ "$ALLOW_SKIPS" = 1 ]; then
    echo "WARNING: OVMF firmware is missing at $OVMF_CODE, the UEFI path did not run" >&2
    exit 0
  fi
  fail "uefi: OVMF firmware not found at $OVMF_CODE, the UEFI path did not run"
fi

cp "$OVMF_VARS" build/OVMF_VARS.fd
LOG=build/serial-uefi.log
rm -f "$LOG"
timeout "$UEFI_SECONDS" "$QEMU" \
  -machine pc -cpu qemu64 -m 256 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  -drive file=build/esp.img,format=raw,if=ide,index=0 \
  "${DATA_DISK[@]}" \
  -display none -serial "file:$LOG" -no-reboot \
  >/dev/null 2>&1 || true

echo '--- serial output (UEFI) ---'
cat "$LOG"
echo '---------------------------'

grep -q 'uefi: loader started'                 "$LOG" || fail 'uefi: loader never ran'
grep -q 'uefi: rsdp 0x[0-9a-f]*'               "$LOG" || fail 'uefi: the ACPI root pointer was not read from the configuration table'
grep -q 'uefi: gop [0-9]*x[0-9]* pitch='       "$LOG" || fail 'uefi: GOP mode not negotiated'
grep -q 'uefi: kernel [0-9]* bytes at 0x'      "$LOG" || fail 'uefi: kernel was not loaded'
grep -q 'uefi: memory map [1-9][0-9]* entries published' "$LOG" || fail 'uefi: memory map not published'
grep -q 'uefi: cr3 switched'                   "$LOG" || fail 'uefi: the loader never installed its own page tables'
grep -q 'uefi: kernel byte 0x'                 "$LOG" || fail 'uefi: the kernel window does not resolve after the switch'
grep -q 'uefi: jumping to the kernel'          "$LOG" || fail 'uefi: never left boot services'
grep -q 'boot: source=uefi framebuffer=0x'     "$LOG" || fail 'uefi: boot info block not published'
grep -q 'boot: .* mode=1024x768x32'            "$LOG" || fail 'uefi: GOP mode is not 1024x768x32'
grep -q 'acpi: rsdp at .* via bootinfo'        "$LOG" || fail 'uefi: the RSDP did not come from the boot info block'
common_assertions "$LOG" uefi

echo 'PASS: uefi path boots the same kernel through GOP'
