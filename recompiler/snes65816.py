#!/usr/bin/env python3
"""
tools/recomp/snes65816.py -- Shared 65816 decoder and ROM utilities

Used by both recomp.py (the C emitter) and discover.py (function discovery).
"""

from typing import Optional, List

# ==============================================================================
# ROM LOADING
# ==============================================================================

ROM_MAP_LOROM = 'lorom'
ROM_MAP_HIROM = 'hirom'
_active_rom_mapping = ROM_MAP_LOROM


def _header_score(data: bytes, base: int, expected_low_nibble: int) -> int:
    """Score a conventional 32-byte SNES header without trusting its title.

    The score is intentionally conservative: map-mode agreement, a plausible
    reset vector, and a checksum/complement pair are independent signals. A
    tie preserves the historical LoROM default.
    """
    if base < 0 or base + 0x40 > len(data):
        return -1
    score = 0
    map_mode = data[base + 0x15]
    if (map_mode & 0x0F) == expected_low_nibble:
        score += 4
    reset = data[base + 0x3C] | (data[base + 0x3D] << 8)
    if reset >= 0x8000 and reset != 0xFFFF:
        score += 2
    complement = data[base + 0x1C] | (data[base + 0x1D] << 8)
    checksum = data[base + 0x1E] | (data[base + 0x1F] << 8)
    if (checksum ^ complement) == 0xFFFF:
        score += 2
    return score


def detect_rom_mapping(data: bytes) -> str:
    """Detect standard LoROM versus HiROM from their internal headers."""
    lorom_score = _header_score(data, 0x7FC0, 0)
    hirom_score = _header_score(data, 0xFFC0, 1)
    return ROM_MAP_HIROM if hirom_score > lorom_score else ROM_MAP_LOROM


def set_rom_mapping(mapping: str) -> None:
    global _active_rom_mapping
    if mapping not in (ROM_MAP_LOROM, ROM_MAP_HIROM):
        raise ValueError(f"unsupported ROM mapping: {mapping}")
    _active_rom_mapping = mapping


def get_rom_mapping() -> str:
    return _active_rom_mapping


def load_rom(path: str) -> bytes:
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) % 1024 == 512:
        data = data[512:]
    set_rom_mapping(detect_rom_mapping(data))
    return data

def lorom_offset(bank: int, addr: int) -> int:
    """Active cartridge mapping's (bank, addr) -> physical ROM offset.

    The legacy name remains as a compatibility alias for the v1/v2 decoder
    modules. New code should call ``rom_offset``.
    """
    return rom_offset(bank, addr)


def rom_offset(bank: int, addr: int) -> int:
    bank &= 0xFF
    addr &= 0xFFFF
    assert bank not in (0x7E, 0x7F), (
        f"address ${bank:02X}:{addr:04X} is WRAM, not ROM")
    if _active_rom_mapping == ROM_MAP_HIROM:
        canonical_bank = bank & 0x7F
        assert canonical_bank >= 0x40 or addr >= 0x8000, (
            f"address ${bank:02X}:{addr:04X} is not in a HiROM ROM window")
        return ((canonical_bank & 0x3F) << 16) | addr
    assert 0x8000 <= addr <= 0xFFFF, (
        f"addr ${addr:04X} not in LoROM range $8000-$FFFF")
    return (bank & 0x7F) * 0x8000 + (addr - 0x8000)


def is_rom_address(bank: int, addr: int) -> bool:
    bank &= 0xFF
    addr &= 0xFFFF
    if bank in (0x7E, 0x7F):
        return False
    if _active_rom_mapping == ROM_MAP_HIROM:
        canonical_bank = bank & 0x7F
        return canonical_bank >= 0x40 or addr >= 0x8000
    return addr >= 0x8000 and ((bank & 0xFF) < 0x40 or bank >= 0x80)


def vector_table_offset(data: bytes) -> int:
    """Return the physical offset of the $FFE0-$FFFF vector table."""
    mapping = detect_rom_mapping(data)
    return 0xFFE0 if mapping == ROM_MAP_HIROM else 0x7FE0

def rom_slice(rom: bytes, bank: int, addr: int, length: int) -> bytes:
    off = rom_offset(bank, addr)
    return rom[off:off + length]

