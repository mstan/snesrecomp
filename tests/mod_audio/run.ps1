$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\mod_audio_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-I$root\runner\src",
    "$root\tests\mod_audio\mod_audio_test.c",
    "$root\runner\src\mod_audio.c",
    "-o", $out
)
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "mod audio test build failed" }
& $out
if ($LASTEXITCODE -ne 0) { throw "mod audio tests failed" }
