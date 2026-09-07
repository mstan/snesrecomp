param(
    [string]$Gcc = "C:\msys64\mingw64\bin\gcc.exe",
    [string]$CMake = "",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if ($OutDir -eq "") {
    $OutDir = Join-Path $root "build\audio-trace-config-tests"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

if (-not (Test-Path -LiteralPath $Gcc -PathType Leaf)) {
    throw "MinGW gcc not found at '$Gcc'"
}
if ($CMake -eq "") {
    $cmakeCandidates = @(
        "C:\msys64\mingw64\bin\cmake.exe",
        "C:\Users\Matthew\.local\share\retcomm\toolchains\cmake-clang-v1\latest\bin\cmake.exe",
        "C:\Users\Matthew\AppData\Local\retcomm\toolchains\cmake-clang-v1\latest\bin\cmake.exe",
        "C:\Program Files\CMake\bin\cmake.exe"
    )
    foreach ($candidate in $cmakeCandidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $CMake = $candidate
            break
        }
    }
    if ($CMake -eq "") {
        $CMake = (Get-Command cmake).Source
    }
}
if (-not (Test-Path -LiteralPath $CMake -PathType Leaf)) {
    throw "cmake not found at '$CMake'"
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$runnerSrc = Join-Path $root "runner\src"
$configTestC = Join-Path $root "tests\audio\audio_trace_history_config_test.c"
$cmakeSrc = Join-Path $root "tests\audio\cmake_audio_trace_history"

function Invoke-Native {
    param(
        [Parameter(Mandatory=$true)][string]$Exe,
        [string[]]$Args = @()
    )
    & $Exe @Args
    if ($LASTEXITCODE -ne 0) {
        throw "$Exe failed with exit code $LASTEXITCODE"
    }
}

function Invoke-DirectCase {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][int]$Expected,
        [string[]]$Defines = @()
    )
    $out = Join-Path $OutDir ("audio_trace_history_config_{0}.exe" -f $Name)
    Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
    $compileArgs = @(
        "-std=c11", "-Wall", "-Wextra", "-Werror",
        "-DEXPECT_AUDIO_TRACE_HISTORY=$Expected",
        "-I", $runnerSrc
    ) + $Defines + @($configTestC, "-o", $out)
    Invoke-Native $Gcc $compileArgs
    Invoke-Native $out @()
}

function Invoke-CMakeCase {
    param(
        [Parameter(Mandatory=$true)][string]$Name,
        [Parameter(Mandatory=$true)][int]$Expected,
        [string[]]$Args = @()
    )
    $build = Join-Path $OutDir ("cmake_{0}" -f $Name)
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $cmakeArgs = @(
        "-S", $cmakeSrc,
        "-B", $build,
        "-G", "Ninja",
        "-DEXPECT_AUDIO_TRACE_HISTORY=$Expected",
        "-DSNESRECOMP_ROOT=$root"
    ) + $Args
    Invoke-Native $CMake $cmakeArgs
}

function Invoke-CMakeInvalidCase {
    $build = Join-Path $OutDir "cmake_invalid"
    New-Item -ItemType Directory -Force -Path $build | Out-Null
    $invalidOut = Join-Path $OutDir "cmake_invalid.stdout.log"
    $invalidErr = Join-Path $OutDir "cmake_invalid.stderr.log"
    Remove-Item -LiteralPath $invalidOut, $invalidErr -Force -ErrorAction SilentlyContinue
    $cmakeArgs = @(
        "-S", $cmakeSrc,
        "-B", $build,
        "-G", "Ninja",
        "-DEXPECT_AUDIO_TRACE_HISTORY=0",
        "-DSNESRECOMP_ROOT=$root",
        "-DSNESRECOMP_AUDIO_TRACE_HISTORY=INVALID"
    )
    $oldErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $CMake @cmakeArgs > $invalidOut 2> $invalidErr
        $cmakeExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorAction
    }
    if ($cmakeExit -eq 0) {
        throw "invalid CMake audio trace history unexpectedly configured"
    }
    $invalidText = ((Get-Content -LiteralPath $invalidOut -Raw -ErrorAction SilentlyContinue) + "`n" +
                    (Get-Content -LiteralPath $invalidErr -Raw -ErrorAction SilentlyContinue))
    if ($invalidText -notmatch "SNESRECOMP_AUDIO_TRACE_HISTORY" -or
        $invalidText -notmatch "INVALID") {
        throw "invalid CMake audio trace history failed for an unexpected reason"
    }
}

Invoke-DirectCase "default" 1
Invoke-DirectCase "trace0_default" 1 @("-DSNESRECOMP_TRACE=0")
Invoke-DirectCase "explicit_counters" 0 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=0")
Invoke-DirectCase "explicit_small" 1 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=1")
Invoke-DirectCase "explicit_full" 2 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=2")
Invoke-DirectCase "explicit_reserved" 3 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=3")
Invoke-DirectCase "trace1_forces_full" 2 @(
    "-DSNESRECOMP_TRACE=1",
    "-DSNESRECOMP_AUDIO_TRACE_HISTORY=0")
Invoke-DirectCase "cosim_trace0_forces_full" 2 @(
    "-DSNES_COSIM=1",
    "-DSNESRECOMP_TRACE=0",
    "-DSNESRECOMP_AUDIO_TRACE_HISTORY=1")

Invoke-CMakeCase "default" 1
Invoke-CMakeCase "explicit_counters" 0 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=COUNTERS")
Invoke-CMakeCase "explicit_small" 1 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=SMALL")
Invoke-CMakeCase "explicit_full" 2 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=FULL")
Invoke-CMakeCase "explicit_reserved" 3 @("-DSNESRECOMP_AUDIO_TRACE_HISTORY=RESERVED")
Invoke-CMakeCase "trace_forces_full" 2 @(
    "-DSNESRECOMP_ENABLE_TRACE=ON",
    "-DSNESRECOMP_AUDIO_TRACE_HISTORY=COUNTERS")
Invoke-CMakeCase "cosim_forces_full" 2 @(
    "-DSNES_COSIM=ON",
    "-DSNESRECOMP_AUDIO_TRACE_HISTORY=SMALL")
Invoke-CMakeInvalidCase

Write-Host "audio_trace config matrix passed"
