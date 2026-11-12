# Target hardware: Jetway JNF3A

Everything here is confirmed against Jetway and Intel documentation. Items marked
**verify on your unit** are inferred from the same AMI firmware family and must be
confirmed in the BIOS setup before they are relied on.

## Board

| Item | Value |
| --- | --- |
| Model | Jetway JNF3A series (`JNF3A-2930` / `JNF3A-2807`) |
| Form factor | 3.5" SBC, 148 x 102 mm |
| SoC | Intel Bay Trail-M, soldered (FC-BGA 1170), no socket |
| Memory | 1 x SO-DIMM DDR3L-1333, **1.35 V only**, 4 GB max on N2807 / 8 GB max on N2930 |
| Storage | 1 x SATA II (3 Gb/s), 1 x full-size mPCIe shared with mSATA |
| Expansion | 1 x full-size mPCIe/mSATA, 1 x half-size mPCIe |
| Network | 2 x Realtek RTL8111H GbE |
| Audio | Realtek ALC888S HD Audio codec, 2-channel |
| Serial | COM1 RS-232/422/485 (external) + 3 x RS-232 internal headers |
| USB | 1 x USB 3.0, 7 x USB 2.0 |
| Display | DVI-I + LVDS (dual display), integrated Intel HD Graphics |
| Power | 9-24 V DC-in |
| Firmware | AMI UEFI, 64 Mb SPI flash (legacy CSM present, **verify on your unit**) |

Single SO-DIMM slot means **single-channel memory** even with the quad-core
N2930, so the SoC's dual-channel capability is not realised on this board.

## CPU options

| | Celeron N2807 | Celeron N2930 |
| --- | --- | --- |
| Cores / threads | 2 / 2 | 4 / 4 |
| Clock / burst | 1.58 / 2.16 GHz | 1.83 / 2.16 GHz |
| L2 cache | 1 MB | 2 MB |
| TDP / SDP | 4.3 W / 2.5 W | 7.5 W / 4.5 W |
| Memory channels | 1 | 2 (board provides 1 slot) |
| iGPU | HD Graphics, 4 EUs, 313-750 MHz | HD Graphics, 4 EUs, 313-854 MHz |

Microarchitecture is Silvermont (22 nm): **SSE4.2 yes, AVX no, AVX2 no**. Any SIMD
code path written for the decoder must stay at SSE4.2.

Both parts expose Intel Quick Sync Video, i.e. fixed-function H.264 decode. That
block is only reachable from Linux through the `i915` driver and VA-API.

## What this means for the project

- Serial console exists (COM1), so early bring-up never depends on graphics.
  COM1 is the board's debug console at 115200 8N1 and needs a null-modem cable
  from a DTE host: TX and RX must cross, or the log appears without input.
- The normal console after the Linux handoff is the board's own: i915 modesets
  the monitor, fbcon paints the virtual terminals, and the 8 USB ports (1 USB 3.0
  and 7 USB 2.0, so xHCI and EHCI) carry the keyboard and mouse.
- The RTL8111H is an ordinary PCIe NIC, so Linux's in-tree `r8169` drives it. The
  bare-metal kernel only enumerates it; DHCP, routes and DNS live in the root
  filesystem, so no network driver is needed before the Linux handoff.
- The iGPU is Gen7 (Ivy Bridge generation, 4 EUs). Intel publishes Gen7 PRMs, but
  a from-scratch driver plus VA-API stack is a multi-year effort. Own-kernel video
  output will be a linear framebuffer only.
- 1080p H.264 hardware decode requires Linux + `i915` + `libva`. Software decode on
  this silicon is realistic at 720p on the N2930 and marginal on the two-core N2807.
- Therefore the roadmap splits into a bare-metal core (this repository's kernel) and
  a Linux payload booted by the same loader (see `docs/roadmap.md`, phase 5).

## Serial headers

The board has four internal 9-pin COM headers (COM1-COM4) plus the external COM1
DB9. The Jetway manual (`G03-NF3A-F`, page 18) defines the internal block:

| Pin | RS-232 | RS-422 (COM1 header) | RS-485 (COM1 header) |
| --- | --- | --- | --- |
| 1 | DCD | TX- | DATA- |
| 2 | RXD | TX+ | DATA+ |
| 3 | TXD | RX+ | NC |
| 4 | DTR | RX- | NC |
| 5 | GND | GND | GND |
| 6 | DSR | NC | NC |
| 7 | RTS | NC | NC |
| 8 | CTS | NC | NC |
| 9 | RI | NC | NC |

Only the COM1 header carries RS-422/485; the BIOS "Transmission Mode Select" picks
the mode. JP6 selects the pin-9 function on COM1 (RI, 5V or 12V), and JP3/JP4/JP5
do the same for COM4/COM3/COM2. The block is a 2x5 header: confirm pin 1 on the
silkscreen before wiring. The console used by this project is the external COM1
DB9, wired with a null-modem cable at 115200 8N1.

## Sources

- Jetway JNF3A series datasheet (jetwayipc.com / jetwaycomputer.com)
- Jetway G03-NF3A-F technical manual (serial headers and BIOS options)
- Intel ARK: Celeron N2930 (SKU 81073), Celeron N2807
- Intel Open Source Graphics Programmer's Reference Manual, Gen7
