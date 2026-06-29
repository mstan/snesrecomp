$ErrorActionPreference = 'Continue'
$cw  = 'F:\Projects\snesrecomp\_wt-accuracy\tools\cyc_watch'
$py  = 'C:\Users\Matthew\.pyenv\pyenv-win\versions\3.10.9\python.exe'
$dll = 'F:\Projects\_bsnes_src\bsnes_libretro.dll'
Set-Location $cw

& $py build_test_rom.py "$cw\_static.sfc" static
& $py build_test_rom.py "$cw\_dynamics.sfc" dynamics

Write-Host "=== STATIC region (expect MATCH 60) ==="
& "$cw\bsnes_cycles_probe.exe" $dll "$cw\_static.sfc" 0x008000 0x008011 60
Write-Host "static_exit=$LASTEXITCODE"
Write-Host "=== DYNAMICS region (expect MATCH 13) ==="
& "$cw\bsnes_cycles_probe.exe" $dll "$cw\_dynamics.sfc" 0x00800B 0x008011 13
Write-Host "dynamics_exit=$LASTEXITCODE"
