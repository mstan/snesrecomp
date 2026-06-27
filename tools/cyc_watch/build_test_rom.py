#!/usr/bin/env python3
"""Build a minimal LoROM .sfc with a known 65816 instruction stream, for the
Axis-2 recomp-model-vs-bsnes cycle cross-check (tools/cyc_watch).

The program (at $00:8000) enters native 16-bit and runs a 3-iteration RMW loop
ending in STP — the same shape cyc_trace.c validates interp816+authority on,
so the authority's CPU-cycle prediction for the region is known exactly. bsnes
runs this ROM and (via the bsnes_set_cyc_anchor hook) reports its own CPU-cycle
count for the same guest-PC region; the two must agree.

Anchors: START=$008000 (reset target, hit once), END=$008011 (the STP, hit
once). Authority prediction for [START, END) CPU cycles:
  CLC 2 + XCE 2 + REP#$30 3 + LDX#$0003 3            = 10
  loop x3: LDA $1000 (16b) 5 + INC A 2 + STA $1000 5 + DEX 2 + BNE(taken 3 / not 2)
           iter1 17 + iter2 17 + iter3 16            = 50
  TOTAL                                              = 60 CPU cycles
"""
import sys

# mode 'static': base + width + branch-taken model (no dp / no page-cross).
PROG_STATIC = bytes([
    0x18,             # 8000 CLC
    0xFB,             # 8001 XCE      -> native
    0xC2, 0x30,       # 8002 REP #$30 -> 16-bit A+X
    0xA2, 0x03, 0x00, # 8004 LDX #$0003
    # START = $8007
    0xAD, 0x00, 0x10, # 8007 LDA $1000
    0x1A,             # 800A INC A
    0x8D, 0x00, 0x10, # 800B STA $1000
    0xCA,             # 800E DEX
    0xD0, 0xF6,       # 800F BNE $8007
    # END = $8011
    0xDB,             # 8011 STP
])

# mode 'dynamics': exercises the runtime-only charges — D.l!=0 (dp) and an
# abs,X read page-cross. Region [START=$800B, END=$8011):
#   LDA $00   (dp, m=0, D.l=$34!=0): base 3 +1(m=0) +1(D.l) = 5
#   LDA $80FF,X (abs,X read, m=0, X=1 -> $8100 crosses page): 4 +1(m=0) +1(cross) = 6
#   NOP                                                                       = 2
#   TOTAL                                                                     = 13
PROG_DYNAMICS = bytes([
    0x18,             # 8000 CLC
    0xFB,             # 8001 XCE      -> native
    0xC2, 0x30,       # 8002 REP #$30 -> 16-bit A+X
    0xA9, 0x34, 0x12, # 8004 LDA #$1234
    0x5B,             # 8007 TCD          -> D = $1234 (D.l = $34, nonzero)
    0xA2, 0x01, 0x00, # 8008 LDX #$0001
    # START = $800B
    0xA5, 0x00,       # 800B LDA $00      (dp, D.l!=0)
    0xBD, 0xFF, 0x80, # 800D LDA $80FF,X  (abs,X read, crosses to $8100)
    0xEA,             # 8010 NOP
    # END = $8011
    0xDB,             # 8011 STP
])

MODES = {
    'static':   (PROG_STATIC,   0x008000, 0x008011, 60),
    'dynamics': (PROG_DYNAMICS, 0x00800B, 0x008011, 13),
}


def build(prog: bytes) -> bytes:
    rom = bytearray(b'\x00' * 0x8000)          # 32 KB LoROM
    rom[0:len(prog)] = prog

    # LoROM internal header at file offset $7FC0.
    h = 0x7FC0
    title = b'CYCTEST'.ljust(21, b' ')
    rom[h:h + 21] = title
    rom[h + 0x15] = 0x20   # map mode: LoROM, slow
    rom[h + 0x16] = 0x00   # chipset: ROM only
    rom[h + 0x17] = 0x05   # ROM size: 1<<5 = 32 KB
    rom[h + 0x18] = 0x00   # RAM size: none
    rom[h + 0x19] = 0x01   # country: US/NTSC
    rom[h + 0x1A] = 0x33   # developer id
    rom[h + 0x1B] = 0x00   # version

    # Vectors. Emulation RESET ($7FFC) = $8000; the rest point at $8000 too
    # (harmless — no interrupts fire before STP: NMI is disabled at reset and
    # the program halts within microseconds, long before the first vblank).
    def putw(off, val):
        rom[off] = val & 0xFF
        rom[off + 1] = (val >> 8) & 0xFF
    for off in range(0x7FE0, 0x8000, 2):       # all native + emulation vectors
        putw(off, 0x8000)
    putw(0x7FFC, 0x8000)                        # RESET

    # Checksum (complement at $7FDC, checksum at $7FDE), computed with both
    # fields treated as 0xFF/0x00 respectively per convention.
    putw(0x7FDC, 0x0000)
    putw(0x7FDE, 0x0000)
    s = sum(rom) & 0xFFFF
    putw(0x7FDE, s)
    putw(0x7FDC, s ^ 0xFFFF)
    return bytes(rom)


def main(argv):
    out = argv[0] if argv else 'cyctest.sfc'
    mode = argv[1] if len(argv) > 1 else 'static'
    prog, start, end, expected = MODES[mode]
    data = build(prog)
    with open(out, 'wb') as f:
        f.write(data)
    print(f'wrote {out} ({len(data)} bytes) mode={mode}; '
          f'START=${start:06X} END=${end:06X} expected region = {expected} CPU cycles')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
