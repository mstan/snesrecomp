"""Regression coverage for hle_spc_upload's Option-1 return ABI."""

from _helpers import make_lorom_bank0  # noqa: E402

from v2.emit_function import emit_function  # noqa: E402


def test_hle_spc_upload_emits_rts_frame_pop_before_normal_return():
    rom = make_lorom_bank0({
        0x8059: bytes([0x60]),  # body is HLE-replaced; terminator is RTS.
    })

    src = emit_function(rom, bank=0, start=0x8059,
                        entry_m=0, entry_x=0,
                        func_name='SpcUpload',
                        hle_spc_upload=[0x8059])

    assert 'RtlUploadSpcImageFromDp(cpu)' in src, src
    assert 'uint16 _entry_s = cpu->S;' in src, src
    assert 'uint8 _hrv = cpu->host_return_valid;' in src, src
    assert 'HLE SPC upload RTS pop hardware return frame' in src, src
    assert 'uint8 _rpb = cpu->PB;' in src, src
    assert 'dbg_rts_trace(cpu, 0x008059u, _entry_s, _ret_s, _rpc24, (uint8)_hrv);' in src, src
    assert 'return RECOMP_RETURN_NORMAL;  /* HLE RTS host return */' in src, src
    assert 'cpu_dispatch_pc_from(cpu, _rpc24, (uint16)(_entry_s + 2u), 0x008059u)' in src, src

    assert src.index('HLE SPC upload RTS pop hardware return frame') < src.index(
        'return RECOMP_RETURN_NORMAL;  /* HLE RTS host return */')
