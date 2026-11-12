#!/usr/bin/env bash
# Boots the built artefacts in QEMU and asserts the expected serial output.
# This is the phase gate: both the BIOS and the UEFI path must pass before the
# image is written to a stick.
set -euo pipefail

cd "$(dirname "$0")"

QEMU=${QEMU:-qemu-system-x86_64}
# Every path now boots a real Linux kernel after the demos, and QEMU emulates the
# CPU in software, so these are generous on purpose: a timeout that fires during
# the payload would look like a loader bug.
BOOT_SECONDS=${BOOT_SECONDS:-60}
# OVMF under TCG spends much longer than the kernel timestamps suggest before
# OpenRC prints; the wait loop stops as soon as the banner appears.
UEFI_SECONDS=${UEFI_SECONDS:-180}
OVMF_CODE=${OVMF_CODE:-/usr/share/OVMF/OVMF_CODE_4M.fd}
OVMF_VARS=${OVMF_VARS:-/usr/share/OVMF/OVMF_VARS_4M.fd}

# The gate is only worth anything if both boot paths really ran. BMX_ALLOW_SKIPS=1
# exists for a machine that cannot run them yet (no OVMF package); it downgrades
# that to a loud warning instead of a pass.
ALLOW_SKIPS=${BMX_ALLOW_SKIPS:-0}

if ! command -v "$QEMU" >/dev/null 2>&1; then
  echo "FAIL: $QEMU is not installed, the gate cannot run" >&2
  exit 1
fi

# One image for every path. The build writes the boot record, the stage 2
# loader, the kernel and the FAT32 volume into the same sector space, which is
# also the layout of the single SATA disk the board has. A missing image must
# rebuild instead of testing a stale one.
if [ ! -f build/disk.img ]; then
  ./build.sh
fi

# The disk is the only drive, so it has to be reachable both by the firmware (to
# boot) and by the kernel's AHCI driver (to read the volume). The `pc` machine
# has no AHCI controller, so one is added; q35 has it built in. SeaBIOS and OVMF
# both know how to boot from an AHCI disk.
DISK_PC=(
  -device ich9-ahci,id=sata
  -drive file=build/disk.img,format=raw,if=none,id=disk0
  -device ide-hd,drive=disk0,bus=sata.0,bootindex=1
)

DISK_Q35=(
  -drive file=build/disk.img,format=raw,if=none,id=disk0
  -device ide-hd,drive=disk0,bootindex=1
)

fail() { echo "FAIL: $1" >&2; exit 1; }

