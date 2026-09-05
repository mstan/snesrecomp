param(
    [string]$Gcc = "C:\msys64\mingw64\bin\gcc.exe",
    [string]$OutDir = ""
)

$ErrorActionPreference = "Stop"

$root = Resolve-Path (Join-Path $PSScriptRoot "..\..")
if ($OutDir -eq "") {
    $OutDir = Join-Path $root "build\audio-trace-tests"
}
$OutDir = [System.IO.Path]::GetFullPath($OutDir)

if (-not (Test-Path -LiteralPath $Gcc -PathType Leaf)) {
    throw "MinGW gcc not found at '$Gcc'"
}
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$runnerSrc = Join-Path $root "runner\src"
$audioTraceC = Join-Path $runnerSrc "audio_trace.c"
$historyTestC = Join-Path $root "tests\audio\audio_trace_history_test.c"
$clockTestC = Join-Path $root "tests\audio\audio_trace_clock_gate_test.c"

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

function Invoke-WithEnv {
    param(
        [Parameter(Mandatory=$true)][string]$Exe,
        [string[]]$Args = @(),
        [hashtable]$Set = @{},
        [string[]]$Unset = @(),
        [string]$StderrPath = ""
    )

    $names = New-Object System.Collections.Generic.HashSet[string]
    foreach ($name in $Set.Keys) { [void]$names.Add([string]$name) }
    foreach ($name in $Unset) { [void]$names.Add($name) }

    $old = @{}
    foreach ($name in $names) {
        $old[$name] = @{
            Exists = [Environment]::GetEnvironmentVariable($name, "Process") -ne $null
            Value = [Environment]::GetEnvironmentVariable($name, "Process")
        }
    }

    try {
        foreach ($name in $Unset) {
            [Environment]::SetEnvironmentVariable($name, $null, "Process")
        }
        foreach ($entry in $Set.GetEnumerator()) {
            [Environment]::SetEnvironmentVariable([string]$entry.Key,
                                                  [string]$entry.Value,
                                                  "Process")
        }

        if ($StderrPath -ne "") {
            $proc = Start-Process -FilePath $Exe -ArgumentList $Args `
                -RedirectStandardError $StderrPath -WindowStyle Hidden `
                -Wait -PassThru
            if ($proc.ExitCode -ne 0) {
                throw "$Exe failed with exit code $($proc.ExitCode)"
            }
        } else {
            & $Exe @Args
            if ($LASTEXITCODE -ne 0) {
                throw "$Exe failed with exit code $LASTEXITCODE"
            }
        }
    } finally {
        foreach ($name in $names) {
            if ($old[$name].Exists) {
                [Environment]::SetEnvironmentVariable($name, $old[$name].Value, "Process")
            } else {
                [Environment]::SetEnvironmentVariable($name, $null, "Process")
            }
        }
    }
}

function Assert-StatsStderr {
    param([string]$Path)
    $text = Get-Content -LiteralPath $Path -Raw
    if ($text -notmatch "# ms produced consumed dropped") {
        throw "stderr stats header missing from $Path"
    }
    if ($text -notmatch "(?m)^1000 " -or $text -notmatch "(?m)^2000 ") {
        throw "stderr stats cadence rows missing from $Path"
    }
}

foreach ($mode in 0, 1, 2, 3) {
    $historyExe = Join-Path $OutDir "audio_trace_history_test_$mode.exe"
    Invoke-Native $Gcc @(
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
        "-D_POSIX_C_SOURCE=200809L",
        "-DSNESRECOMP_AUDIO_TRACE_HISTORY=$mode",
        "-I", $runnerSrc,
        $historyTestC,
        $audioTraceC,
        "-o", $historyExe
    )
    Push-Location $OutDir
    try {
        Invoke-Native $historyExe @()
    } finally {
        Pop-Location
    }

    $clockExe = Join-Path $OutDir "audio_trace_clock_gate_test_$mode.exe"
    Invoke-Native $Gcc @(
        "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
        "-D_POSIX_C_SOURCE=200809L",
        "-DSNESRECOMP_AUDIO_TRACE_HISTORY=$mode",
        "-DSNESRECOMP_AUDIO_TRACE_TEST_WALL_MS=1",
        "-DSNESRECOMP_AUDIO_TRACE_TEST_FOPEN=1",
        "-DSNESRECOMP_AUDIO_TRACE_TEST_GETENV=1",
        "-I", $runnerSrc,
        $clockTestC,
        $audioTraceC,
        "-o", $clockExe
    )

    $statsPath = Join-Path $OutDir "audio_trace_clock_gate_stats_$mode.log"
    $failPath = Join-Path $OutDir "audio_trace_clock_gate_fail_$mode.log"
    $stderrPath = Join-Path $OutDir "audio_trace_clock_gate_stderr_$mode.log"
    Remove-Item -LiteralPath $statsPath, $failPath, $stderrPath -Force -ErrorAction SilentlyContinue

    Invoke-WithEnv $clockExe @("off") -Unset @("SNESRECOMP_AUDIO_STATS",
        "SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("snap") -Unset @("SNESRECOMP_AUDIO_STATS",
        "SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("off-empty") `
        -Unset @("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("snap-empty") `
        -Unset @("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("off-zero") `
        -Unset @("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("snap-zero") `
        -Unset @("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("stderr") -Set @{ SNESRECOMP_AUDIO_STATS = "1" } `
        -Unset @("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL") -StderrPath $stderrPath
    Assert-StatsStderr $stderrPath
    Invoke-WithEnv $clockExe @("path") -Set @{ SNESRECOMP_AUDIO_STATS = $statsPath } `
        -Unset @("SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL")
    Invoke-WithEnv $clockExe @("open-fail") -Set @{
        SNESRECOMP_AUDIO_STATS = $failPath
        SNESRECOMP_AUDIO_TRACE_TEST_FOPEN_FAIL = "1"
    }
}

Write-Host "audio_trace history/clock matrix passed"
