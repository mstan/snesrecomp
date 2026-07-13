"""Transactional generated-output publication tests."""

import pathlib
import subprocess
import sys
import tempfile

REPO = pathlib.Path(__file__).resolve().parents[2]
if str(REPO / 'tools') not in sys.path:
    sys.path.insert(0, str(REPO / 'tools'))

import v2_regen  # noqa: E402


def _run_minimal_regen(root: pathlib.Path, opcode: int):
    rom = bytearray([0xFF] * 0x8000)
    rom[0] = opcode
    rom_path = root / 'game.sfc'
    rom_path.write_bytes(rom)
    cfg_dir = root / 'cfg'
    cfg_dir.mkdir()
    (cfg_dir / 'bank00.cfg').write_text(
        'bank = 00\nfunc Entry 8000 end:8001\n', encoding='utf-8')
    out_dir = root / 'gen'
    result = subprocess.run([
        sys.executable, str(REPO / 'tools' / 'v2_regen.py'),
        '--rom', str(rom_path), '--cfg-dir', str(cfg_dir),
        '--out-dir', str(out_dir), '--jobs', '1',
    ], capture_output=True, text=True)
    return result, out_dir


def test_failed_workspace_cannot_modify_live_hardlink():
    with tempfile.TemporaryDirectory() as raw:
        target = pathlib.Path(raw) / 'gen'
        target.mkdir()
        live = target / 'bank00_v2.c'
        live.write_text('old\n', encoding='utf-8')

        workspace = v2_regen._AtomicOutputDir(target)
        staged = workspace.staging / live.name
        assert v2_regen.write_if_changed(staged, 'new\n')
        assert live.read_text(encoding='utf-8') == 'old\n'

        workspace.cleanup()
        assert target.exists()
        assert live.read_text(encoding='utf-8') == 'old\n'


def test_publish_swaps_complete_tree_and_preserves_unchanged_file():
    with tempfile.TemporaryDirectory() as raw:
        target = pathlib.Path(raw) / 'gen'
        target.mkdir()
        changed = target / 'bank00_v2.c'
        unchanged = target / 'bank01_v2.c'
        changed.write_text('old\n', encoding='utf-8')
        unchanged.write_text('same\n', encoding='utf-8')
        unchanged_mtime = unchanged.stat().st_mtime_ns

        workspace = v2_regen._AtomicOutputDir(target)
        assert v2_regen.write_if_changed(
            workspace.staging / changed.name, 'new\n')
        assert not v2_regen.write_if_changed(
            workspace.staging / unchanged.name, 'same\n')
        workspace.publish()

        assert (target / changed.name).read_text(encoding='utf-8') == 'new\n'
        assert (target / unchanged.name).read_text(encoding='utf-8') == 'same\n'
        assert (target / unchanged.name).stat().st_mtime_ns == unchanged_mtime
        assert not workspace.previous.exists()


def test_next_workspace_recovers_interrupted_directory_swap():
    with tempfile.TemporaryDirectory() as raw:
        target = pathlib.Path(raw) / 'gen'
        target.mkdir()
        (target / 'bank00_v2.c').write_text('complete\n', encoding='utf-8')
        previous = target.parent / '.gen.snesrecomp-previous'
        target.replace(previous)
        assert not target.exists() and previous.exists()

        workspace = v2_regen._AtomicOutputDir(target)
        try:
            assert target.exists()
            assert (target / 'bank00_v2.c').read_text(
                encoding='utf-8') == 'complete\n'
            assert not previous.exists()
        finally:
            workspace.cleanup()


def test_minimal_regen_publishes_successful_generation():
    with tempfile.TemporaryDirectory() as raw:
        result, out_dir = _run_minimal_regen(pathlib.Path(raw), 0x60)  # RTS
        assert result.returncode == 0, result.stdout + result.stderr
        assert (out_dir / 'bank00_v2.c').exists()
        assert (out_dir / 'dispatch_v2.c').exists()
        assert 'atomically published generated output' in result.stdout


def test_failed_regen_leaves_previous_generation_untouched():
    with tempfile.TemporaryDirectory() as raw:
        root = pathlib.Path(raw)
        out_dir = root / 'gen'
        out_dir.mkdir()
        live = out_dir / 'bank00_v2.c'
        live.write_text('known-good\n', encoding='utf-8')

        result, _ = _run_minimal_regen(root, 0x00)  # BRK -> stub lint
        assert result.returncode != 0
        assert live.read_text(encoding='utf-8') == 'known-good\n'