# Runs QEMU until the marker, and the optional second marker, appear in the log,
# then stops it. The root shell never exits, so a fixed timeout would either cut
# a slow OVMF boot or waste the whole budget after init is already up. The BIOS
# path waits for the console prompt and both DHCP leases so both user-visible
# behaviours are in the log; the other paths only need the OpenRC banner.
run_until() {
  local log=$1 seconds=$2 first=$3 second=$4
  shift 4

  timeout "$seconds" "$QEMU" "$@" -display none -serial "file:$log" -no-reboot \
    >/dev/null 2>&1 &
  local pid=$!
  local spins=$((seconds * 2))

  while [ "$spins" -gt 0 ]; do
    if grep -qE "$first" "$log" 2>/dev/null &&
       { [ -z "$second" ] || grep -qE "$second" "$log" 2>/dev/null; }; then
      break
    fi
    kill -0 "$pid" 2>/dev/null || break
    sleep 0.5
    spins=$((spins - 1))
  done
  kill "$pid" 2>/dev/null || true
  wait "$pid" 2>/dev/null || true
}

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
  grep -q 'fat32: volume BMXDATA sectors [0-9][0-9]* clusters [0-9][0-9]* base [0-9][0-9]*' "$log" \
    || fail "$label: the FAT32 volume was not mounted"
  grep -q 'fat32: HELLO.TXT 27 bytes'           "$log" || fail "$label: the FAT32 file was not found"
  grep -q 'fat32: content read from the fat32 volume' "$log" \
    || fail "$label: the FAT32 file content was not read back"
  grep -q 'ramdisk: staged USER.ELF [0-9][0-9]* bytes at 0x[0-9a-f]*' "$log" \
    || fail "$label: the user program was not staged in RAM"
  grep -q 'elf: USER.ELF entry 0x[0-9a-f]* segments [1-9][0-9]* pages [1-9][0-9]*' "$log" \
    || fail "$label: the user program was not loaded into user pages"
  grep -q 'vmm: supervisor entries 0 above the user window' "$log" \
    || fail "$label: a mapping outside the user window is reachable from ring 3"
  grep -q 'user: entering ring 3 at 0x[0-9a-f]* with stack 0x[0-9a-f]*' "$log" \
    || fail "$label: the process was never started in user space"
  grep -q 'sys: first syscall from cs 0x1b'     "$log" \
    || fail "$label: the syscall gate was not entered from ring 3"
  grep -q 'user: hello from ring 3'             "$log" \
    || fail "$label: the user program could not write through a syscall"
  grep -q 'user: running in ring 3'             "$log" \
    || fail "$label: the user program did not observe CPL 3"
  grep -q 'user: exited with status 0'          "$log" \
    || fail "$label: the user program did not exit cleanly"
  grep -q 'user: returned to ring 0'            "$log" \
    || fail "$label: the kernel did not resume after the user process"
  grep -q 'pit: *[0-9]* ticks observed'         "$log" || fail "$label: PIT ticks not observed"
  grep -q 'irq: spurious interrupt self-test passed' "$log" \
    || fail "$label: a spurious IRQ7 halted the machine"
  grep -q 'irq: spurious IRQ7 ignored'          "$log" \
    || fail "$label: IRQ7 was not recognised as the spurious vector"
  if grep -q 'irq: masked unhandled irq' "$log"; then
    fail "$label: a spurious IRQ7 was masked as if it were a real interrupt"
  fi
  grep -q 'exc: resumed after breakpoint'       "$log" || fail "$label: breakpoint did not resume"
  grep -q 'exc: resumed after divide error'     "$log" || fail "$label: divide error did not resume"
  grep -q 'kbd: scancode translation self-test passed' "$log" || fail "$label: scancode translation is wrong"
  grep -q 'serial: loopback self-test passed'   "$log" || fail "$label: the UART receive path failed its loopback test"
  grep -q 'bmx: vaapi ffmpeg ready'             "$log" || fail "$label: ffmpeg was not built with VA-API"
  grep -q 'bmx: 1080p clip present'             "$log" || fail "$label: the 1080p clip is missing from the rootfs"
  grep -q 'bmx: mpv ready'                      "$log" || fail "$label: mpv is missing or does not start"
  grep -q 'bmx: bmx-decode ready'               "$log" || fail "$label: bmx-decode is missing or does not parse"
  grep -q 'bmx: bmx-play ready'                 "$log" || fail "$label: bmx-play is missing or does not parse"
  grep -q 'bmx: local console ready'            "$log" || fail "$label: the Linux VT is missing"
  grep -q 'bmx: console mouse ready'            "$log" || fail "$label: the input stack did not create /dev/input/mice"
  grep -q 'bmx: network eth0 ready'             "$log" || fail "$label: eth0 was not created"
  grep -q 'bmx: network eth1 ready'             "$log" || fail "$label: eth1 was not created"
  grep -q 'bmx: network firmware ready'         "$log" || fail "$label: the r8169 firmware is missing from the rootfs"
  grep -q 'bmx: bmx-shell ready'                "$log" || fail "$label: bmx-shell is missing or does not parse"
  grep -q 'bmx: bmx-clear ready'                "$log" || fail "$label: bmx-clear is missing or does not parse"
  grep -q 'bmx: bmx-net ready'                  "$log" || fail "$label: bmx-net is missing or does not parse"
  grep -q 'kbd: controller ready'               "$log" || fail "$label: 8042 controller was not initialised"
  grep -q 'Phase 3 complete'                    "$log" || fail "$label: phase 3 did not complete"
  grep -q 'mbr: partition table self-test passed' "$log" \
    || fail "$label: the partition table decoder failed its self-test"
  grep -q 'Phase 4 complete'                    "$log" || fail "$label: phase 4 did not complete"
  grep -aq 'console=tty0 console=ttyS0,115200' build/kernel.bin \
    || fail "$label: the Linux handoff does not open the local console"
  grep -q 'linux: LINUX.BIN [0-9][0-9]* bytes protocol 2\.' "$log" \
    || fail "$label: the Linux payload was not opened"
  grep -q 'linux: load 0x[0-9a-f]* init_size 0x[0-9a-f]* runtime 0x[0-9a-f]*' "$log" \
    || fail "$label: the payload load window was not reported"
  grep -q 'linux: boot_params 0x[0-9a-f]* cmdline 0x[0-9a-f]*' "$log" \
    || fail "$label: the zero page was not built"
  grep -q 'linux: e820 [1-9][0-9]* entries'     "$log" \
    || fail "$label: the memory map was not handed to Linux"
  grep -q 'linux: jumping to the 64-bit entry at 0x[0-9a-f]*' "$log" \
    || fail "$label: the loader never jumped to the Linux entry point"
  grep -q 'Linux version 6\.12\.115'            "$log" \
    || fail "$label: the Linux payload did not start"
  grep -q 'VFS: Mounted root (ext4 filesystem)'  "$log" \
    || fail "$label: the Alpine root filesystem was not mounted"
  grep -q 'Run /sbin/init as init process'      "$log" \
    || fail "$label: the Alpine init was not started"
  grep -q 'OpenRC [0-9]'                        "$log" \
    || fail "$label: OpenRC did not start"
}

