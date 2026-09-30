# w2-076 evidence helper (same as docs\evidence\w2-074\tools\run-step.ps1): runs one command line through
# cmd.exe in the taidaflow folder, saves its output to docs\evidence\w2-076\<Name>.txt and appends the REAL
# exit code (read by PowerShell from $LASTEXITCODE after cmd.exe ended - "cmd /c "x & echo %errorlevel%""
# would expand %errorlevel% before x runs).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-076\tools\run-step.ps1 -Name <file stem> -Cmd "<cmd line>"
param(
    [Parameter(Mandatory = $true)][string]$Name,
    [Parameter(Mandatory = $true)][string]$Cmd
)
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
$out = Join-Path (Split-Path -Parent $PSScriptRoot) "$Name.txt"
Set-Location $root
$sw = [Diagnostics.Stopwatch]::StartNew()
$start = (Get-Date).ToString('o')
& "$env:SystemRoot\System32\cmd.exe" /d /c "$Cmd > `"$out`" 2>&1 < NUL"
$rc = $LASTEXITCODE
Add-Content -LiteralPath $out -Value @("----", "cmd     : $Cmd", "started : $start", "elapsed : $([int]$sw.Elapsed.TotalSeconds)s", "EXIT=$rc") -Encoding UTF8
[Console]::Out.WriteLine("$Name EXIT=$rc ($([int]$sw.Elapsed.TotalSeconds)s) -> $out")
exit $rc
