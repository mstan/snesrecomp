$ErrorActionPreference = "Stop"
$root = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$out = Join-Path $root "build\diagnostic_gates_test.exe"
$gcc = (Get-Command gcc).Source
$args = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\diagnostic_gates_test.c",
    "$root\runner\src\common_cpu_infra.c",
    "-Wl,--gc-sections", "-o", $out
)

New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
Remove-Item -LiteralPath $out -Force -ErrorAction SilentlyContinue
& $gcc @args
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $out)) {
    throw "gcc did not produce the diagnostic gate test executable"
}
& $out
if ($LASTEXITCODE -ne 0) { throw "diagnostic gate test failed" }

$wlogOut = Join-Path $root "build\wlog_addr_lazy_test.exe"
$wlogArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\wlog_addr_lazy_test.c",
    "$root\runner\src\cpu_state.c",
    "-Wl,--wrap=getenv",
    "-Wl,--gc-sections", "-lm", "-o", $wlogOut
)

Remove-Item -LiteralPath $wlogOut -Force -ErrorAction SilentlyContinue
& $gcc @wlogArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $wlogOut)) {
    throw "gcc did not produce the wlog addr lazy test executable"
}
foreach ($mode in @("unset", "empty", "invalid")) {
    & $wlogOut $mode
    if ($LASTEXITCODE -ne 0) {
        throw "wlog addr lazy test failed in $mode mode"
    }
}

$apuDiagOut = Join-Path $root "build\apu_port_diag_getenv_test.exe"
$apuDiagArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-function", "-Wno-unused-parameter",
    "-ffunction-sections", "-fdata-sections",
    "-DSNESRECOMP_APU_PORT_DIAG_TEST=1",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\apu_port_diag_getenv_test.c",
    "$root\runner\src\snes\interp_bridge.c",
    "-Wl,--wrap=getenv",
    "-Wl,--gc-sections", "-lm", "-o", $apuDiagOut
)

Remove-Item -LiteralPath $apuDiagOut -Force -ErrorAction SilentlyContinue
& $gcc @apuDiagArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $apuDiagOut)) {
    throw "gcc did not produce the APU port diagnostic getenv test executable"
}
foreach ($mode in @("unset-apu", "empty-apu", "zero-apu",
                    "unset-nonport", "empty-nonport", "zero-nonport")) {
    $apuDiagErr = Join-Path $root ("build\apu_port_diag_getenv_{0}.stderr.log" -f $mode)
    Remove-Item -LiteralPath $apuDiagErr -Force -ErrorAction SilentlyContinue
    $oldErrorAction = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    try {
        & $apuDiagOut $mode 2> $apuDiagErr
        $apuDiagExit = $LASTEXITCODE
    } finally {
        $ErrorActionPreference = $oldErrorAction
    }
    if ($apuDiagExit -ne 0) {
        throw "APU port diagnostic getenv test failed in $mode mode"
    }
}

$cartBusOut = Join-Path $root "build\cart_cpu_bus_latch_test.exe"
$cartBusArgs = @(
    "-std=c11", "-Wall", "-Wextra", "-Werror",
    "-Wno-unused-function", "-Wno-unused-parameter",
    "-ffunction-sections", "-fdata-sections",
    "-I$root\runner\src", "-I$root\runner\src\snes",
    "$root\tests\runtime_dispatch\cart_cpu_bus_latch_test.c",
    "$root\runner\src\cpu_state.c",
    "-Wl,--gc-sections", "-lm", "-o", $cartBusOut
)

Remove-Item -LiteralPath $cartBusOut -Force -ErrorAction SilentlyContinue
& $gcc @cartBusArgs
if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $cartBusOut)) {
    throw "gcc did not produce the cart CPU bus latch test executable"
}
& $cartBusOut
if ($LASTEXITCODE -ne 0) {
    throw "cart CPU bus latch test failed"
}
