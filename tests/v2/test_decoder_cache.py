"""Decoder-cache correctness across immutable analysis snapshots."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.decoder import (  # noqa: E402
    clear_decode_cache,
    decode_cache_stats,
    decode_function,
    set_decode_cache_enabled,
)


def _rom():
    return make_lorom_bank0({
        0x8000: bytes([
            0x20, 0x00, 0x90,  # JSR $9000
            0xA9, 0x12,        # LDA #$12 when callee exits M=1
            0x60,              # RTS
        ]),
        0x9000: bytes([0x60]),
    })


def _with_cache(fn):
    set_decode_cache_enabled(True)
    clear_decode_cache()
    try:
        fn()
    finally:
        set_decode_cache_enabled(False)


def test_identical_snapshot_hits_cache():
    def run():
        rom = _rom()
        snapshot = {(0x009000, 1, 1): (1, 1)}
        first = decode_function(
            rom, 0, 0x8000, 1, 1, callee_exit_mx=snapshot)
        second = decode_function(
            rom, 0, 0x8000, 1, 1, callee_exit_mx=snapshot)
        stats = decode_cache_stats()
        assert first is second
        assert stats['misses'] == 1 and stats['hits'] == 1
    _with_cache(run)


def test_equal_but_distinct_snapshots_do_not_alias():
    def run():
        rom = _rom()
        one = {(0x009000, 1, 1): (1, 1)}
        two = dict(one)
        first = decode_function(rom, 0, 0x8000, 1, 1,
                                callee_exit_mx=one)
        second = decode_function(rom, 0, 0x8000, 1, 1,
                                 callee_exit_mx=two)
        stats = decode_cache_stats()
        assert first is not second
        assert stats['misses'] == 2 and stats['hits'] == 0
    _with_cache(run)


def test_phase_clear_allows_mutated_facts_to_redecode():
    def run():
        rom = _rom()
        facts = {(0x009000, 1, 1): (1, 1)}
        first = decode_function(rom, 0, 0x8000, 1, 1,
                                callee_exit_mx=facts)
        first_post = [di for k, di in first.insns.items()
                      if (k.pc & 0xFFFF) == 0x8003]
        assert first_post and first_post[0].insn.length == 2

        facts[(0x009000, 1, 1)] = (0, 1)
        clear_decode_cache()
        second = decode_function(rom, 0, 0x8000, 1, 1,
                                 callee_exit_mx=facts)
        second_post = [di for k, di in second.insns.items()
                       if (k.pc & 0xFFFF) == 0x8003]
        assert second_post and second_post[0].insn.length == 3
    _with_cache(run)
