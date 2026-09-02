$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$gcc = (Get-Command gcc).Source
$tests = @("ppu_sprite_limit_test", "ppu_world_mirror_test", "ppu_elastic_band_test")
$failed = @()

foreach ($test in $tests) {
    $out = Join-Path $root "build\$test.exe"
    $args = @(
        "-std=c11", "-Wall", "-Wextra", "-O1",
        "-DSNESRECOMP_REVERSE_DEBUG=0",
        "-I$root\runner\src", "-I$root\runner\src\snes",
        "$root\tests\ppu\$test.c",
        "$root\runner\src\snes\ppu.c",
        "$root\runner\src\snes\ppu_legacy.c",
        "-o", $out
    )

    New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $gcc -ArgumentList $args -PassThru -NoNewWindow
    $proc.PriorityClass = "BelowNormal"
    $proc.WaitForExit()
    if (-not (Test-Path -LiteralPath $out)) {
        throw "gcc did not produce $test.exe"
    }
    & $out
    if ($LASTEXITCODE -ne 0) { $failed += $test }
}

if ($failed.Count -gt 0) { throw ("PPU tests failed: " + ($failed -join ", ")) }
