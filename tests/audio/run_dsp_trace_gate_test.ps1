$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$build = Join-Path $root "build\audio"
$gcc = (Get-Command gcc).Source
New-Item -ItemType Directory -Force $build | Out-Null

$baseArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-function",
    "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\audio\dsp_trace_gate_test.c",
    "$root\runner\src\snes\dsp.c",
    "$root\runner\src\snes\dsp_shadow.c",
    "$root\runner\src\snes\audio_shadow.c",
    "$root\runner\src\audio_trace.c",
    "-Wl,--gc-sections", "-lm"
)

$variants = @(
    @{ Name = "trace_undefined"; Defines = @() },
    @{ Name = "trace_0"; Defines = @("-DSNESRECOMP_TRACE=0") },
    @{ Name = "trace_1"; Defines = @("-DSNESRECOMP_TRACE=1") },
    @{ Name = "cosim"; Defines = @("-DSNES_COSIM=1", "-DSNESRECOMP_TRACE=0") }
)

foreach ($variant in $variants) {
    $out = Join-Path $build ("dsp_trace_gate_test_{0}.exe" -f $variant.Name)
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    & $gcc @($variant.Defines + $baseArgs + @("-o", $out))
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $out)) {
        throw "gcc did not produce $($variant.Name) DSP trace-gate test"
    }
    $stderr = Join-Path $build ("dsp_trace_gate_test_{0}.stderr.log" -f $variant.Name)
    Remove-Item -LiteralPath $stderr -Force -ErrorAction SilentlyContinue
    $oldErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $out 2> $stderr
        $dspExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorAction
    }
    if ($dspExit -ne 0) {
        throw "DSP trace-gate test failed for $($variant.Name)"
    }
}