# ==============================================================================
# 65816 DECODER
# ==============================================================================

# Addressing modes
(IMP, ACC, IMM, DP, DP_X, DP_Y, ABS, ABS_X, ABS_Y,
 LONG, LONG_X, REL, REL16, STK, INDIR, INDIR_X, INDIR_Y, INDIR_LY,
 INDIR_L, INDIR_DPX, DP_INDIR, STK_IY) = range(22)

MODE_STR = {
    IMP:'imp', ACC:'acc', IMM:'imm', DP:'dp', DP_X:'dp,x', DP_Y:'dp,y',
    ABS:'abs', ABS_X:'abs,x', ABS_Y:'abs,y', LONG:'long', LONG_X:'long,x',
    REL:'rel', REL16:'rel16', STK:'stk', INDIR:'(abs)', INDIR_X:'(abs,x)',
    INDIR_Y:'(dp),y', INDIR_LY:'[dp],y', INDIR_L:'[dp]', INDIR_DPX:'(dp,x)',
    DP_INDIR:'(dp)', STK_IY:'(stk,S),Y',
}

class Insn:
    __slots__ = ('addr', 'opcode', 'mnem', 'mode', 'operand', 'length',
                 'dispatch_entries', 'dispatch_kind', 'dispatch_idx_reg',
                 'dispatch_table_bases', 'm_flag', 'x_flag', 'dispatch_terminal',
                 'dispatch_call', 'dispatch_pushed_call',
                 'dispatch_pushed_call_frame_size',
                 'dispatch_return_pc', 'dispatch_return_m', 'dispatch_return_x',
                 'dispatch_local_goto', 'dispatch_runtime',
                 'const_z_fold_unconditional', 'const_z_fold_dead_pc24')

    def __init__(self, addr, opcode, mnem, mode, operand, length):
        self.addr = addr
        self.opcode = opcode
        self.mnem = mnem
        self.mode = mode
        self.operand = operand
        self.length = length
        self.dispatch_entries = None
        self.dispatch_kind = None
        # cfg-resolved indirect_dispatch sites carry the index register
        # ('X' or 'Y') here so codegen emits a switch on the right
        # source. None for legacy dispatch-helper sites which use A.
        self.dispatch_idx_reg = None
        # Non-empty for cfg/auto resolved indirect dispatches whose targets
        # came from static table base(s). len >= 2 means parallel byte tables,
        # where the index register is already a logical entry index.
        self.dispatch_table_bases = ()
        self.dispatch_terminal = False
        # Pointer-sourced CALL idiom (PEA <ret>; JMP (ptr)): non-terminal
        # indirect call that falls through to the next block. See cfg_loader
        # `ptrcall` + codegen _emit_indirect_dispatch.
        self.dispatch_call = False
        # PHK; PEA <ret-1>; JML [ptr] variant: a 3-byte return frame is
        # already on the guest stack and dispatch_return_pc names the compiled
        # continuation to resume after the dynamic callee consumes it.
        self.dispatch_pushed_call = False
        self.dispatch_pushed_call_frame_size = 0
        self.dispatch_return_pc = None
        self.dispatch_return_m = None
        self.dispatch_return_x = None
        # Absolute-indirect dispatch that jumps to labels inside the current
        # decoded function, not to standalone handler functions.
        self.dispatch_local_goto = False
        # Reachable JSR (abs,X) whose pointer-table base is in WRAM
        # ($0000-$1FFF): a per-object runtime function-pointer dispatch whose
        # target cannot be statically enumerated (SM enemy/PLM/eproj AI).
        # Codegen routes it through cpu_dispatch_call_pc. See decoder
        # runtime-pointer recovery + codegen._emit_runtime_dispatch.
        self.dispatch_runtime = False
        self.m_flag = 1
        self.x_flag = 1
        # Constant-Z branch fold: when set on a BEQ/BNE, the preceding
        # same-block immediate-LD* made Z statically known and the
        # branch was rewritten to an unconditional Goto with a single
        # live successor. dead_pc24 records the pruned edge for the
        # build report.
        self.const_z_fold_unconditional = False
        self.const_z_fold_dead_pc24 = None

    def __repr__(self):
        bank = (self.addr >> 16) & 0xFF
        pc = self.addr & 0xFFFF
        flags = f"[M={self.m_flag} X={self.x_flag}]"
        return f"${bank:02X}:{pc:04X} {flags} {self.mnem:<5} {self._fmt()}"

    def _fmt(self):
        m, v = self.mode, self.operand
        if m == IMP:      return ''
        if m == ACC:      return 'A'
        if m == IMM:
            if self.mnem in ('REP', 'SEP'):
                return f'#${v:02X}'
            return f'#${v:02X}' if v <= 0xFF else f'#${v:04X}'
        if m == DP:       return f'${v:02X}'
        if m == DP_X:     return f'${v:02X},X'
        if m == DP_Y:     return f'${v:02X},Y'
        if m == ABS:      return f'${v:04X}'
        if m == ABS_X:    return f'${v:04X},X'
        if m == ABS_Y:    return f'${v:04X},Y'
        if m == LONG:     return f'${v:06X}'
        if m == LONG_X:   return f'${v:06X},X'
        if m == REL:      return f'${v:04X}'
        if m == REL16:    return f'${v:04X}'
        if m == STK:      return f'${v:02X},S'
        if m == INDIR:    return f'(${v:04X})'
        if m == INDIR_X:  return f'(${v:04X},X)'
        if m == INDIR_Y:  return f'(${v:02X}),Y'
        if m == INDIR_LY: return f'[${v:02X}],Y'
        if m == INDIR_L:  return f'[${v:02X}]'
        if m == INDIR_DPX:return f'(${v:02X},X)'
        if m == DP_INDIR: return f'(${v:02X})'
        if m == STK_IY:   return f'${v:02X},S),Y'
        return f'${v:X}'


