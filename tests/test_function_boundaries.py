"""
Structural invariants for key functions. These tests encode specific bug
classes uncovered during the mode-02/03 advancement fix so they cannot
regress silently.

Each test reads the current generated .c files (assumes regen already
happened — run `regen.sh` or the recomp.py driver first).

Bug classes covered:

1. Natural fall-through over a non-terminal last instruction (GameMode03Entry
   at $96AE → $96CF). Symptom pre-fix: the decoder flood-filled past end_addr
   through several downstream functions and emitted Mode04Finish's RTS tail
   inline, suppressing the needed fall-through call to the next function.

2. Past-start backward branch into the previous function's RTS
   (GameModeXX_FadeInOrOut at $9F6F branches BPL → $9F6E RTS). Symptom
   pre-fix: the RTS at $9F6E was emitted in-line in decode order, ahead
   of the fall-through call, making the fall-through dead code.

3. Dispatch table overread (DisplayOwPrompt at $04F3E5 has an 8-entry
   jsl_dispatch table; the decoder used to read a 9th bogus entry
   pointing to $A822, which left `label_a822` undefined at link time).

4. Bank 05 cfg typo (ExecutePtrLong registered as `jsl_dispatch` instead
   of `jsl_dispatch_long`, producing 16-bit entries read off a 24-bit
   table — `label_8d05` at link time).
"""
import pathlib
import re

GEN_DIR = pathlib.Path('F:/Projects/SuperMarioWorldRecomp/src/gen')


def _read(bank: str) -> str:
    p = GEN_DIR / f'smw_{bank}_gen.c'
    assert p.exists(), f'gen file missing: {p}'
    return p.read_text()


def _extract_function(src: str, name: str) -> str:
    """Return the body of the named function (from '{' through matching '}')."""
    # Locate a line that starts a function definition for `name`:
    #   <ret_type> <name>(<params>) { ...
    sig_re = re.compile(
        rf'^(?:\w[\w ]*)\s+{re.escape(name)}\s*\([^)]*\)\s*\{{',
        re.MULTILINE,
    )
    m = sig_re.search(src)
    assert m, f'function {name} not found'
    # Brace-match from the opening {.
    i = m.end() - 1
    depth = 0
    start = i
    while i < len(src):
        c = src[i]
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return src[start:i + 1]
        i += 1
    raise AssertionError(f'unterminated function body for {name}')


# ---------------------------------------------------------------------------
# Bug class 1: natural fall-through, GameMode03Entry → 0096CF
# ---------------------------------------------------------------------------

def test_gamemode03entry_tail_calls_0096CF():
    body = _extract_function(_read('00'), 'GameMode11_LoadSublevel_GameMode03Entry')
    assert 'GameMode11_LoadSublevel_0096CF(' in body, (
        'GameMode03Entry must tail-call GameMode11_LoadSublevel_0096CF '
        '(fall-through emit). Without it, mode 03 never progresses past '
        'the Nintendo-Presents → title-screen transition.'
    )


def test_gamemode03entry_no_bleed_through_mode04finish_tail():
    """Pre-fix, the decoder flood-filled past end:96cf into Mode04Finish
    and emitted `WriteReg(0x4200, 0x81)` (LDA #$81; STA HW_NMITIMEN) at
    the end of GameMode03Entry's body. That is Mode04Finish's tail and
    has no business being inline here."""
    body = _extract_function(_read('00'), 'GameMode11_LoadSublevel_GameMode03Entry')
    assert 'WriteReg(0x4200, 0x81)' not in body and 'WriteReg(0x4200, v7)' not in body, (
        'GameMode03Entry body contains Mode04Finish tail (STA $4200 with '
        'value 0x81) — decoder over-ran end_addr via _continuing_past_end.'
    )


def test_0096CF_tail_calls_0096D5():
    body = _extract_function(_read('00'), 'GameMode11_LoadSublevel_0096CF')
    assert 'GameMode11_LoadSublevel_0096D5(' in body, (
        '0096CF must tail-call 0096D5 (natural fall-through).'
    )


# ---------------------------------------------------------------------------
# Bug class 2: past-start backward branch, GameModeXX_FadeInOrOut
# ---------------------------------------------------------------------------

def test_gamemode_fade_tail_calls_009F77():
    body = _extract_function(_read('00'), 'GameModeXX_FadeInOrOut')
    assert 'GameModeXX_FadeInOrOut_009F77(' in body, (
        'GameModeXX_FadeInOrOut must tail-call _009F77 (natural '
        'fall-through). Without it, brightness never updates and mode 02 '
        '(fade-out to title screen) never advances to mode 03.'
    )


