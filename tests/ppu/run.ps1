$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\ppu_sprite_limit_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-O1",
    "-DSNESRECOMP_REVERSE_DEBUG=0",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\ppu\ppu_sprite_limit_test.c",
    "$root\runner\src\snes\ppu.c",
    "-o", $out
)

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
$proc = Start-Process -FilePath $gcc -ArgumentList $args -PassThru -NoNewWindow
$proc.PriorityClass = "BelowNormal"
$proc.WaitForExit()
if (-not (Test-Path -LiteralPath $out)) {
    throw "gcc did not produce the sprite-limit test executable"
}
& $out
if ($LASTEXITCODE -ne 0) { throw "sprite-limit test failed" }