def _build_opcode_table() -> dict:
    fixed = [
        # Implied / accumulator
        (0xAA,'TAX',IMP,1),(0x8A,'TXA',IMP,1),(0xA8,'TAY',IMP,1),(0x98,'TYA',IMP,1),
        (0x9B,'TXY',IMP,1),(0xBB,'TYX',IMP,1),(0xBA,'TSX',IMP,1),(0x9A,'TXS',IMP,1),
        (0x5B,'TCD',IMP,1),(0x7B,'TDC',IMP,1),(0x1B,'TCS',IMP,1),(0x3B,'TSC',IMP,1),
        (0xDA,'PHX',IMP,1),(0xFA,'PLX',IMP,1),(0x5A,'PHY',IMP,1),(0x7A,'PLY',IMP,1),
        (0x48,'PHA',IMP,1),(0x68,'PLA',IMP,1),(0x08,'PHP',IMP,1),(0x28,'PLP',IMP,1),
        (0x8B,'PHB',IMP,1),(0xAB,'PLB',IMP,1),(0x0B,'PHD',IMP,1),(0x2B,'PLD',IMP,1),
        (0x4B,'PHK',IMP,1),
        (0xE8,'INX',IMP,1),(0xC8,'INY',IMP,1),(0xCA,'DEX',IMP,1),(0x88,'DEY',IMP,1),
        (0x1A,'INC',ACC,1),(0x3A,'DEC',ACC,1),
        (0x18,'CLC',IMP,1),(0x38,'SEC',IMP,1),(0x58,'CLI',IMP,1),(0x78,'SEI',IMP,1),
        (0xD8,'CLD',IMP,1),(0xF8,'SED',IMP,1),(0xB8,'CLV',IMP,1),
        (0xFB,'XCE',IMP,1),(0xEB,'XBA',IMP,1),
        (0x0A,'ASL',ACC,1),(0x4A,'LSR',ACC,1),(0x2A,'ROL',ACC,1),(0x6A,'ROR',ACC,1),
        (0x60,'RTS',IMP,1),(0x6B,'RTL',IMP,1),(0x40,'RTI',IMP,1),(0xEA,'NOP',IMP,1),
        (0xDB,'STP',IMP,1),(0xCB,'WAI',IMP,1),
        # Direct page (2 bytes)
        (0x64,'STZ',DP,2),(0x74,'STZ',DP_X,2),
        (0xA5,'LDA',DP,2),(0xB5,'LDA',DP_X,2),(0xB2,'LDA',DP_INDIR,2),(0xB1,'LDA',INDIR_Y,2),
        (0xA7,'LDA',INDIR_L,2),(0xB7,'LDA',INDIR_LY,2),
        (0x85,'STA',DP,2),(0x95,'STA',DP_X,2),(0x92,'STA',DP_INDIR,2),(0x91,'STA',INDIR_Y,2),
        (0x87,'STA',INDIR_L,2),(0x97,'STA',INDIR_LY,2),
        (0xA6,'LDX',DP,2),(0xB6,'LDX',DP_Y,2),
        (0xA4,'LDY',DP,2),(0xB4,'LDY',DP_X,2),
        (0x86,'STX',DP,2),(0x96,'STX',DP_Y,2),(0x84,'STY',DP,2),(0x94,'STY',DP_X,2),
        (0x25,'AND',DP,2),(0x35,'AND',DP_X,2),(0x21,'AND',INDIR_DPX,2),
        (0x27,'AND',INDIR_L,2),(0x37,'AND',INDIR_LY,2),
        (0x05,'ORA',DP,2),(0x15,'ORA',DP_X,2),(0x01,'ORA',INDIR_DPX,2),
        (0x07,'ORA',INDIR_L,2),(0x17,'ORA',INDIR_LY,2),
        (0x45,'EOR',DP,2),(0x55,'EOR',DP_X,2),(0x41,'EOR',INDIR_DPX,2),
        (0x47,'EOR',INDIR_L,2),(0x57,'EOR',INDIR_LY,2),
        (0x65,'ADC',DP,2),(0x75,'ADC',DP_X,2),(0x61,'ADC',INDIR_DPX,2),
        (0x67,'ADC',INDIR_L,2),(0x77,'ADC',INDIR_LY,2),
        (0xE5,'SBC',DP,2),(0xF5,'SBC',DP_X,2),(0xE1,'SBC',INDIR_DPX,2),
        (0xE7,'SBC',INDIR_L,2),(0xF7,'SBC',INDIR_LY,2),
        (0xC5,'CMP',DP,2),(0xD5,'CMP',DP_X,2),(0xC1,'CMP',INDIR_DPX,2),
        (0xC7,'CMP',INDIR_L,2),(0xD7,'CMP',INDIR_LY,2),
        (0xA1,'LDA',INDIR_DPX,2),(0x81,'STA',INDIR_DPX,2),
        # (dp) indirect
        (0x12,'ORA',DP_INDIR,2),(0x32,'AND',DP_INDIR,2),(0x52,'EOR',DP_INDIR,2),
        (0x72,'ADC',DP_INDIR,2),(0xD2,'CMP',DP_INDIR,2),(0xF2,'SBC',DP_INDIR,2),
        # (dp),Y
        (0x11,'ORA',INDIR_Y,2),(0x31,'AND',INDIR_Y,2),(0x51,'EOR',INDIR_Y,2),
        (0x71,'ADC',INDIR_Y,2),(0xD1,'CMP',INDIR_Y,2),(0xF1,'SBC',INDIR_Y,2),
        # (stk,S),Y and BRL
        (0x93,'STA',STK_IY,2),(0x13,'ORA',STK_IY,2),(0x33,'AND',STK_IY,2),
        (0x53,'EOR',STK_IY,2),(0x73,'ADC',STK_IY,2),(0xB3,'LDA',STK_IY,2),
        (0xD3,'CMP',STK_IY,2),(0xF3,'SBC',STK_IY,2),
        (0x82,'BRL',REL16,3),
        (0xC6,'DEC',DP,2),(0xD6,'DEC',DP_X,2),(0xE6,'INC',DP,2),(0xF6,'INC',DP_X,2),
        (0x26,'ROL',DP,2),(0x36,'ROL',DP_X,2),(0x66,'ROR',DP,2),(0x76,'ROR',DP_X,2),
        (0x06,'ASL',DP,2),(0x16,'ASL',DP_X,2),(0x46,'LSR',DP,2),(0x56,'LSR',DP_X,2),
        (0x24,'BIT',DP,2),(0x34,'BIT',DP_X,2),(0x04,'TSB',DP,2),(0x14,'TRB',DP,2),
        (0x03,'ORA',STK,2),(0x23,'AND',STK,2),(0x43,'EOR',STK,2),(0x63,'ADC',STK,2),
        (0x83,'STA',STK,2),(0xA3,'LDA',STK,2),(0xC3,'CMP',STK,2),(0xE3,'SBC',STK,2),
        (0xD4,'PEI',DP,2),(0xC2,'REP',IMM,2),(0xE2,'SEP',IMM,2),
        (0x00,'BRK',IMM,2),(0x02,'COP',IMM,2),(0x42,'WDM',IMM,2),
        (0x10,'BPL',REL,2),(0x30,'BMI',REL,2),(0xF0,'BEQ',REL,2),(0xD0,'BNE',REL,2),
        (0x90,'BCC',REL,2),(0xB0,'BCS',REL,2),(0x50,'BVC',REL,2),(0x70,'BVS',REL,2),
        (0x80,'BRA',REL,2),
        # Absolute (3 bytes)
        (0x9C,'STZ',ABS,3),(0x9E,'STZ',ABS_X,3),
        (0xAD,'LDA',ABS,3),(0xBD,'LDA',ABS_X,3),(0xB9,'LDA',ABS_Y,3),
        (0x8D,'STA',ABS,3),(0x9D,'STA',ABS_X,3),(0x99,'STA',ABS_Y,3),
        (0xAE,'LDX',ABS,3),(0xBE,'LDX',ABS_Y,3),
        (0xAC,'LDY',ABS,3),(0xBC,'LDY',ABS_X,3),
        (0x8E,'STX',ABS,3),(0x8C,'STY',ABS,3),
        (0xEC,'CPX',ABS,3),(0xE4,'CPX',DP,2),
        (0xCC,'CPY',ABS,3),(0xC4,'CPY',DP,2),
        (0x2D,'AND',ABS,3),(0x3D,'AND',ABS_X,3),(0x39,'AND',ABS_Y,3),
        (0x0D,'ORA',ABS,3),(0x1D,'ORA',ABS_X,3),(0x19,'ORA',ABS_Y,3),
        (0x4D,'EOR',ABS,3),(0x5D,'EOR',ABS_X,3),(0x59,'EOR',ABS_Y,3),
        (0x6D,'ADC',ABS,3),(0x7D,'ADC',ABS_X,3),(0x79,'ADC',ABS_Y,3),
        (0xED,'SBC',ABS,3),(0xFD,'SBC',ABS_X,3),(0xF9,'SBC',ABS_Y,3),
        (0xCD,'CMP',ABS,3),(0xDD,'CMP',ABS_X,3),(0xD9,'CMP',ABS_Y,3),
        (0xCE,'DEC',ABS,3),(0xDE,'DEC',ABS_X,3),(0xEE,'INC',ABS,3),(0xFE,'INC',ABS_X,3),
        (0x2E,'ROL',ABS,3),(0x3E,'ROL',ABS_X,3),(0x6E,'ROR',ABS,3),(0x7E,'ROR',ABS_X,3),
        (0x0E,'ASL',ABS,3),(0x1E,'ASL',ABS_X,3),(0x4E,'LSR',ABS,3),(0x5E,'LSR',ABS_X,3),
        (0x2C,'BIT',ABS,3),(0x3C,'BIT',ABS_X,3),(0x0C,'TSB',ABS,3),(0x1C,'TRB',ABS,3),
        (0x4C,'JMP',ABS,3),(0x6C,'JMP',INDIR,3),(0x7C,'JMP',INDIR_X,3),(0xDC,'JMP',INDIR,3),
        (0x20,'JSR',ABS,3),(0xFC,'JSR',INDIR_X,3),(0xF4,'PEA',ABS,3),
        (0x62,'PER',REL16,3),(0x44,'MVP',IMM,3),(0x54,'MVN',IMM,3),
        # Long (4 bytes)
        (0xAF,'LDA',LONG,4),(0xBF,'LDA',LONG_X,4),
        (0x8F,'STA',LONG,4),(0x9F,'STA',LONG_X,4),
        (0x0F,'ORA',LONG,4),(0x1F,'ORA',LONG_X,4),
        (0x2F,'AND',LONG,4),(0x3F,'AND',LONG_X,4),
        (0x4F,'EOR',LONG,4),(0x5F,'EOR',LONG_X,4),
        (0x6F,'ADC',LONG,4),(0x7F,'ADC',LONG_X,4),
        (0xCF,'CMP',LONG,4),(0xDF,'CMP',LONG_X,4),
        (0xEF,'SBC',LONG,4),(0xFF,'SBC',LONG_X,4),
        (0x5C,'JMP',LONG,4),(0x22,'JSL',LONG,4),
    ]

    m_dep = [(0xA9,'LDA'),(0x09,'ORA'),(0x29,'AND'),(0x49,'EOR'),
             (0x69,'ADC'),(0xE9,'SBC'),(0xC9,'CMP'),(0x89,'BIT')]
    x_dep = [(0xA2,'LDX'),(0xA0,'LDY'),(0xE0,'CPX'),(0xC0,'CPY')]

    table = {}
    for op, mn, mode, length in fixed:
        if op not in table:
            table[op] = (mn, mode, length)
    for op, mn in m_dep:
        table[op] = (mn, IMM, lambda m, x, _l=None: 2 if m else 3)
    for op, mn in x_dep:
        table[op] = (mn, IMM, lambda m, x, _l=None: 2 if x else 3)
    return table

