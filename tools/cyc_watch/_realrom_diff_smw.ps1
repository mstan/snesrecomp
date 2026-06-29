$ErrorActionPreference = 'Continue'
$cw  = 'F:\Projects\snesrecomp\_wt-accuracy\tools\cyc_watch'
$dll = 'F:\Projects\_bsnes_src\bsnes_libretro.dll'
$rom = 'F:\Projects\snesrecomp\SuperMarioWorldRecomp\build\bin-x64-Release\smw.sfc'
Set-Location $cw

# (start, end, recomp_delta) from the SMW recomp ring (ring_pick.py).
$cases = @(
  # --- clean regions (non-self-loop start) ---
  @('0x00C510','0x00C508',  5),
  @('0x00C508','0x00C510',  7),
  @('0x00AE54','0x00AE51',  5),
  @('0x00AE51','0x00AE54',  4),
  @('0x0180A9','0x0180D2', 10),
  @('0x018126','0x0180AF',  6),
  @('0x028B69','0x028B74',  7),
  @('0x029B0C','0x029B16', 10),
  # --- loop-exit edges (start self-loops; tight latch isolates) ---
  @('0x008496','0x0084C7', 76),
  @('0x00A355','0x00A36D', 34),
  @('0x00A375','0x00A38D', 34),
  @('0x00F7EA','0x00F7F2', 13)
)

Write-Host ("{0,-10} {1,-10} {2,8} {3,10}  {4}" -f 'start','end','recomp','bsnes','verdict')
foreach ($c in $cases) {
  $out = & "$cw\bsnes_cycles_probe.exe" $dll $rom $c[0] $c[1] $c[2] 2>&1
  $line = ($out | Out-String)
  $bsnes = 'n/a'; $verdict = '?'
  if ($line -match 'bsnes CPU cycles = (-?\d+)') { $bsnes = $matches[1] }
  if ($line -match 'FAIL')         { $verdict = 'NOT-HIT' }
  elseif ($line -match 'MISMATCH') { $verdict = 'MISMATCH' }
  elseif ($line -match 'MATCH')    { $verdict = 'MATCH' }
  Write-Host ("{0,-10} {1,-10} {2,8} {3,10}  {4}" -f $c[0],$c[1],$c[2],$bsnes,$verdict)
}
Write-Host "=== DONE ==="
