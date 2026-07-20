$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\apu_port_guest_time_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-Wno-error=unknown-pragmas", "-Wno-error=comment",
    "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\apu_port_guest_time_test.c",
    "$root\runner\src\snes\apu.c",
    "$root\runner\src\snes\spc.c",
    "$root\runner\src\snes\dsp.c",
    "-Wl,--gc-sections", "-o", $out
)

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $out)) {
    throw "gcc did not produce the APU guest-time test executable"
}
& $out
if ($LASTEXITCODE -ne 0) { throw "APU guest-time test failed" }
