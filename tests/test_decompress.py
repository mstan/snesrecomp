"""
Regression gate: GFX decompression must remain bit-exact vs the Python LZ2
reference for all 32 files. Native Python reimplementation of sweep_decompress.sh
to avoid the WSL/git-bash drive-mount ambiguity (subprocess bash is WSL,
interactive bash is git-bash, and they mount drives differently).

The decompress harness exercises the three LZ2 decompression functions
extracted verbatim from smw_00_gen.c. Any decoder fix that corrupts
65816 M/X state, branch width, or instruction selection tends to break
bit-exactness here long before it breaks the full game runtime.
"""
import filecmp
import importlib.util
import pathlib
import shutil
import subprocess
import sys
import tempfile

_HARNESS = pathlib.Path(
    'F:/Projects/SuperMarioWorldRecomp/test/decompress_harness/harness.exe'
)
_ROM = pathlib.Path('F:/Projects/SuperMarioWorldRecomp/smw.sfc')
_REF_PY = pathlib.Path('F:/Projects/SuperMarioWorldRecomp/lz2_ref.py')


def _load_ref_module():
    spec = importlib.util.spec_from_file_location('lz2_ref', _REF_PY)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_decompress_32_files_bit_exact():
    assert _HARNESS.exists(), f'harness.exe missing: {_HARNESS}'
    assert _ROM.exists(), f'ROM missing: {_ROM}'
    assert _REF_PY.exists(), f'lz2 reference missing: {_REF_PY}'

    failures = []
    with tempfile.TemporaryDirectory() as td:
        tdp = pathlib.Path(td)
        for j in range(32):
            jh = f'{j:02x}'
            ref_out = tdp / f'ref_{jh}.bin'
            rec_out = tdp / f'rec_{jh}.bin'

            # Reference via Python port.
            r = subprocess.run(
                [sys.executable, str(_REF_PY), str(_ROM), jh, str(ref_out)],
                capture_output=True, text=True, timeout=30,
            )
            if r.returncode != 0 or not ref_out.exists():
                failures.append(f'gfx {jh}: reference failed (rc={r.returncode}): {r.stderr.strip()[:100]}')
                continue
            ref_sz = ref_out.stat().st_size

            # Harness (recomp'd C).
            r = subprocess.run(
                [str(_HARNESS), str(_ROM), jh, str(rec_out)],
                capture_output=True, text=True, timeout=10,
            )
            if r.returncode != 0 or not rec_out.exists():
                failures.append(f'gfx {jh}: harness failed (rc={r.returncode})')
                continue

            # Trim rec to ref_sz and cmp.
            rec_bytes = rec_out.read_bytes()[:ref_sz]
            ref_bytes = ref_out.read_bytes()
            if rec_bytes != ref_bytes:
                first_diff = next(
                    (i for i in range(min(len(rec_bytes), len(ref_bytes)))
                     if rec_bytes[i] != ref_bytes[i]),
                    min(len(rec_bytes), len(ref_bytes)),
                )
                failures.append(
                    f'gfx {jh}: first diff at byte {first_diff} '
                    f'(ref={ref_bytes[first_diff]:02x} rec={rec_bytes[first_diff]:02x})'
                )

    assert not failures, (
        f'{len(failures)}/32 gfx files diverged from Python LZ2 reference:\n  ' +
        '\n  '.join(failures[:5])
    )
