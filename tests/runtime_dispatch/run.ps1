$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\known_lle_entry_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\known_lle_entry_test.c",
    "$root\runner\src\cpu_state.c",
    "$root\runner\src\snes\cart.c",
    "$root\runner\src\snes\cx4.c",
    "$root\runner\src\snes\dsp1.c",
    "$root\runner\src\snes\dsp1_hle.c",
    "$root\runner\src\snes\sa1.c",
    "$root\runner\src\snes\sdd1.c",
    "$root\runner\src\snes\interp816.c",
    "-Wl,--gc-sections", "-lm", "-o", $out
)

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
$err = Join-Path $root "build\known_lle_entry_test.gcc.stderr.log"
Remove-Item -LiteralPath $err -Force -ErrorAction SilentlyContinue
$oldErrorAction = $ErrorActionPreference
$ErrorActionPreference = "Continue"
try {
    & $gcc @args 2> $err
    $gccExit = $LASTEXITCODE
} finally {
    $ErrorActionPreference = $oldErrorAction
}
if ($gccExit -ne 0 -or -not (Test-Path -LiteralPath $out)) {
    throw "gcc did not produce the test executable"
}
& $out
if ($LASTEXITCODE -ne 0) { throw "dispatch contract test failed" }