_OPCODES = _build_opcode_table()


def opcode_table() -> dict:
    """Public accessor for the canonical opcode -> (mnem, mode, length) map.

    The value's third field (length) may be an int or a callable
    ``(m, x, _l=None) -> int`` for the (M,X)-width-dependent immediates.
    Cost modelling (``snes_cycles.py``) consumes only (mnem, mode); reusing
    this table keeps the cycle model tied to the single decoder rather than
    re-listing 256 opcodes.
    """
    return _OPCODES


def decode_insn(data: bytes, offset: int, pc: int, bank: int,
                m: int = 1, x: int = 1) -> Optional[Insn]:
    """Decode one 65816 instruction. Returns None on unknown opcode."""
    op = data[offset]
    if op not in _OPCODES:
        return None
    mnem, mode, len_spec = _OPCODES[op]
    length = len_spec(m, x) if callable(len_spec) else len_spec

    def b(n): return data[offset + n]
    def word(): return b(1) | (b(2) << 8)
    def long24(): return b(1) | (b(2) << 8) | (b(3) << 16)
    def rel8():
        v = b(1)
        delta = v - 256 if v >= 128 else v
        return (pc + 2 + delta) & 0xFFFF

    if   mode == REL:              operand = rel8()
    elif mode == REL16:
        raw = word()
        delta = raw - 0x10000 if raw >= 0x8000 else raw
        operand = (pc + 3 + delta) & 0xFFFF
    elif mode in (LONG, LONG_X):   operand = long24()
    elif mode in (ABS, ABS_X, ABS_Y, INDIR, INDIR_X): operand = word()
    elif mode in (DP, DP_X, DP_Y, STK, INDIR_Y, INDIR_LY, INDIR_L,
                  INDIR_DPX, DP_INDIR, STK_IY):  operand = b(1)
    elif mode == IMM:              operand = b(1) if length == 2 else word()
    else:                          operand = 0

    return Insn((bank << 16) | pc, op, mnem, mode, operand, length)


def validate_decoded_insns(insns: List[Insn], bank: int) -> bool:
    """Check if a sequence of decoded instructions looks like valid code.

    Returns True if the instructions look plausible, False if they look
    like data decoded as code (garbled JSL targets, nonsensical addresses).
    """
    for insn in insns:
        # JSL to invalid bank (SMW uses $00-$0D)
        if insn.mnem == 'JSL':
            tgt_bank = (insn.operand >> 16) & 0xFF
            if tgt_bank > 0x0D and tgt_bank not in (0x7E, 0x7F):
                return False
        # Long addressing (LDA/STA/etc long,X) to invalid bank
        if insn.mode in (LONG, LONG_X) and insn.mnem != 'JSL':
            addr_bank = (insn.operand >> 16) & 0xFF
            if addr_bank > 0x0D and addr_bank not in (0x7E, 0x7F):
                return False
        # JSR to address below $8000 (invalid ROM in LoROM for game code banks)
        if insn.mnem == 'JSR' and insn.operand < 0x0800:
            return False
        # BRK or COP in game code is almost certainly data
        if insn.mnem in ('BRK', 'COP'):
            return False
    return True
