from pathlib import Path
import sys
import tempfile


TOOLS = Path(__file__).resolve().parents[2] / "tools"
sys.path.insert(0, str(TOOLS))

from ingest_dkc2_disasm import (  # noqa: E402
    collect_animation_callback_contracts,
    collect_data_regions,
    collect_entries,
    collect_interaction_callback_contracts,
    collect_kong_cutscene_contracts,
    collect_rts_stack_dispatch_contracts,
    collect_indexed_record_dispatch_contracts,
    collect_sprite_state_contracts,
    collect_terrain_dispatch_contracts,
    emit_cfg,
)


def test_animation_command_83_is_tail_dispatch_without_synthetic_frame():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_B9.asm").write_text(
            """\
Callback81:
    RTS
Callback83:
    JMP process_anim_script
Callback84:
    RTS
Script:
    db !animation_command_81 : dw Callback81
    db !animation_command_83 : dw Callback83
    db !animation_command_84 : dw Callback84
animation_command_81:
    JMP (temp_26)
animation_command_83:
    JMP (temp_26)
process_sprite_animation:
    LDA sprite.animation_routine,x
    JMP (temp_26)
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
B9:D700 Callback81
B9:D710 Callback83
B9:D720 Callback84
b9:d186 0001:0000000c
b9:d1b2 0001:0000000e
b9:d14c 0001:00000011
""",
            encoding="utf-8",
        )
        contracts, _entries = collect_animation_callback_contracts(
            full, disasm)
        assert [(item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xD186, (0xB9D700,), "ptrcall"),
            (0xD1B2, (0xB9D710,), "ptrtail"),
            (0xD14C, (0xB9D720,), "ptrcall"),
        ]


def test_imports_first_field_of_sprite_main_records_for_both_dispatch_sites():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_B3.asm").write_text(
            """\
sprite_handler:
    JMP (sprite_main_table,x)
time_stop_sprite_handler:
    JMP (sprite_main_table,x)
sprite_main_table:
    %offset(sprite_time_stop_flags_table, 2)
    dw SpriteA,$0000
    dw SpriteB,$0001
NotPartOfTable:
    dw Unrelated,$0000
SpriteA:
    RTL
SpriteB:
    RTL
Unrelated:
    RTL
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
B3:8000 sprite_handler
B3:8003 time_stop_sprite_handler
B3:8010 sprite_main_table
B3:8020 NotPartOfTable
B3:8030 SpriteA
B3:8040 SpriteB
B3:8050 Unrelated
b3:8000 0001:00000002
b3:8003 0001:00000004
""",
            encoding="utf-8",
        )
        contracts, entries = collect_indexed_record_dispatch_contracts(
            full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xB3, 0x8000, (0xB38030, 0xB38040), "ptrtail"),
            (0xB3, 0x8003, (0xB38030, 0xB38040), "ptrtail"),
        ]
        assert {(item.pc24, item.name) for item in entries} == {
            (0xB38030, "SpriteA"),
            (0xB38040, "SpriteB"),
        }


def test_imports_first_field_of_kong_state_records_only():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_B8.asm").write_text(
            """\
kong_state_handler:
    JMP (kong_state_table,x)
kong_state_table:
    %offset(kong_state_flags_table, 2)
    dw StateA,$0000
if !version == 0
    dw StateB,$0048
else
    dw StateC,$0048
endif
    dw StateD,$0002
NotPartOfTable:
    dw Unrelated,$0000
StateA:
    RTS
StateB:
    RTS
StateC:
    RTS
StateD:
    RTS
Unrelated:
    RTS
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
B8:8000 kong_state_handler
B8:8010 kong_state_table
B8:8020 NotPartOfTable
B8:8030 StateA
B8:8040 StateB
B8:8050 StateC
B8:8060 StateD
B8:8070 Unrelated
b8:8000 0001:00000002
""",
            encoding="utf-8",
        )
        contracts, entries = collect_indexed_record_dispatch_contracts(
            full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xB8, 0x8000,
             (0xB88030, 0xB88040, 0xB88050, 0xB88060), "ptrtail")
        ]
        assert {(item.pc24, item.name) for item in entries} == {
            (0xB88030, "StateA"),
            (0xB88040, "StateB"),
            (0xB88050, "StateC"),
            (0xB88060, "StateD"),
        }


def _fixture(root: Path) -> tuple[Path, Path, Path]:
    disasm = root / "disasm"
    disasm.mkdir()
    (disasm / "bank_80.asm").write_text(
        """\
DirectRoutine:
    LDA #$00               ;$808000
    RTS                    ;$808002

HandlerTable:
    dw IndirectHandler, DataBlob

IndirectHandler:
    RTS                    ;$808010

LocalTable:
    dw .local_handler

.local_handler:
    RTS                    ;$808018

DataBlob:
    dw $1234

Computed_entry:
    JMP IndirectHandler    ;$808030
""",
        encoding="utf-8",
    )
    entries = root / "entries.sym"
    entries.write_text(
        "; entries\n[labels]\n80:8000 DirectRoutine\n", encoding="utf-8"
    )
    full = root / "full.sym"
    full.write_text(
        """\
[labels]
80:8000 DirectRoutine
80:8003 HandlerTable
80:8010 IndirectHandler
80:8018 LocalTable_local_handler
80:8020 DataBlob
80:8030 Computed_entry
""",
        encoding="utf-8",
    )
    return disasm, entries, full


