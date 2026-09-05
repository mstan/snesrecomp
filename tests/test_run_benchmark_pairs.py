import importlib.util
import math
import os
import pathlib
import subprocess
import sys
import tempfile
from argparse import Namespace


ROOT = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "tools" / "run_benchmark_pairs.py"
SAME_PATH_SCRIPT = ROOT / "tools" / "run_same_path_benchmark_control.py"
PREP_OBJECT_SCRIPT = ROOT / "tools" / "prepare_object_substitution_variants.py"


def load_module():
    spec = importlib.util.spec_from_file_location("run_benchmark_pairs", SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def load_same_path_module():
    spec = importlib.util.spec_from_file_location("run_same_path_benchmark_control",
                                                  SAME_PATH_SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    sys_path_inserted = False
    tools_dir = str(ROOT / "tools")
    if tools_dir not in sys.path:
        sys.path.insert(0, tools_dir)
        sys_path_inserted = True
    try:
        spec.loader.exec_module(mod)
    finally:
        if sys_path_inserted:
            sys.path.remove(tools_dir)
    return mod


def load_prepare_object_module():
    spec = importlib.util.spec_from_file_location(
        "prepare_object_substitution_variants", PREP_OBJECT_SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(mod)
    return mod


def test_parse_benchmark_stdout_validates_positive_fps():
    mod = load_module()
    rec = mod.parse_benchmark_stdout(
        'noise\nSNESRECOMP_BENCHMARK '
        '{"frames":120,"seconds":0.5,"fps":240.0}\n')
    assert rec["frames"] == 120
    try:
        mod.parse_benchmark_stdout(
            'SNESRECOMP_BENCHMARK {"frames":120,"seconds":0.0,"fps":0.0}\n')
    except ValueError as exc:
        assert "non-positive" in str(exc)
    else:
        raise AssertionError("non-positive benchmark accepted")

    try:
        mod.parse_benchmark_stdout(
            'SNESRECOMP_BENCHMARK {"frames":119,"seconds":0.5,"fps":238.0}\n',
            expected_frames=120)
    except ValueError as exc:
        assert "frame mismatch" in str(exc)
    else:
        raise AssertionError("frame mismatch accepted")

    try:
        mod.parse_benchmark_stdout(
            'SNESRECOMP_BENCHMARK {"frames":119.5,"seconds":0.5,"fps":239.0}\n')
    except ValueError as exc:
        assert "frame count" in str(exc)
    else:
        raise AssertionError("fractional frame count accepted")


def test_run_one_passes_timeout_and_side(monkeypatch=None):
    mod = load_module()
    calls = []
    old_run = mod.subprocess.run

    def fake_run(cmd, cwd, text, timeout, stdout, stderr):
        calls.append((cmd, cwd, text, timeout, stdout, stderr))
        return subprocess.CompletedProcess(
            cmd, 0,
            stdout='SNESRECOMP_BENCHMARK '
                   '{"frames":60,"seconds":0.25,"fps":240.0}\n',
            stderr="")

    mod.subprocess.run = fake_run
    try:
        rec = mod.run_one("candidate", pathlib.Path("game.exe"),
                          pathlib.Path("game.sfc"), 60, False,
                          pathlib.Path("."), 3.5, "B")
    finally:
        mod.subprocess.run = old_run

    assert rec["side"] == "B"
    assert rec["name"] == "candidate"
    assert rec["cwd"] == "."
    assert rec["command"][1] == "--benchmark"
    assert rec["raw_stdout"].startswith("SNESRECOMP_BENCHMARK")
    assert rec["inputs_after"]["cwd"]["config.ini"]["exists"] is False
    assert rec["reports_after"]["cwd"]["meta"]["exists"] is False
    assert rec["reports_after"]["cwd"]["freshness"] == "missing"
    assert calls[0][0][1] == "--benchmark"
    assert calls[0][3] == 3.5


def test_run_one_captures_renderer_and_report():
    mod = load_module()
    old_run = mod.subprocess.run
    with tempfile.TemporaryDirectory() as td:
        tmp_path = pathlib.Path(td)
        report = tmp_path / "last_run_report.json"
        (tmp_path / "config.ini").write_text("NewRenderer = 1\n",
                                             encoding="utf-8")

        def fake_run(cmd, cwd, text, timeout, stdout, stderr):
            assert cwd == str(tmp_path)
            report.write_text(
                '{"status":{"frame":60},"wram":{"GameMode_0100":[1]},'
                '"sdl":{"compiled":"3"},"build":{"game":"test"},'
                '"reason":"atexit",'
                '"breadcrumbs":{"events":[{"msg":"config parsed: output=0"},'
                '{"msg":"first frame simulated"}]}}',
                encoding="utf-8")
            return subprocess.CompletedProcess(
                cmd, 0,
                stdout='SNESRECOMP_BENCHMARK '
                       '{"frames":60,"seconds":0.25,"fps":240.0}\n',
                stderr='SNESRECOMP_BENCHMARK_RENDERER name=direct3d11 vsync=0\n'
                       '[host] config parsed: output=0\n')

        mod.subprocess.run = fake_run
        try:
            rec = mod.run_one("candidate", tmp_path / "game.exe",
                              tmp_path / "game.sfc", 60, False,
                              tmp_path, 3.5, "A")
        finally:
            mod.subprocess.run = old_run

    assert rec["benchmark_renderer"]["name"] == "direct3d11"
    assert rec["benchmark_renderer"]["vsync"] == 0
    assert rec["reports_after"]["cwd"]["status"]["frame"] == 60
    assert rec["reports_after"]["cwd"]["freshness"] == "fresh"
    assert rec["reports_after"]["exe_dir"]["status"]["frame"] == 60
    assert rec["inputs_after"]["cwd"]["config.ini"]["exists"] is True
    assert "config parsed" in rec["host_breadcrumbs"][0]


def test_run_one_timeout_raises_system_exit():
    mod = load_module()
    old_run = mod.subprocess.run

    def fake_timeout(cmd, cwd, text, timeout, stdout, stderr):
        raise subprocess.TimeoutExpired(cmd, timeout)

    mod.subprocess.run = fake_timeout
    try:
        try:
            mod.run_one("candidate", pathlib.Path("game.exe"),
                        pathlib.Path("game.sfc"), 60, True,
                        pathlib.Path("."), 1.0, "A")
        except SystemExit as exc:
            assert "timed out" in str(exc)
            assert "--benchmark-audio" in str(exc)
        else:
            raise AssertionError("timeout did not raise SystemExit")
    finally:
        mod.subprocess.run = old_run


def test_same_path_stage_refuses_unmarked_nonempty_dir():
    mod = load_same_path_module()
    with tempfile.TemporaryDirectory() as td:
        stage = pathlib.Path(td)
        (stage / "not-owned.txt").write_text("x", encoding="utf-8")
        try:
            mod.ensure_stage_dir(stage)
        except SystemExit as exc:
            assert "lacks" in str(exc)
        else:
            raise AssertionError("unmarked nonempty stage dir accepted")


def test_same_path_copy_payload_uses_marked_stage_dir():
    mod = load_same_path_module()
    with tempfile.TemporaryDirectory() as source_td, tempfile.TemporaryDirectory() as stage_td:
        source = pathlib.Path(source_td)
        stage = pathlib.Path(stage_td)
        exe = source / "game.exe"
        exe.write_bytes(b"exe")
        (source / "SDL3.dll").write_bytes(b"dll")
        (source / "config.ini").write_text("NewRenderer = 1\n",
                                           encoding="utf-8")
        (source / "assets").mkdir()
        (source / "assets" / "asset.txt").write_text("asset", encoding="utf-8")
        manifest = mod.copy_payload(exe, stage, "stable.exe")

        assert (stage / mod.MARKER).exists()
        assert (stage / "stable.exe").read_bytes() == b"exe"
        assert (stage / "SDL3.dll").read_bytes() == b"dll"
        assert (stage / "config.ini").exists()
        assert (stage / "assets" / "asset.txt").exists()
        assert manifest["source_exe"]["sha256"]

        (stage / "transient.txt").write_text("remove", encoding="utf-8")
        mod.copy_payload(exe, stage, "stable.exe")
        assert not (stage / "transient.txt").exists()


def test_same_path_rejects_non_plain_stage_exe_name():
    mod = load_same_path_module()
    for name in ("..\\game.exe", "../game.exe", "subdir/game.exe", ".."):
        try:
            mod.validate_stage_exe_name(name)
        except SystemExit as exc:
            assert "plain filename" in str(exc)
        else:
            raise AssertionError(f"unsafe stage exe name accepted: {name}")


def test_same_path_validate_args_rejects_overlap_and_bad_timeout():
    mod = load_same_path_module()
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        src = root / "src"
        stage = src / "stage"
        src.mkdir()
        exe = src / "game.exe"
        rom = root / "game.sfc"
        exe.write_bytes(b"exe")
        rom.write_bytes(b"rom")
        args = Namespace(
            frames=60, pairs=1, warmups=0, timeout=math.inf,
            a_name="A", b_name="B", stage_exe_name="game.exe",
            stage_dir=stage, a_exe=exe, b_exe=exe,
            a_rom=rom, b_rom=rom)
        try:
            mod.validate_args(args)
        except SystemExit as exc:
            assert "timeout" in str(exc)
        else:
            raise AssertionError("non-finite timeout accepted")

        args.timeout = 10.0
        try:
            mod.validate_args(args)
        except SystemExit as exc:
            assert "overlaps" in str(exc)
        else:
            raise AssertionError("overlapping stage/source accepted")


def test_same_path_rejects_stage_child_symlink_when_supported():
    mod = load_same_path_module()
    with tempfile.TemporaryDirectory() as td:
        stage = pathlib.Path(td) / "stage"
        stage.mkdir()
        (stage / mod.MARKER).write_text("owned\n", encoding="utf-8")
        target = pathlib.Path(td) / "target"
        target.mkdir()
        link = stage / "linked"
        try:
            os.symlink(target, link, target_is_directory=True)
        except (OSError, NotImplementedError):
            return
        try:
            mod.clear_stage_dir(stage)
        except SystemExit as exc:
            assert "symlink" in str(exc) or "reparse" in str(exc)
        else:
            raise AssertionError("stage child symlink accepted")


def test_object_variant_refuses_overlapping_output_root():
    mod = load_prepare_object_module()
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        current = root / "current"
        baseline = root / "baseline"
        current.mkdir()
        baseline.mkdir()
        args = Namespace(current_build=current, baseline_build=baseline,
                         out_root=current / "variants")
        try:
            mod.validate_args(args)
        except SystemExit as exc:
            assert "overlaps" in str(exc)
        else:
            raise AssertionError("overlapping output root accepted")


def test_object_variant_owned_directory_clear_rejects_symlink_when_supported():
    mod = load_prepare_object_module()
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / "out"
        out.mkdir()
        (out / mod.MARKER).write_text("owned\n", encoding="utf-8")
        target = pathlib.Path(td) / "target"
        target.mkdir()
        link = out / "linked"
        try:
            os.symlink(target, link, target_is_directory=True)
        except (OSError, NotImplementedError):
            return
        try:
            mod.clear_owned_directory(out)
        except SystemExit as exc:
            assert "symlink" in str(exc) or "reparse" in str(exc)
        else:
            raise AssertionError("object variant symlink child accepted")


def test_object_variant_prepare_dry_run_manifest():
    mod = load_prepare_object_module()
    with tempfile.TemporaryDirectory() as td:
        root = pathlib.Path(td)
        current = root / "current"
        baseline = root / "baseline"
        out_root = root / "out"
        target = "Game.exe"
        cur_obj_dir = current / "CMakeFiles" / "Game.dir" / "runner" / "src"
        base_obj_dir = baseline / "CMakeFiles" / "Game.dir" / "runner" / "src"
        cur_obj_dir.mkdir(parents=True)
        base_obj_dir.mkdir(parents=True)
        (current / "CMakeCache.txt").write_text(
            "CMAKE_CXX_COMPILER:STRING=C:/tool/g++.exe\n", encoding="utf-8")
        (current / "build.ninja").write_text(
            "build Game.exe: RULE CMakeFiles/Game.dir/runner/src/common_cpu_infra.c.obj "
            "CMakeFiles/Game.dir/runner/src/common_rtl.c.obj\n"
            "  FLAGS = -O3 -DNDEBUG\n"
            "  LINK_LIBRARIES = -lm C:/libs/libSDL3.dll.a\n",
            encoding="utf-8")
        for name in ("common_cpu_infra.c.obj", "common_rtl.c.obj"):
            (cur_obj_dir / name).write_bytes(b"current-" + name.encode())
            (base_obj_dir / name).write_bytes(b"baseline-" + name.encode())
        (current / "config.ini").write_text("NewRenderer = 1\n",
                                            encoding="utf-8")
        args = Namespace(title="test", target=target, current_build=current,
                         baseline_build=baseline, out_root=out_root,
                         execute=False)
        args.current_build = args.current_build.resolve()
        args.baseline_build = args.baseline_build.resolve()
        args.out_root = args.out_root.resolve()
        edge = mod.parse_link_edge(args.current_build, args.target)
        manifest = mod.prepare_variant(
            args, "orig_common_cpu_infra",
            mod.DEFAULT_VARIANTS["orig_common_cpu_infra"], edge,
            mod.collect_objects(args.current_build, args.target),
            mod.collect_objects(args.baseline_build, args.target))
        variant_dir = out_root / "test" / "orig_common_cpu_infra"

        assert manifest["prepared_only"] is True
        assert (variant_dir / "objects.rsp").exists()
        assert (variant_dir / "libs.rsp").exists()
        assert (variant_dir / "relink.ps1").exists()
        assert "runner/src/common_cpu_infra.c.obj" in manifest["substitutions"]
        assert (variant_dir / "config.ini").exists()
