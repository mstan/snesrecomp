param(
    [switch]$Bench,
    [int]$BenchLoops = 100
)

$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$buildDir = Join-Path $root "build"
$gccCommand = Get-Command gcc.exe -ErrorAction Stop
$gcc = $gccCommand.Source

function Build-CompositionRegressionTest {
    param(
        [string]$OutputPath,
        [string]$PpuSourcePath
    )

    $compileArgs = @(
        "-std=c11", "-Wall", "-Wextra", "-O2",
        "-DSNESRECOMP_REVERSE_DEBUG=0",
        "-I$root\runner\src", "-I$root\runner\src\snes",
        "$root\tests\ppu\ppu_composition_regression_test.c",
        "$root\runner\src\snes\ppu_legacy.c",
        "-o", $OutputPath
    )
    if ($PpuSourcePath) {
        $includePath = (Resolve-Path -LiteralPath $PpuSourcePath).Path.Replace('\', '/')
        $compileArgs = @("-DPPU_COMPOSITION_PPU_INCLUDE=\`"$includePath\`"") + $compileArgs
    }

    Remove-Item -LiteralPath $OutputPath -Force -ErrorAction SilentlyContinue
    $self = Get-Process -Id $PID
    $oldPriority = $self.PriorityClass
    try {
        $self.PriorityClass = "BelowNormal"
        & $gcc @compileArgs
        $exitCode = $LASTEXITCODE
    } finally {
        try { $self.PriorityClass = $oldPriority } catch { }
    }
    if ($exitCode -ne 0) {
        throw "gcc failed for $OutputPath with exit code $exitCode"
    }
    if (-not (Test-Path -LiteralPath $OutputPath)) {
        throw "gcc did not produce $OutputPath"
    }
}

New-Item -ItemType Directory -Force $buildDir | Out-Null
$candidateOut = Join-Path $buildDir "ppu_composition_candidate_test.exe"
$headDir = Join-Path $buildDir "ppu_composition_head"
$headSource = Join-Path $headDir "ppu_head.c"
$headOut = Join-Path $buildDir "ppu_composition_head_test.exe"

New-Item -ItemType Directory -Force $headDir | Out-Null
& git -C $root show HEAD:runner/src/snes/ppu.c | Set-Content -LiteralPath $headSource
if ($LASTEXITCODE -ne 0) { throw "failed to export HEAD ppu.c" }

Build-CompositionRegressionTest -OutputPath $headOut -PpuSourcePath $headSource
Build-CompositionRegressionTest -OutputPath $candidateOut

$headDigest = (& $headOut --digest | Select-Object -Last 1)
if ($LASTEXITCODE -ne 0) { throw "HEAD digest run failed" }
$candidateDigest = (& $candidateOut --digest | Select-Object -Last 1)
if ($LASTEXITCODE -ne 0) { throw "candidate digest run failed" }
if ($headDigest -ne $candidateDigest) {
    throw "digest mismatch: HEAD=$headDigest candidate=$candidateDigest"
}

& $candidateOut
if ($LASTEXITCODE -ne 0) { throw "composition regression test failed" }

if ($Bench) {
    $self = Get-Process -Id $PID
    $oldPriority = $self.PriorityClass
    try {
        $self.PriorityClass = "BelowNormal"
        for ($i = 1; $i -le 5; $i++) {
            Write-Host "bench_pair=$i"
            & $headOut --render-bench $BenchLoops
            if ($LASTEXITCODE -ne 0) { throw "HEAD render benchmark failed" }
            & $candidateOut --render-bench $BenchLoops
            if ($LASTEXITCODE -ne 0) { throw "candidate render benchmark failed" }
        }
    } finally {
        try { $self.PriorityClass = $oldPriority } catch { }
    }
}
