$ErrorActionPreference = 'Continue'
$cw  = 'F:\Projects\snesrecomp\_wt-accuracy\tools\cyc_watch'
$dll = 'F:\Projects\_bsnes_src\bsnes_libretro.dll'
$rom = 'F:\Projects\snesrecomp\MegamanXRecomp\build\bin-x64-Release\mmx.sfc'
Set-Location $cw

$cases = @(
  @('0x00D35C','0x00D34A', 14),
  @('0x00D34A','0x00D35C', 12),
  @('0x008289','0x00828E',  6),
  @('0x00828E','0x0082B6',  5),
  @('0x0082B6','0x008289', 24),
  @('0x00B5FE','0x00B5ED', 30),
  @('0x00B60F','0x00B5F5', 30),
  @('0x00B5EA','0x00B5FE',  6)
)

Write-Host ("{0,-10} {1,-10} {2,8} {3,10}  {4}" -f 'start','end','recomp','bsnes','verdict')
foreach ($c in $cases) {
  $out = & "$cw\bsnes_cycles_probe.exe" $dll $rom $c[0] $c[1] $c[2] 7200 2>&1
  $line = ($out | Out-String)
  $bsnes = 'n/a'; $verdict = '?'
  if ($line -match 'bsnes CPU cycles = (-?\d+)') { $bsnes = $matches[1] }
  if ($line -match 'FAIL')         { $verdict = 'NOT-HIT' }
  elseif ($line -match 'MISMATCH') { $verdict = 'MISMATCH' }
  elseif ($line -match 'MATCH')    { $verdict = 'MATCH' }
  Write-Host ("{0,-10} {1,-10} {2,8} {3,10}  {4}" -f $c[0],$c[1],$c[2],$bsnes,$verdict)
}
Write-Host "=== DONE ==="
