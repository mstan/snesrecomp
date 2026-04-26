"""Sig completeness: every callsite in src/gen/*_gen.c must pass the
arg count its callee declares in funcs.h.

Catches the C2198 bug class (we fought 50+ of these on the dispatch-
extent-multipass branch — a thunk emits a no-arg call to a callee
declared with one param). Compile catches them too, but only after
building; this check fires from the framework test suite, so the
loop is much tighter.

Coverage scope: only the simple-arity check (callsite emits N args,
funcs.h declares M params, M != N). The C compiler still owns the
type-check (uint8 vs uint16 etc.). False-positive control: skip
callsites whose argument list contains nested parens or commas
inside argument expressions — those are too complex to count
without a real C parser, and the compiler's own check catches them
anyway.

Skipped when src/gen or recomp/funcs.h aren't present.
"""
import importlib.util
import pathlib
import re
import sys


def _paths():
    framework_root = pathlib.Path(__file__).absolute().parent.parent
    game_root = framework_root.parent
    funcs_h = game_root / 'recomp' / 'funcs.h'
    gen_dir = game_root / 'src' / 'gen'
    if not (funcs_h.exists() and gen_dir.exists()):
        return None
    sys.path.insert(0, str(framework_root / 'recompiler'))
    return funcs_h, gen_dir


def _count_top_level_args(arg_str: str) -> int:
    """Count comma-separated args at paren-depth 0. Returns -1 if the
    arg string is too complex to parse (nested function calls, etc.)
    so the caller can skip rather than report a false positive."""
    if not arg_str.strip():
        return 0
    depth = 0
    commas = 0
    for c in arg_str:
        if c == '(':
            depth += 1
        elif c == ')':
            if depth == 0:
                return -1  # unbalanced
            depth -= 1
        elif c == ',' and depth == 0:
            commas += 1
    if depth != 0:
        return -1
    return commas + 1


def _param_count_from_sig(sig: str) -> int:
    """parse_funcs_h emits 'void()' or 'void(uint8_k,uint8_j)'."""
    m = re.match(r'^\w[\w*]*\(([^)]*)\)$', sig)
    if not m:
        return -1
    inner = m.group(1).strip()
    if not inner or inner == 'void':
        return 0
    return inner.count(',') + 1


# Callsite pattern: identifier followed by `(` at the start of a line
# or after whitespace/operator. Skip declarations (`void foo(...);`)
# by requiring NO `void`/`uint*`/etc. type token immediately before
# the function name on the same line.
_CALL_RE = re.compile(
    r'(?<![\w])([A-Za-z_]\w*)\s*\(([^;{]*?)\)\s*[;,)]'
)
_DECL_TYPE_RE = re.compile(
    r'\b(?:void|uint8|uint16|int8|int16|bool|RetA?XY?Y?|RetY|PairU16|FuncU8\w*)\b\s*[*&]?\s*$'
)


def test_callsite_arity_matches_funcs_h():
    paths = _paths()
    if paths is None:
        return
    funcs_h, gen_dir = paths
    import recomp  # noqa: E402
    sig_map = recomp.parse_funcs_h(str(funcs_h))
    # Pre-compute param counts. Skip funcs whose sig we can't parse
    # cleanly (e.g. struct returns); those still get C-compiler-checked.
    param_counts = {
        name: _param_count_from_sig(sig)
        for name, sig in sig_map.items()
    }
    param_counts = {n: c for n, c in param_counts.items() if c >= 0}

    failures = []
    skip_complex = 0
    for gen_file in sorted(gen_dir.glob('smw_*_gen.c')):
        text = gen_file.read_text(encoding='utf-8', errors='replace')
        # Scan line by line so we know which line a mismatch lives on
        # and so we can skip declaration lines cheaply.
        for lineno, line in enumerate(text.splitlines(), start=1):
            # Skip pure declaration lines (`void foo(uint8 k);`).
            stripped = line.strip()
            if stripped.startswith(('void ', 'uint8 ', 'uint16 ',
                                    'int8 ', 'int16 ', 'bool ',
                                    'RetY ', 'RetAY ', 'RetAXY ',
                                    'PairU16 ', 'static ',
                                    'extern ')):
                continue
            for m in _CALL_RE.finditer(line):
                fname = m.group(1)
                args = m.group(2)
                if fname not in param_counts:
                    continue
                expected = param_counts[fname]
                actual = _count_top_level_args(args)
                if actual < 0:
                    skip_complex += 1
                    continue
                if actual != expected:
                    failures.append(
                        f'{gen_file.name}:{lineno} {fname}('
                        f'{args[:60]}{"..." if len(args)>60 else ""}'
                        f') passes {actual} args, sig declares {expected}'
                    )
    if failures:
        msg = (
            f'Callsite arity mismatches against funcs.h '
            f'({len(failures)} sites, {skip_complex} complex sites '
            f'skipped):\n  '
            + '\n  '.join(failures[:30])
        )
        if len(failures) > 30:
            msg += f'\n  ... ({len(failures) - 30} more)'
        assert False, msg
