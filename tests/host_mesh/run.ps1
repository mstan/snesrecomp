$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\host_mesh_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
    "-I$root\runner\src",
    "$root\tests\host_mesh\host_mesh_test.c",
    "$root\runner\src\host_mesh.c",
    "$root\runner\src\host_mesh_builder.c",
    "-o", $out
)
New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0) { throw "host mesh test build failed" }
& $out
if ($LASTEXITCODE -ne 0) { throw "host mesh tests failed" }