def test_harvests_direct_table_and_entry_stub_but_not_data():
    with tempfile.TemporaryDirectory() as temp:
        disasm, direct, full = _fixture(Path(temp))
        entries = collect_entries(direct, full, disasm, include_indirect=True)
        got = {(entry.pc24, entry.name, entry.source) for entry in entries}
        assert got == {
            (0x808000, "DirectRoutine", "direct"),
            (0x808010, "IndirectHandler", "indirect"),
            (0x808018, "LocalTable_local_handler", "indirect"),
            (0x808030, "Computed_entry", "indirect"),
        }


def test_emits_next_entry_as_exclusive_function_boundary():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm, direct, full = _fixture(root)
        entries = collect_entries(direct, full, disasm, include_indirect=True)
        output = root / "cfg"
        emit_cfg(entries, output)
        bootstrap = (output / "bank00.cfg").read_text(encoding="utf-8")
        assert "auto_vectors" in bootstrap
        assert "tier_down_stubs" in bootstrap
        text = (output / "bank80.cfg").read_text(encoding="utf-8")
        assert "func DirectRoutine 8000 end:8010" in text
        assert "func DirectRoutine 8000 end:8010 entry_mx:0,0" in text
        assert "func IndirectHandler 8010 end:8018" in text
        assert "func LocalTable_local_handler 8018 end:8030" in text
        assert "func Computed_entry 8030 end:10000" in text


def test_direct_only_mode_excludes_table_targets():
    with tempfile.TemporaryDirectory() as temp:
        disasm, direct, full = _fixture(Path(temp))
        entries = collect_entries(direct, full, disasm, include_indirect=False)
        assert [(entry.pc24, entry.name) for entry in entries] == [
            (0x808000, "DirectRoutine")
        ]


def test_direct_entries_use_exact_full_symbol_address():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm, direct, full = _fixture(root)
        direct.write_text(
            "; stale address comment overlay\n[labels]\n"
            "80:8010 DirectRoutine\n",
            encoding="utf-8",
        )
        entries = collect_entries(direct, full, disasm, include_indirect=False)
        assert [(entry.pc24, entry.name, entry.source) for entry in entries] == [
            (0x808000, "DirectRoutine", "direct")
        ]


def test_direct_entries_resolve_unique_scoped_local_and_drop_absent_symbol():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm, direct, full = _fixture(root)
        direct.write_text(
            "[labels]\n80:8010 LocalRoutine\n80:8020 OtherVersionOnly\n",
            encoding="utf-8",
        )
        full.write_text(
            full.read_text(encoding="utf-8")
            + "80:8040 Parent_LocalRoutine\n",
            encoding="utf-8",
        )
        entries = collect_entries(direct, full, disasm, include_indirect=False)
        assert [(entry.pc24, entry.name) for entry in entries] == [
            (0x808040, "LocalRoutine")
        ]


def test_collects_exact_active_data_spans_from_asar_rows():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_80.asm").write_text(
            "Start:\n    BCC Continue\nTrapData:\n"
            "    db $00, $80, $FD\nContinue:\n    RTS\n",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            "[labels]\n80:8000 Start\n80:8002 TrapData\n80:8005 Continue\n"
            "80:8000 0001:00000002\n80:8002 0001:00000004\n"
            "80:8005 0001:00000006\n",
            encoding="utf-8",
        )
        assert collect_data_regions(full, disasm) == [
            (0x80, 0x8002, 0x8005)
        ]


def test_sprite_state_helper_imports_inline_table_target_union():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_B3.asm").write_text(
            """\
sprite_state_handler_B3:
    JMP ($0000,x)

SpriteOne:
    JSR sprite_state_handler_B3

.state_table:
    dw .idle, .move

.idle:
    RTS

.move:
    RTS
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
B3:A000 sprite_state_handler_B3
B3:A100 SpriteOne
B3:A110 SpriteOne_idle
B3:A120 SpriteOne_move
b3:a010 0001:00000002
b3:a102 0001:00000005
""",
            encoding="utf-8",
        )
        contracts, entries, terminal_jsrs = collect_sprite_state_contracts(
            full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xB3, 0xA010, (0xB3A110, 0xB3A120), "ptrtail_popcall")
        ]
        assert {(item.pc24, item.name) for item in entries} == {
            (0xB3A110, "SpriteOne_idle"),
            (0xB3A120, "SpriteOne_move"),
        }
        assert terminal_jsrs == [0xB3A102]

        output = root / "cfg"
        emit_cfg(entries, output, contracts, terminal_jsrs=terminal_jsrs)
        cfg_text = (output / "bankb3.cfg").read_text(encoding="utf-8")
        assert "terminal_jsr A102" in cfg_text


