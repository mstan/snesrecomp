"""Whole-program M/X demand must constrain first-pass emission."""

import pathlib
import re
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]


def test_static_m1x1_call_does_not_manufacture_three_dead_variants():
    with tempfile.TemporaryDirectory() as raw:
        root = pathlib.Path(raw)
        rom = bytearray([0xFF] * 0x8000)
        rom[0:4] = bytes([
            0x20, 0x10, 0x80,  # $8000: JSR $8010
            0x60,              # $8003: RTS
        ])
        rom[0x10] = 0x60       # $8010: RTS
        rom_path = root / 'game.sfc'
        rom_path.write_bytes(rom)
        cfg_dir = root / 'cfg'
        cfg_dir.mkdir()
        (cfg_dir / 'bank00.cfg').write_text(
            'bank = 00\n'
            'func Caller 8000 end:8004\n'
            'func Callee 8010 end:8011\n',
            encoding='utf-8')
        out_dir = root / 'gen'

        result = subprocess.run([
            sys.executable, str(REPO / 'tools' / 'v2_regen.py'),
            '--rom', str(rom_path), '--cfg-dir', str(cfg_dir),
            '--out-dir', str(out_dir), '--jobs', '1',
        ], capture_output=True, text=True)
        assert result.returncode == 0, result.stdout + result.stderr

        source = (out_dir / 'bank00_v2.c').read_text(encoding='utf-8')
        callee_variants = set(re.findall(
            r'\bCallee_M([01])X([01])\b', source))
        assert callee_variants == {('1', '1')}, callee_variants
        assert 'auto-promote pass 1: added 3 entries' not in result.stdout
        assert 'emit_pass_1' not in result.stdout
