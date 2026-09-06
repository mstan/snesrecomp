<#
.SYNOPSIS
New SNES recomp project scaffolding -- the Windows entry point.

.DESCRIPTION
The scaffolder is tools/new_project/setup_project.sh. This file is deliberately
not a second copy of it: it finds the bash that ships with Git for Windows and
runs the same script with the same arguments, so a fix to the flow reaches
every platform at once. Every question is asked in this console -- bash reads
the answers through the console's /dev/tty, exactly as it does on Linux.

Requires Git for Windows (for bash.exe), Python 3 and CMake on PATH. WSL and
Git Bash users can run setup_project.sh directly instead.

.PARAMETER Rom
Your legally-owned ROM (.sfc/.smc). Probed for identity; never copied into the
repository. Optional on a terminal: with no ROM given it is the first question.

.PARAMETER Dir
Parent directory for the new repository (default: the current directory).

.PARAMETER Name
Display name. Default: derived from the dump filename or cartridge header.

.PARAMETER Players
Seats, 1-8. Above two configures a Super Multitap.

.PARAMETER Yes
Non-interactive: every question takes its default; network toggles stay off.

.PARAMETER Passthrough
Every other setup_project.sh flag passes straight through: --rollback,
--netplay, --no-ci, --generate, --build, --create-github,
--snesrecomp-ref <ref>, --fetch-boxart, ... Run with -Help for the list.

.EXAMPLE
powershell -File tools\new_project\setup_project.ps1 -Rom C:\roms\game.sfc -Dir C:\src

.EXAMPLE
powershell -File tools\new_project\setup_project.ps1 C:\roms\game.sfc

.EXAMPLE
powershell -File tools\new_project\setup_project.ps1 -Rom game.sfc -Name "My Game" -Players 4 -Yes --rollback
#>
[CmdletBinding(PositionalBinding = $false)]
param(
    [Parameter(Position = 0)][string]$Rom = "",
    [string]$Dir = "",
    [string]$Name = "",
    [int]$Players = 0,
    [switch]$Yes,
    [switch]$Help,
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$Passthrough = @()
)

$ErrorActionPreference = "Stop"
$ScriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$Script = Join-Path $ScriptDir "setup_project.sh"

function Find-GitBash {
    # Prefer the bash that lives beside git.exe. PATH may carry WSL's
    # System32\bash.exe, which would run the scaffold inside a different
    # filesystem and leave the project somewhere under /mnt.
    $git = Get-Command git.exe -ErrorAction SilentlyContinue
    if ($git) {
        $dir = Split-Path -Parent $git.Source
        for ($i = 0; $i -lt 4 -and $dir; $i++) {
            foreach ($rel in @("bin\bash.exe", "usr\bin\bash.exe")) {
                $candidate = Join-Path $dir $rel
                if (Test-Path -LiteralPath $candidate) { return $candidate }
            }
            $dir = Split-Path -Parent $dir
        }
    }
    $roots = @($env:ProgramFiles, ${env:ProgramFiles(x86)}, "$env:LOCALAPPDATA\Programs")
    foreach ($root in $roots) {
        if (-not $root) { continue }
        $candidate = Join-Path $root "Git\bin\bash.exe"
        if (Test-Path -LiteralPath $candidate) { return $candidate }
    }
    return $null
}

function ConvertTo-BashPath([string]$Path) {
    # Git Bash accepts C:/dir/file; the backslashes are what it does not.
    if (-not $Path) { return $Path }
    return ([System.IO.Path]::GetFullPath($Path) -replace '\\', '/')
}

$bash = Find-GitBash
if (-not $bash) {
    Write-Error ("setup_project: Git for Windows' bash.exe was not found. Install Git for " +
                 "Windows (https://git-scm.com/download/win), or run " +
                 "tools/new_project/setup_project.sh from WSL or Git Bash.")
    exit 1
}
foreach ($tool in @("git", "cmake")) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Write-Warning "$tool is not on PATH; the scaffold needs it before generate/build."
    }
}
if (-not (Get-Command python3 -ErrorAction SilentlyContinue) -and
    -not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Error "setup_project: Python 3 is not on PATH."
    exit 1
}

$shArgs = @()
if ($Help) { $shArgs += "--help" }
if ($Rom) {
    if (-not (Test-Path -LiteralPath $Rom)) {
        Write-Error "setup_project: ROM not found: $Rom"
        exit 1
    }
    $shArgs += @("--rom", (ConvertTo-BashPath $Rom))
}
if ($Dir) { $shArgs += @("--dir", (ConvertTo-BashPath $Dir)) }
if ($Name) { $shArgs += @("--name", $Name) }
if ($Players -gt 0) { $shArgs += @("--players", "$Players") }
if ($Yes) { $shArgs += "--yes" }
$shArgs += $Passthrough

& $bash (ConvertTo-BashPath $Script) @shArgs
exit $LASTEXITCODE
