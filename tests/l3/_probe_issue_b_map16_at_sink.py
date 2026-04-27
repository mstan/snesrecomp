"""Issue B: Map16 tile at the EXACT addresses F44D would compute
for Mario's foot interaction point.

F465's pointer arithmetic (bank_00:13282-13312):
  byte_offset_in_screen = (Y_low & $F0) | (X_low >> 4)
  ptr_lo = (DATA_00BA60[X_high]) + byte_offset_in_screen
  ptr_hi = (DATA_00BA9C[X_high]) + Y_high + carry

For Mario at X=$01C7 (X_low=$C7, X_high=$01) Y in {$0140, $0150,
$0160, $0170}:
  screen base from BA60/BA9C[1] = $C800 + $1B0 = $C9B0
  X_low >> 4 = $C
  Y_low & $F0 -> {$40, $50, $60, $70}
  Y_high = $01

  byte_offset -> {$4C, $5C, $6C, $7C}
  ptr_lo (incl. carry) -> {$FC, $0C, $1C, $2C}
  ptr_hi (CA, CB, CB, CB depending on carry from low add)

So the WRAM addresses F44D actually reads for Mario's foot at
each Y are:
  Y=$0140: $7E:CAFC
  Y=$0150: $7E:CB0C
  Y=$0160: $7E:CB1C
  Y=$0170: $7E:CB2C
"""
from __future__ import annotations
import json, socket, sys

PORT = 4377


def cmd(sock, f, line):
    sock.sendall((line + '\n').encode())
    return json.loads(f.readline())


def _read_byte(sock, f, addr):
    h = cmd(sock, f, f'dump_ram 0x{addr:x} 1').get('hex', '').replace(' ', '')
    return int(h, 16) if h else -1


def _read_byte_word_addr(sock, f, addr):
    """Read from $7E:WRAM via the recomp's full WRAM offset (no bank)."""
    return _read_byte(sock, f, addr & 0x1FFFF)


def main():
    sock = socket.socket()
    try:
        sock.connect(('127.0.0.1', PORT))
    except ConnectionRefusedError:
        print(f'no exe on port {PORT}'); sys.exit(1)
    f = sock.makefile('r')
    f.readline()

    # Read Mario's actual position to confirm we're at the sink area.
    px_lo = _read_byte(sock, f, 0xD1)
    px_hi = _read_byte(sock, f, 0xD2)
    py_lo = _read_byte(sock, f, 0xD3)
    py_hi = _read_byte(sock, f, 0xD4)
    print(f'Mario position now: X=${(px_hi<<8)|px_lo:04x}  '
          f'Y=${(py_hi<<8)|py_lo:04x}')

    # Compute the exact F465 byte address for each test Y at Mario's
    # current X.
    bg_base_lo = 0xB0   # ($C800 + $1B0) lo for screen 1
    bg_base_hi = 0xC9   # ($C800 + $1B0) hi for screen 1

    # Verify by reading the actual screen pointers. SMW maps these
    # at $7E:0065 / $7E:0066 maybe; or DATA_00BA60 in ROM. For now
    # use the hardcoded known-good values for screen 1.

    print()
    print('F44D-equivalent probe at Mario col $C, screen 1, rows $14-$17:')
    print('     Y       byte_offset  ptr_lo     ptr_hi   addr        tile')
    for y in (0x0140, 0x0150, 0x0160, 0x0170):
        y_lo = y & 0xFF
        y_hi = (y >> 8) & 0xFF
        x_lo = 0xC7
        x_high_byte = 0x01
        col = x_lo >> 4
        byte_offset = (y_lo & 0xF0) | col
        # 8-bit add with carry-out
        ptr_lo = bg_base_lo + byte_offset
        carry = 1 if ptr_lo > 0xFF else 0
        ptr_lo &= 0xFF
        ptr_hi = (bg_base_hi + y_hi + carry) & 0xFF
        addr = (ptr_hi << 8) | ptr_lo
        tile = _read_byte_word_addr(sock, f, addr)
        marker = ''
        if y == 0x0150: marker = '   <- expected ground top'
        if y == 0x0160: marker = '   <- sink position'
        print(f'  ${y:04x}    ${byte_offset:02x}          ${ptr_lo:02x}        '
              f'${ptr_hi:02x}      $7E:{addr:04x}     ${tile:02x}{marker}')

    # Also peek at the OAM-rendered Mario column visually: print
    # tile bytes for the column $C across all rows (row index $0-$F)
    # so we can see the tile-stack at this column.
    print(f'\nTile column at X col $C across all rows in screen 1 + screen-1-with-Y-high=$01:')
    # screen 1, Y_high=$01 page:
    base_y_low_page = (bg_base_hi + 0x01) << 8 | bg_base_lo  # ~$CAB0
    print(f'  base (Y_high=$01): ${base_y_low_page:04x}')
    for row in range(0, 16):
        addr = base_y_low_page + (row << 4) + 0xC
        # carry from low add of bg_base_lo + (row<<4)|c may flow
        # into hi byte; treat as 16-bit linear for visualization:
        addr16 = ((bg_base_hi << 8) | bg_base_lo) + ((1) << 8) + ((row << 4) | 0xC)
        addr16 &= 0x1FFFF
        v = _read_byte_word_addr(sock, f, addr16)
        print(f'  row {row:2}: addr=$7E:{addr16:04x}  tile=${v:02x}')


if __name__ == '__main__':
    main()
