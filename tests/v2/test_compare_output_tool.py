import pathlib
import subprocess
import sys


REPO = pathlib.Path(__file__).resolve().parents[2]
TOOL = REPO / "tools" / "v2_compare_output.py"


def _run(expected, actual):
    return subprocess.run([
        sys.executable, str(TOOL),
        "--expected", str(expected), "--actual", str(actual),
    ], text=True, capture_output=True)


def test_compare_output_accepts_identical_complete_trees(tmp_path):
    expected = tmp_path / "expected"
    actual = tmp_path / "actual"
    expected.mkdir()
    actual.mkdir()
    (expected / "bank00_v2.c").write_bytes(b"same\n")
    (actual / "bank00_v2.c").write_bytes(b"same\n")

    result = _run(expected, actual)

    assert result.returncode == 0, result.stdout + result.stderr
    assert "1 files byte-identical" in result.stdout


def test_compare_output_reports_content_and_file_set_drift(tmp_path):
    expected = tmp_path / "expected"
    actual = tmp_path / "actual"
    expected.mkdir()
    actual.mkdir()
    (expected / "bank00_v2.c").write_bytes(b"old\n")
    (actual / "bank00_v2.c").write_bytes(b"new\n")
    (expected / "program_manifest.json").write_bytes(b"{}\n")
    (actual / "unexpected.c").write_bytes(b"x\n")

    result = _run(expected, actual)

    assert result.returncode == 1
    assert "DRIFT: bank00_v2.c" in result.stdout
    assert "MISSING: program_manifest.json" in result.stdout
    assert "UNEXPECTED: unexpected.c" in result.stdout
