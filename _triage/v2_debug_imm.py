import sys, pathlib
REPO = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'recompiler'))
sys.path.insert(0, str(REPO / 'tests/v2'))
from snes65816 import decode_insn
from v2 import lowering
from v2.ir import Value, ConstI

def vf():
    c = [0]
    def alloc():
        c[0] += 1
        return Value(vid=c[0])
    return alloc

rom = bytearray(0x8000)
rom[0:3] = bytes([0xA9, 0x34, 0x12])
insn1 = decode_insn(bytes(rom), 0, 0x8000, 0, m=1, x=1)
ops1 = lowering.lower(insn1, value_factory=vf())
print('M=1 ops:', ops1)
print('M=1 op types:', [type(o).__module__ + '.' + type(o).__name__ for o in ops1])
print('ConstI module:', ConstI.__module__)
print('any ConstI?', any(isinstance(o, ConstI) for o in ops1))
