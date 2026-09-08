param(
  [string]$WorkspaceRoot = "",
  [string]$OutputRoot = "",
  [string]$SmwCurrentDir = "",
  [string]$SmwOriginalPpuDir = "",
  [string]$MmxCurrentDir = "",
  [string]$MmxOriginalPpuDir = "",
  [string]$SmwCurrentExeName = "SuperMarioWorldSNESRecomp.exe",
  [string]$SmwOriginalPpuExeName = "SuperMarioWorldSNESRecomp.exe",
  [string]$MmxCurrentExeName = "MegaManXSNESRecomp.exe",
  [string]$MmxOriginalPpuExeName = "MegaManXSNESRecomp.exe",
  [string]$SmwRom = "",
  [string]$MmxRom = "",
  [int]$Frames = 3000,
  [switch]$Force
)

$ErrorActionPreference = "Stop"
$StageMarker = ".snesrecomp-ppu-workload-staging"

function FullPath([string]$Path) {
  if ([string]::IsNullOrWhiteSpace($Path)) { return "" }
  return [System.IO.Path]::GetFullPath($Path)
}

function JoinFull([string]$Base, [string]$Child) {
  return [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($Base, $Child))
}

function PathPrefix([string]$Path) {
  return (FullPath $Path).TrimEnd('\') + '\'
}

function AssertUnder([string]$Path, [string]$Root, [string]$Label) {
  $full = FullPath $Path
  $rootFull = PathPrefix $Root
  if (!$full.StartsWith($rootFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "$Label must stay under $rootFull, got $full"
  }
}

function AssertLeafName([string]$Name, [string]$Context) {
  if ([string]::IsNullOrWhiteSpace($Name) -or
      [System.IO.Path]::IsPathRooted($Name) -or
      [System.IO.Path]::GetFileName($Name) -ne $Name) {
    throw "$Context must be a file leaf name, got '$Name'"
  }
}

function JoinUnder([string]$Base, [string]$Child, [string]$Context) {
  $joined = JoinFull $Base $Child
  AssertUnder $joined $Base $Context
  return $joined
}

function AssertNotOverlapping([string]$A, [string]$B, [string]$Context) {
  $aFull = PathPrefix $A
  $bFull = PathPrefix $B
  if ($aFull.StartsWith($bFull, [System.StringComparison]::OrdinalIgnoreCase) -or
      $bFull.StartsWith($aFull, [System.StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing overlapping source/output for ${Context}: $aFull vs $bFull"
  }
}

function AssertNoReparsePoint([string]$Path, [string]$Context) {
  if (!(Test-Path -LiteralPath $Path)) { return }
  $item = Get-Item -LiteralPath $Path -Force
  if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing reparse point for ${Context}: $Path"
  }
  if ($item.PSIsContainer) {
    $rp = Get-ChildItem -LiteralPath $Path -Recurse -Force -Attributes ReparsePoint |
        Select-Object -First 1
    if ($rp) {
      throw "Refusing reparse point below ${Context}: $($rp.FullName)"
    }
  }
}

function AssertPathNotReparsePoint([string]$Path, [string]$Context) {
  if (!(Test-Path -LiteralPath $Path)) { return }
  $item = Get-Item -LiteralPath $Path -Force
  if (($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) {
    throw "Refusing reparse point for ${Context}: $Path"
  }
}

function AssertExistingAncestorsNoReparsePoint(
  [string]$Path, [string]$StopRoot, [string]$Context) {
  $full = FullPath $Path
  $stop = FullPath $StopRoot
  AssertUnder $full $stop $Context
  $cur = $full
  while (!(Test-Path -LiteralPath $cur)) {
    $parent = [System.IO.Path]::GetDirectoryName($cur)
    if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cur) {
      return
    }
    $cur = $parent
  }
  while ($true) {
    AssertPathNotReparsePoint $cur $Context
    if ((FullPath $cur).Equals($stop, [System.StringComparison]::OrdinalIgnoreCase)) {
      break
    }
    $parent = [System.IO.Path]::GetDirectoryName($cur)
    if ([string]::IsNullOrWhiteSpace($parent) -or $parent -eq $cur) {
      break
    }
    if (!(FullPath $parent).StartsWith((PathPrefix $stop),
        [System.StringComparison]::OrdinalIgnoreCase) -and
        !(FullPath $parent).Equals($stop,
        [System.StringComparison]::OrdinalIgnoreCase)) {
      break
    }
    $cur = $parent
  }
}

function ResetOwnedDir([string]$Path) {
  $full = FullPath $Path
  AssertUnder $full $OutputRoot "stage directory"
  AssertExistingAncestorsNoReparsePoint $full $FrameworkRoot "stage directory"
  AssertNoReparsePoint $full "stage directory"
  if (Test-Path -LiteralPath $full) {
    $marker = JoinFull $full $StageMarker
    if (!$Force) { throw "Output exists; pass -Force to recreate: $full" }
    if (!(Test-Path -LiteralPath $marker)) {
      throw "Refusing to delete unmarked directory: $full"
    }
    Remove-Item -LiteralPath $full -Recurse -Force
  }
  New-Item -ItemType Directory -Path $full | Out-Null
  Set-Content -LiteralPath (JoinFull $full $StageMarker) `
      -Value "owned by prepare_ppu_workload_staging.ps1" -Encoding ASCII
}

function CopyFileIfPresent([string]$SourceDir, [string]$DestDir, [string]$Name) {
  AssertLeafName $Name "runtime file name"
  $src = JoinUnder $SourceDir $Name "runtime file source"
  if (Test-Path -LiteralPath $src) {
    AssertNoReparsePoint $src "runtime file $Name"
    $dest = JoinUnder $DestDir $Name "runtime file destination"
    Copy-Item -LiteralPath $src -Destination $dest -Force
    return $true
  }
  return $false
}

function CopyRequiredFile([string]$SourceDir, [string]$DestDir, [string]$Name) {
  if (!(CopyFileIfPresent $SourceDir $DestDir $Name)) {
    throw "Missing required runtime file: $(JoinFull $SourceDir $Name)"
  }
}

function CopyDirIfPresent([string]$SourceDir, [string]$DestDir, [string]$Name) {
  AssertLeafName $Name "runtime directory name"
  $src = JoinUnder $SourceDir $Name "runtime directory source"
  if (Test-Path -LiteralPath $src) {
    AssertNoReparsePoint $src "runtime directory $Name"
    $dest = JoinUnder $DestDir $Name "runtime directory destination"
    Copy-Item -LiteralPath $src -Destination $dest -Recurse -Force
    return $true
  }
  return $false
}

function Write-ConfigLocal([string]$StageDir, [string]$Game) {
  if ($Game -eq "mmx") {
    $displayAspect = "DisplayAspect = 4:3`nWidescreen = 0"
  } else {
    $displayAspect = "Widescreen = Standard`nWidescreenHud = 0"
  }
  $text = @"
[General]
SkipLauncher = 1
DisableFrameDelay = 1

[Graphics]
Fullscreen = 0
WindowScale = 3
OutputMethod = SDL
NewRenderer = 1
NoSpriteLimits = 1
$displayAspect

[Sound]
EnableAudio = 1
AudioFreq = 32040
AudioChannels = 2
AudioSamples = 512
"@
  Set-Content -LiteralPath (JoinFull $StageDir "config.local.ini") `
      -Value $text -Encoding ASCII
}

function Write-SmwState([string]$StageDir, [bool]$Wide) {
  $mods = JoinFull $StageDir "mods"
  New-Item -ItemType Directory -Path $mods -Force | Out-Null
  $enabled = if ($Wide) { "true" } else { "false" }
  $state = @"
format_version = 1

[[package]]
id = "super-mario-world.enhancement.msu1"
version = "1.0.0"

[[package]]
id = "super-mario-world.enhancement.widescreen"
version = "1.0.0"

[[feature]]
package_id = "super-mario-world.enhancement.msu1"
id = "msu1"
enabled = false

[[feature]]
package_id = "super-mario-world.enhancement.widescreen"
id = "widescreen"
enabled = $enabled
[feature.values]
mode = "16_9"
hud = "split"
"@
  Set-Content -LiteralPath (JoinFull $mods "state.toml") `
      -Value $state -Encoding ASCII
}

function Write-MmxState([string]$StageDir, [bool]$Wide) {
  $mods = JoinFull $StageDir "mods"
  New-Item -ItemType Directory -Path $mods -Force | Out-Null
  $enabled = if ($Wide) { "true" } else { "false" }
  $state = @"
format_version = 1

[[package]]
id = "megaman-x.enhancement.widescreen"
version = "1.0.0"

[[feature]]
package_id = "megaman-x.enhancement.widescreen"
id = "widescreen"
enabled = $enabled
"@
  Set-Content -LiteralPath (JoinFull $mods "state.toml") `
      -Value $state -Encoding ASCII
}

function Stage-Case(
  [string]$Game, [string]$Variant, [string]$Mode, [string]$SourceDir,
  [string]$GameRoot, [string]$ExeName) {
  if ([string]::IsNullOrWhiteSpace($SourceDir)) { return $null }
  AssertLeafName $ExeName "$Game/$Variant executable name"
  $src = FullPath $SourceDir
  if (!(Test-Path -LiteralPath $src)) {
    Write-Warning "Missing source directory, skipping: $src"
    return $null
  }
  AssertPathNotReparsePoint $src "source artifact directory"
  AssertNotOverlapping $src $OutputRoot "$Game/$Variant/$Mode"
  $dest = JoinFull $OutputRoot "$Game\$Variant\$Mode"
  ResetOwnedDir $dest

  CopyRequiredFile $src $dest $ExeName
  CopyRequiredFile $src $dest "SDL3.dll"
  CopyRequiredFile $src $dest "config.ini"
  CopyFileIfPresent $src $dest "keybinds.ini" | Out-Null
  CopyFileIfPresent $src $dest "rom.cfg" | Out-Null
  CopyDirIfPresent $src $dest "assets" | Out-Null
  if (!(CopyDirIfPresent $src $dest "mods")) {
    CopyDirIfPresent $GameRoot $dest "mods" | Out-Null
  }
  CopyDirIfPresent $src $dest "saves" | Out-Null

  Write-ConfigLocal $dest $Game
  $wide = $Mode -eq "wide"
  if ($Game -eq "smw") {
    Write-SmwState $dest $wide
  } else {
    Write-MmxState $dest $wide
  }

  return [pscustomobject]@{
    Game = $Game
    Variant = $Variant
    Mode = $Mode
    Dir = $dest
    Exe = JoinFull $dest $ExeName
  }
}

$FrameworkRoot = FullPath (Split-Path -Parent $PSScriptRoot)
if ([string]::IsNullOrWhiteSpace($WorkspaceRoot)) {
  $WorkspaceRoot = FullPath (Split-Path -Parent $FrameworkRoot)
} else {
  $WorkspaceRoot = FullPath $WorkspaceRoot
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
  $OutputRoot = JoinFull $FrameworkRoot "build\perf\ppu_workloads"
} else {
  $OutputRoot = FullPath $OutputRoot
}
AssertUnder $OutputRoot $FrameworkRoot "output root"
AssertPathNotReparsePoint $WorkspaceRoot "workspace root"
AssertPathNotReparsePoint $FrameworkRoot "framework root"
AssertExistingAncestorsNoReparsePoint $OutputRoot $FrameworkRoot "output root"

if ([string]::IsNullOrWhiteSpace($SmwCurrentDir)) {
  throw "Pass -SmwCurrentDir explicitly; build-codex-perf-current-full is a known stale bad artifact during this validation."
}
if ([string]::IsNullOrWhiteSpace($SmwOriginalPpuDir)) {
  throw "Pass -SmwOriginalPpuDir explicitly; original-PPU probes must be rebuilt from the same stabilized base as the candidate."
}
if ([string]::IsNullOrWhiteSpace($MmxCurrentDir)) {
  throw "Pass -MmxCurrentDir explicitly; build-codex-perf-current-full is a known stale bad artifact during this validation."
}
if ([string]::IsNullOrWhiteSpace($MmxOriginalPpuDir)) {
  throw "Pass -MmxOriginalPpuDir explicitly; original-PPU probes must be rebuilt from the same stabilized base as the candidate."
}
if ([string]::IsNullOrWhiteSpace($MmxRom)) {
  $MmxRom = JoinFull $WorkspaceRoot "MegamanXRecomp\mmx.sfc"
}
if ([string]::IsNullOrWhiteSpace($SmwRom)) {
  $SmwRom = JoinFull $WorkspaceRoot "SuperMarioWorldRecomp\smw.sfc"
}

New-Item -ItemType Directory -Path $OutputRoot -Force | Out-Null
Set-Content -LiteralPath (JoinFull $OutputRoot $StageMarker) `
    -Value "owned by prepare_ppu_workload_staging.ps1" -Encoding ASCII

$cases = @()
$smwRoot = JoinFull $WorkspaceRoot "SuperMarioWorldRecomp"
$mmxRoot = JoinFull $WorkspaceRoot "MegamanXRecomp"
foreach ($mode in @("native", "wide")) {
  $cases += Stage-Case "smw" "current" $mode $SmwCurrentDir $smwRoot $SmwCurrentExeName
  $cases += Stage-Case "smw" "original_ppu" $mode $SmwOriginalPpuDir $smwRoot $SmwOriginalPpuExeName
  $cases += Stage-Case "mmx" "current" $mode $MmxCurrentDir $mmxRoot $MmxCurrentExeName
  $cases += Stage-Case "mmx" "original_ppu" $mode $MmxOriginalPpuDir $mmxRoot $MmxOriginalPpuExeName
}
$cases = @($cases | Where-Object { $_ -ne $null })

$commands = New-Object System.Collections.Generic.List[string]
$commands.Add('$ErrorActionPreference = "Stop"')
$commands.Add('$env:SNESRECOMP_NO_LAUNCHER = "1"')
$commands.Add('$env:SDL_AUDIODRIVER = "dummy"')
$commands.Add('function Assert-PcmCapture([string]$Path, [int]$Frames) {')
$commands.Add('  $item = Get-Item -LiteralPath $Path -ErrorAction Stop')
$commands.Add('  $minBytes = [int64]$Frames * 500 * 4')
$commands.Add('  if ($item.Length -lt $minBytes) { throw "PCM capture too short: $Path length=$($item.Length) min=$minBytes" }')
$commands.Add('  $fs = [System.IO.File]::OpenRead($item.FullName)')
$commands.Add('  try {')
$commands.Add('    $buf = New-Object byte[] 65536')
$commands.Add('    $nonzero = $false')
$commands.Add('    while (($n = $fs.Read($buf, 0, $buf.Length)) -gt 0) {')
$commands.Add('      for ($i = 0; $i -lt $n; $i++) { if ($buf[$i] -ne 0) { $nonzero = $true; break } }')
$commands.Add('      if ($nonzero) { break }')
$commands.Add('    }')
$commands.Add('    if (-not $nonzero) { throw "PCM capture is all zero: $Path" }')
$commands.Add('  } finally { $fs.Dispose() }')
$commands.Add('}')
$commands.Add('function Get-BmpHashByName([string]$Dir) {')
$commands.Add('  Get-ChildItem -LiteralPath $Dir -File | Sort-Object Name | Get-FileHash -Algorithm SHA256 | ForEach-Object {')
$commands.Add('    [pscustomobject]@{ Name = Split-Path -Leaf $_.Path; Hash = $_.Hash }')
$commands.Add('  }')
$commands.Add('}')
$commands.Add('')

function Add-PairCommand([string]$Game, [string]$Mode, [string]$Rom) {
  $a = $cases | Where-Object { $_.Game -eq $Game -and $_.Variant -eq "current" -and $_.Mode -eq $Mode } | Select-Object -First 1
  $b = $cases | Where-Object { $_.Game -eq $Game -and $_.Variant -eq "original_ppu" -and $_.Mode -eq $Mode } | Select-Object -First 1
  if ($a -and $b) {
    $out = JoinFull $OutputRoot "$Game-$Mode-current-vs-originalppu-5pair.json"
    $pairTool = JoinFull $FrameworkRoot "tools\run_benchmark_pairs.py"
    $commands.Add("python `"$pairTool`" --frames $Frames --pairs 5 --warmups 1 --timeout 180 --a-name current --a-exe `"$($a.Exe)`" --a-rom `"$Rom`" --b-name original_ppu --b-exe `"$($b.Exe)`" --b-rom `"$Rom`" --json-out `"$out`"")
  }
}

Add-PairCommand "smw" "native" (FullPath $SmwRom)
Add-PairCommand "smw" "wide" (FullPath $SmwRom)
Add-PairCommand "mmx" "native" (FullPath $MmxRom)
Add-PairCommand "mmx" "wide" (FullPath $MmxRom)

$commands.Add('')
$commands.Add('# Untimed correctness captures. Run each side separately, then compare file hashes.')
$commands.Add('# BMP dimensions prove effective native/wide width; last_run_report.json should also show breadcrumbs.')
foreach ($case in $cases) {
  $rom = if ($case.Game -eq "smw") { FullPath $SmwRom } else { FullPath $MmxRom }
  $captureRoot = JoinFull $OutputRoot "captures\$($case.Game)-$($case.Variant)-$($case.Mode)"
  $commands.Add("New-Item -ItemType Directory -Force -Path `"$captureRoot\bmp`" | Out-Null")
  $commands.Add("`$env:SNESRECOMP_FRAME_BMP_DIR = `"$captureRoot\bmp`"")
  $commands.Add('$env:SNESRECOMP_FRAME_BMP_START = "300"')
  $commands.Add('$env:SNESRECOMP_FRAME_BMP_END = "900"')
  $commands.Add('$env:SNESRECOMP_FRAME_BMP_STEP = "60"')
  $commands.Add("`$env:SNESRECOMP_DSPOUT = `"$captureRoot\dspout.s16`"")
  $commands.Add("`$env:SNESRECOMP_WRAM_TRACE_FILE = `"$captureRoot\wram.jsonl`"")
  $commands.Add("`$env:SNESRECOMP_APURAM_TRACE_FILE = `"$captureRoot\apuram.jsonl`"")
  $commands.Add("& `"$($case.Exe)`" --benchmark-audio-paced $Frames `"$rom`"")
  $commands.Add("if (`$LASTEXITCODE -ne 0) { throw `"paced capture failed: $($case.Game) $($case.Variant) $($case.Mode) exit=`$LASTEXITCODE`" }")
  $commands.Add("Assert-PcmCapture `"$captureRoot\dspout.s16`" $Frames")
  $commands.Add("Get-FileHash -Algorithm SHA256 `"$captureRoot\dspout.s16`",`"$captureRoot\wram.jsonl`",`"$captureRoot\apuram.jsonl`" | Format-Table -AutoSize")
  $commands.Add('')
}

foreach ($game in @("smw", "mmx")) {
  foreach ($mode in @("native", "wide")) {
    $a = JoinFull $OutputRoot "captures\$game-current-$mode"
    $b = JoinFull $OutputRoot "captures\$game-original_ppu-$mode"
    $commands.Add("Compare-Object (Get-BmpHashByName `"$a\bmp`") (Get-BmpHashByName `"$b\bmp`") -Property Name,Hash")
    $commands.Add("Get-FileHash -Algorithm SHA256 `"$a\dspout.s16`",`"$b\dspout.s16`",`"$a\wram.jsonl`",`"$b\wram.jsonl`",`"$a\apuram.jsonl`",`"$b\apuram.jsonl`" | Format-Table -AutoSize")
  }
}

Set-Content -LiteralPath (JoinFull $OutputRoot "commands.ps1") -Value $commands -Encoding ASCII
$cases | Format-Table Game,Variant,Mode,Dir,Exe -AutoSize
Write-Host "Wrote later-run commands: $(JoinFull $OutputRoot 'commands.ps1')"
