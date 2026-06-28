# Build + run the 65816 codegen-vs-interp816 differential harness (Axis 1).
# mingw gcc on Windows (the Bash sandbox blocks native compiles). From anywhere.
$ErrorActionPreference = "Stop"
$wt = (Resolve-Path "$PSScriptRoot\..\..").Path
$gcc = "C:\msys64\mingw64\bin\gcc.exe"

Write-Host "=== regen single-opcode functions ==="
python "$wt\tests\cpu_diff\gen_ops.py"

New-Item -ItemType Directory -Force "$wt\build" | Out-Null
Write-Host "=== build ==="
& $gcc -std=c11 -O1 -w -I "$wt\runner\src" -I "$wt\runner\src\snes" `
    "$wt\tests\cpu_diff\cpu_diff.c" "$wt\tests\cpu_diff\gen_ops.c" `
    "$wt\runner\src\snes\interp816.c" -o "$wt\build\cpu_diff.exe"
if ($LASTEXITCODE -ne 0) { throw "build failed" }

Write-Host "=== run ==="
& "$wt\build\cpu_diff.exe"
