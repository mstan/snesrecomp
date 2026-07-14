$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\known_lle_entry_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\known_lle_entry_test.c",
    "$root\runner\src\cpu_state.c",
    "-Wl,--gc-sections", "-o", $out
)

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $gcc -ArgumentList $args -PassThru -NoNewWindow
$proc.PriorityClass = "BelowNormal"
$proc.WaitForExit()
if (-not (Test-Path -LiteralPath $out)) { throw "gcc did not produce the test executable" }
& $out
if ($LASTEXITCODE -ne 0) { throw "dispatch contract test failed" }
