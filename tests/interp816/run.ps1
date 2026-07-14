$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$gcc = (Get-Command gcc).Source
$outDir = Join-Path $root "build"
New-Item -ItemType Directory -Force $outDir | Out-Null

function Build-BelowNormal([string[]] $CompileArgs, [string] $Output) {
    Remove-Item -LiteralPath $Output -Force -ErrorAction SilentlyContinue
    $proc = Start-Process -FilePath $gcc -ArgumentList $CompileArgs `
        -PassThru -NoNewWindow
    $proc.PriorityClass = "BelowNormal"
    $proc.WaitForExit()
    if (-not (Test-Path -LiteralPath $Output)) {
        throw "gcc did not produce $Output"
    }
}

$coreOut = Join-Path $outDir "interp816_test.exe"
Build-BelowNormal @(
    "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-O1",
    "-I$root\runner\src\snes",
    "$root\tests\interp816\interp816_test.c",
    "$root\runner\src\snes\interp816.c", "-o", $coreOut
) $coreOut
& $coreOut
if ($LASTEXITCODE -ne 0) { throw "interp816 core test failed" }

$bridgeOut = Join-Path $outDir "bridge_test.exe"
Build-BelowNormal @(
    "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter", "-O1",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\interp816\bridge_test.c",
    "$root\runner\src\snes\interp816.c",
    "$root\runner\src\snes\interp_bridge.c", "-o", $bridgeOut
) $bridgeOut
& $bridgeOut
if ($LASTEXITCODE -ne 0) { throw "interp bridge test failed" }
