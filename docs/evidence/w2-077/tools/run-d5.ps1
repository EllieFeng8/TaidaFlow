# w2-077 D5: all verification steps in order, one log per step in docs\evidence\w2-077\ and the exit
# codes in docs\evidence\w2-077\d5-summary.txt. Before every build step: wait-no-build.ps1 (process names).
#   powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-077\tools\run-d5.ps1 [-Only <step>[,<step>]]
# Steps (expected exit): 01 debug-configure (0), 02 debug-build (0), 03 preset-desktop-release (0),
# 04 preset-wasm-release (0), 05 mingw-probe (NOT 0 + hint in the log), 06 checks (0), 07 app-tests (0),
# 08 core-tests (0). Exit 0 = every step as expected. Never starts TaidaFlowApp.
param([string[]]$Only = @())
$ErrorActionPreference = 'Continue'
$tools = $PSScriptRoot
$ev = Split-Path -Parent $tools
$root = (Resolve-Path (Join-Path $tools '..\..\..\..')).Path
Set-Location $root
$summary = Join-Path $ev 'd5-summary.txt'
function Line([string]$m) { $s = "{0:yyyy-MM-dd HH:mm:ss} {1}" -f (Get-Date), $m; Write-Output $s; Add-Content -LiteralPath $summary -Value $s -Encoding UTF8 }
$steps = @(
    @{ id = '01'; name = 'debug-configure';        build = $true;  cmd = "`"$tools\clion-debug.bat`" configure fresh"; expect = 0 },
    @{ id = '02'; name = 'debug-build';            build = $true;  cmd = "`"$tools\clion-debug.bat`" build clean";     expect = 0 },
    @{ id = '03'; name = 'preset-desktop-release'; build = $true;  cmd = "`"$tools\clion-preset.bat`" desktop-release fresh"; expect = 0 },
    @{ id = '04'; name = 'preset-wasm-release';    build = $true;  cmd = "`"$tools\clion-preset.bat`" wasm-release fresh";    expect = 0 },
    @{ id = '05'; name = 'mingw-probe';            build = $true;  cmd = "`"$tools\clion-mingw-probe.bat`"";            expect = -1 },
    @{ id = '06'; name = 'checks';                 build = $false; cmd = "`"$tools\run-checks.bat`"";                    expect = 0 },
    @{ id = '07'; name = 'app-tests';              build = $true;  cmd = "`"$root\App\tests\run-app-tests.bat`" fresh";   expect = 0 },
    @{ id = '08'; name = 'core-tests';             build = $true;  cmd = "`"$root\Core\tests\run-core-tests.bat`" fresh"; expect = 0 }
)
$bad = 0
foreach ($s in $steps) {
    if ($Only.Count -gt 0 -and -not ($Only -contains $s.id)) { continue }
    $log = Join-Path $ev ("d5-{0}-{1}.log" -f $s.id, $s.name)
    if ($s.build) {
        $w = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $tools 'wait-no-build.ps1') -Tag $s.name
        Line ($w -join ' | ')
    }
    Line "step $($s.id) $($s.name) start: cmd /c $($s.cmd)"
    $t0 = Get-Date
    & cmd.exe /d /c "$($s.cmd) > `"$log`" 2>&1"
    $rc = $LASTEXITCODE
    $sec = [int]((Get-Date) - $t0).TotalSeconds
    if ($s.expect -eq -1) {
        $text = Get-Content -LiteralPath $log -Raw -Encoding UTF8
        $hint = $text.Contains('supports MSVC only') -and $text.Contains('Toolchains') -and $text.Contains('Visual Studio') -and $text.Contains('BUILD.md')
        $ok = ($rc -ne 0) -and $hint
        Line ("step {0} {1}: exit={2} ({3}s), hint in log={4} -> {5}" -f $s.id, $s.name, $rc, $sec, $hint, $(if ($ok) { 'as expected (configure refused)' } else { 'NOT as expected' }))
    } else {
        $ok = ($rc -eq $s.expect)
        Line ("step {0} {1}: exit={2} ({3}s) -> {4}" -f $s.id, $s.name, $rc, $sec, $(if ($ok) { 'OK' } else { 'NOT as expected' }))
    }
    if (-not $ok) { $bad++ }
}
Line "run-d5: $bad step(s) not as expected"
exit $(if ($bad -eq 0) { 0 } else { 1 })