# --- BIOS / legacy path ----------------------------------------------------
LOG=build/serial-bios.log
rm -f "$LOG"
run_until "$LOG" "$BOOT_SECONDS" 'bmx:(~|/root)#' 'bmx: network online' \
  -machine pc -cpu qemu64 \
  "${DISK_PC[@]}" \
  -m 512 -boot c -nic user,model=e1000 \
  -netdev user,id=n1,net=10.0.3.0/24 -device e1000,netdev=n1

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
grep -Eq 'bmx:(~|/root)#' "$LOG" \
  || fail 'bios: the console shell did not start in /root'
grep -q 'bmx: network up eth0 [0-9]' "$LOG" \
  || fail 'bios: DHCP did not configure eth0'
grep -q 'bmx: network up eth1 [0-9]' "$LOG" \
  || fail 'bios: DHCP did not configure eth1'
grep -q 'bmx: network online' "$LOG" \
  || fail 'bios: the network never reported both interfaces online'
if grep -q 'bmx: network down eth' "$LOG"; then
  fail 'bios: an interface reported no DHCP lease'
fi
# After ::once:/bin/dmesg -n 5, KERN_INFO link-change lines must stay off the
# console while the userspace markers above still appear.
if awk '/bmx:(~|\/root)#/{seen=1} seen' "$LOG" |
   grep -Eq 'NIC Link is (Up|Down)|Link is (Up|Down)'; then
  fail 'bios: NIC link info reached the console after dmesg -n 5'
fi

echo 'PASS: bios path reaches long mode with a framebuffer, PCI and a PS/2 keyboard controller'

# --- Q35 path: the MCFG table only exists on a platform that publishes one ---
LOG=build/serial-q35.log
rm -f "$LOG"
run_until "$LOG" "$BOOT_SECONDS" 'OpenRC [0-9]' '' \
  -machine q35 -cpu qemu64 -m 512 \
  "${DISK_Q35[@]}" \
  -boot c -nic user,model=e1000 \
  -netdev user,id=n1,net=10.0.3.0/24 -device e1000,netdev=n1

echo '--- serial output (q35) ---'
cat "$LOG"
echo '-------------------------'

common_assertions "$LOG" q35
grep -q 'acpi: mcfg ecam 0x[0-9a-f]* buses [0-9a-f][0-9a-f]-[0-9a-f][0-9a-f]' "$LOG" \
  || fail 'q35: the MCFG table did not describe an ECAM window'

echo 'PASS: q35 path parses the MCFG window'

# --- UEFI path -------------------------------------------------------------
if [ ! -f build/uefi/BOOTX64.EFI ]; then
  if [ "$ALLOW_SKIPS" = 1 ]; then
    echo 'WARNING: the UEFI loader was not built, the UEFI path did not run' >&2
    exit 0
  fi
  fail 'uefi: the UEFI loader was not built, the UEFI path did not run'
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
run_until "$LOG" "$UEFI_SECONDS" 'OpenRC [0-9]' '' \
  -machine pc -cpu qemu64 -m 512 \
  -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
  -drive if=pflash,format=raw,file=build/OVMF_VARS.fd \
  "${DISK_PC[@]}" \
  -nic user,model=e1000 \
  -netdev user,id=n1,net=10.0.3.0/24 -device e1000,netdev=n1

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
