$ErrorActionPreference = 'Continue'
$cw  = 'F:\Projects\snesrecomp\_wt-accuracy\tools\cyc_watch'
$gcc = 'C:\msys64\mingw64\bin\gcc.exe'
$dll = 'F:\Projects\_bsnes_src\bsnes_libretro.dll'
$rom = 'F:\Projects\snesrecomp\LegendofZeldaAlttpRecomp\Legend of Zelda, The - A Link to the Past (USA).sfc'
Set-Location $cw

Write-Host "=== rebuild probe (1800-frame budget) ==="
& $gcc -O2 -I 'F:/Projects/_bsnes_src/bsnes/target-libretro' "$cw\bsnes_cycles_probe.c" -o "$cw\bsnes_cycles_probe.exe"
if ($LASTEXITCODE -ne 0) { Write-Host "probe build FAILED"; exit 1 }

# (start, end, recomp_delta) from the recomp ring (constant-d transitions).
# CLEAN = non-self-loop start (regression); LOOP-EXIT = start self-loops and
# exits to end (the tight latch isolates the adjacent exit edge).
$cases = @(
  # --- clean regions (regression: must stay MATCH) ---
  @('0x0092B2','0x009328',118),   # clean
  @('0x009328','0x009341', 40),   # clean
  @('0x009341','0x0092B2',  3),   # clean (backward loop-branch)
  @('0x00814C','0x008200',197),   # clean
  @('0x008719','0x00874E', 17),   # clean
  @('0x00811E','0x00813C', 39),   # clean
  # --- loop-exit edges (were MISMATCH under first-hit; tight latch fixes) ---
  @('0x008420','0x008489',172),   # loop-exit (self-loop 173, exit 172)
  @('0x0085FE','0x00865C',150)    # loop-exit (self-loop 151, exit 150)
)

Write-Host ""
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