def test_recovers_pei_rts_decompressor_dispatch_as_internal_goto():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_BB.asm").write_text(
            """\
copy_or_return_1_entry:
    NOP
    JMP copy_or_return_1
stream_byte_1_entry:
    NOP
    JMP stream_byte_1
copy_or_return_2_entry:
    JMP copy_or_return_2
stream_byte_2_entry:
    NOP
    JMP stream_byte_2
execute_command_set_1:
    PEI ($4E)
    RTS
execute_command_set_2:
    PEI ($4A)
    RTS
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
bb:8000 0001:00000002
bb:8001 0001:00000003
bb:8004 0001:00000005
bb:8005 0001:00000006
bb:8040 0001:00000008
bb:8043 0001:0000000a
bb:8044 0001:0000000b
bb:8e0d 0001:0000000d
bb:8e0f 0001:0000000e
bb:8e27 0001:00000010
bb:8e29 0001:00000011
""",
            encoding="utf-8",
        )
        contracts = collect_rts_stack_dispatch_contracts(full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xBB, 0x8E0D, (0xBB8001, 0xBB8005), "rtsstack"),
            (0xBB, 0x8E27, (0xBB8040, 0xBB8044), "rtsstack"),
        ]


def test_imports_symbolic_terrain_table_and_override_as_ptrtail_contract():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_B5.asm").write_text(
            """\
TerrainA:
    RTL
TerrainB:
    RTL
DATA_B5BC00:
    dw TerrainA, TerrainB
SetupOverride:
    LDA #TerrainB
    STA $17B2
TerrainLookup:
    JMP ($17B2)
ShapeA:
    RTS
ShapeB:
    RTS
DATA_B5CA58:
    dw ShapeA, ShapeB
ShapeLookup:
    JMP ($00AA)
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
B5:9000 TerrainA
B5:9010 TerrainB
B5:9020 DATA_B5BC00
B5:9030 SetupOverride
B5:9040 TerrainLookup
B5:9050 ShapeA
B5:9060 ShapeB
B5:9070 DATA_B5CA58
B5:9080 ShapeLookup
b5:9040 0001:0000000b
b5:9080 0001:00000013
""",
            encoding="utf-8",
        )
        contracts, entries = collect_terrain_dispatch_contracts(full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xB5, 0x9040, (0xB59000, 0xB59010), "ptrtail"),
            (0xB5, 0x9080, (0xB59050, 0xB59060), "ptrtail"),
        ]
        assert {(item.pc24, item.name) for item in entries} == {
            (0xB59000, "TerrainA"),
            (0xB59010, "TerrainB"),
            (0xB59050, "ShapeA"),
            (0xB59060, "ShapeB"),
        }


def test_imports_only_complete_symbolic_interaction_callback_pairs():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_B3.asm").write_text(
            """\
CallbackA:
    RTL
SetupA:
    LDA #CallbackA
    STA interaction_RAM_0A8A
    LDA.w #CallbackA>>16
    STA interaction_RAM_0A8C
NotACallback:
    LDA $48,x
    STA interaction_RAM_0A8A
""",
            encoding="utf-8",
        )
        (disasm / "bank_B8.asm").write_text(
            """\
CODE_B8938A:
    JML [$0032]
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
B3:9000 CallbackA
B3:9010 SetupA
B3:9020 NotACallback
B8:8000 CODE_B8938A
b8:8000 0002:00000002
""",
            encoding="utf-8",
        )
        contracts, entries = collect_interaction_callback_contracts(
            full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xB8, 0x8000, (0xB39000,), "ptrtail")
        ]
        assert [(item.pc24, item.name) for item in entries] == [
            (0xB39000, "CallbackA")
        ]


def test_imports_second_field_of_kong_cutscene_records_only():
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        disasm = root / "disasm"
        disasm.mkdir()
        (disasm / "bank_BB.asm").write_text(
            """\
kong_cutscene_handler:
    STA $00
CODE_BBC174:
    JMP ($0002,x)
.script_table:
    dw .script_one
.script_one:
    dw $0001, .handler_one
    dw $0020, .handler_two
.handler_one:
    RTL
.handler_two:
    RTL
NextGlobal:
    dw $0000, Unrelated
Unrelated:
    RTL
""",
            encoding="utf-8",
        )
        full = root / "full.sym"
        full.write_text(
            """\
[labels]
BB:8000 kong_cutscene_handler
BB:8003 CODE_BBC174
BB:8010 CODE_BBC174_script_one
BB:8020 CODE_BBC174_handler_one
BB:8030 CODE_BBC174_handler_two
BB:8040 NextGlobal
BB:8050 Unrelated
bb:8003 0001:00000004
""",
            encoding="utf-8",
        )
        contracts, entries = collect_kong_cutscene_contracts(full, disasm)
        assert [(item.bank, item.site_pc16, item.targets, item.mode)
                for item in contracts] == [
            (0xBB, 0x8003, (0xBB8020, 0xBB8030), "ptrtail")
        ]
        assert {(item.pc24, item.name) for item in entries} == {
            (0xBB8020, "CODE_BBC174_handler_one"),
            (0xBB8030, "CODE_BBC174_handler_two"),
        }