def test_gamemode_fade_fallthrough_is_reachable():
    """The fall-through call to _009F77 must be reachable, not dead code.

    Pre-known_func_starts-fix, the decoder inlined the shared RTS at $9F6E
    into this function, emitting a `label_9f6e:;` stub and a trailing
    `return;` that left the fall-through as dead code below. The
    known_func_starts filter now stops that inlining entirely, so the body
    ends cleanly with the fall-through call as its final statement.

    Check: the call exists, and no unconditional `return;` sits between it
    and the final closing brace (i.e. it's on the normal exit path).
    """
    body = _extract_function(_read('00'), 'GameModeXX_FadeInOrOut')
    flat = ' '.join(body.split())
    i_call = flat.find('GameModeXX_FadeInOrOut_009F77(')
    assert i_call != -1, 'fall-through call missing'
    # Everything after the call up to the closing brace of the function.
    tail = flat[i_call:]
    # No stray `label_9f6e:;` — that stub indicates the old inlining bug.
    assert 'label_9f6e:;' not in body, (
        'label_9f6e stub is back — decoder is inlining past-end function '
        'entries again (known_func_starts filter regressed).'
    )


# ---------------------------------------------------------------------------
# Bug class 3/4: undefined label from dispatch over-read or cfg typo
# ---------------------------------------------------------------------------

def test_fallthrough_emits_recomp_stack_pop():
    """Fall-through tail-calls must pop the caller's recomp-stack frame
    before calling the next function. Without the pop, the caller's frame
    leaks onto the stack as a phantom (seen on HandleSPCUploads_*MusicBank
    during the mode-03 investigation)."""
    src = _read('00')
    body = _extract_function(src, 'HandleSPCUploads_UploadOverworldMusicBank')
    flat = ' '.join(body.split())
    i_pop = flat.find('RecompStackPop()')
    i_call = flat.find('HandleSPCUploads_StrtSPCMscUpld(')
    assert i_pop != -1, 'fall-through function missing RecompStackPop()'
    assert i_call != -1, 'fall-through call missing'
    assert i_pop < i_call, (
        f'RecompStackPop at {i_pop} must precede the fall-through call '
        f'at {i_call} (tail-call semantics; otherwise the caller frame '
        f'lingers on the recomp stack).'
    )


def test_cross_function_branch_label_after_body():
    """Regression for a family of bugs that all manifest as the body of
    ChocIsld2_Layer1Handler being dead code after the tail call to
    ChocIsld2_Shared_LoadPtrs. Two distinct issues have reached this
    function:

    1. Past-start guard comparing a 16-bit pc16 against a 24-bit
       bank-encoded start — deferred the entire body to _past_start_buf
       and emitted the fall-through before RecompStackPush.
    2. decode_func using inclusive end semantics (pc > end instead of
       pc >= end) — over-decoded the first instruction of
       ChocIsld2_Shared_LoadPtrs into this function, producing a spurious
       label_db49 and turning the fall-through into a past-end emit.

    Current correct shape: BEQ $DB49 targets the next function, so the
    conditional becomes a conditional tail call; the fall-through after
    the false branch becomes an unconditional tail call. There should be
    NO label_db49 inside Layer1Handler's body, and the body must load
    $1422 before either call to Shared_LoadPtrs."""
    body = _extract_function(_read('05'), 'ChocIsld2_Layer1Handler')
    flat = ' '.join(body.split())
    i_push = flat.find('RecompStackPush("ChocIsld2_Layer1Handler")')
    i_body = flat.find('g_ram[0x1422]')
    m_fall = re.search(r'ChocIsld2_Shared_LoadPtrs\s*\(', flat)
    i_fall = m_fall.start() if m_fall else -1
    assert i_push != -1 and i_body != -1 and i_fall != -1, (
        'markers missing in ChocIsld2_Layer1Handler body'
    )
    assert i_push < i_body < i_fall, (
        f'ChocIsld2_Layer1Handler body order broken: push={i_push} '
        f'body={i_body} fall={i_fall}. Expected push < body < '
        f'tail-call-to-Shared_LoadPtrs.'
    )
    assert 'label_db49' not in flat, (
        'ChocIsld2_Layer1Handler should not contain label_db49: $DB49 '
        'is the start of the NEXT function (ChocIsld2_Shared_LoadPtrs), '
        'so the BEQ should become a conditional tail call, not an '
        'intra-function branch to a spurious label.'
    )
    # No `return;` between push and the first body load.
    prelude = flat[i_push:i_body]
    assert 'return;' not in prelude, (
        'ChocIsld2_Layer1Handler has a return; between RecompStackPush '
        'and the body — the fall-through stanza is being emitted before '
        'the real body.'
    )


def test_no_undefined_goto_labels():
    """Every `goto label_XXXX` must have a matching `label_XXXX:;` somewhere
    in the same generated file. An undefined goto target is a link-time
    error."""
    def _check(bank: str):
        src = _read(bank)
        gotos = set(re.findall(r'goto\s+(label_[0-9a-f]{4,8})\s*;', src))
        defs = set(re.findall(r'(label_[0-9a-f]{4,8})\s*:\s*;', src))
        missing = sorted(gotos - defs)
        assert not missing, f'bank {bank}: undefined goto targets: {missing[:5]}'
    for b in ('00', '01', '02', '03', '04', '05', '07', '0c', '0d'):
        _check(b)
