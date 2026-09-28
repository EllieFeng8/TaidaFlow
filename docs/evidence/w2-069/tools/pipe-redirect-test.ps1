# w2-069: does the caller wait for the launched program when the output of a run-*.ps1 style launcher is piped /
# redirected? Uses the same Start-Process -RedirectStandardError/-RedirectStandardOutput pattern as
# scripts\run-desktop.ps1 / run-simulator.ps1, with PING.EXE (about 11 s, ends by itself) as the child - TaidaFlow and
# the simulator are NOT started. Temporary files in build\w2-069\pipetest (removed at the end).
# Usage (repository root): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-069\tools\pipe-redirect-test.ps1 [-OutFile <txt>]
# Exit 0 = A and B wait (> 8 s), C and D return at once (< 3 s), as README / wasm-integration-report say; 1 otherwise.
param([string]$OutFile = "build\w2-069\pipe-redirect-test.txt")
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..')).TrimEnd('\')
if (-not [System.IO.Path]::IsPathRooted($OutFile)) { $OutFile = Join-Path $repo $OutFile }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$tmp = Join-Path $repo 'build\w2-069\pipetest'
New-Item -ItemType Directory -Force $tmp | Out-Null
$launch = @'
# mimics scripts\run-desktop.ps1 / run-simulator.ps1: Start-Process with stdout/stderr redirected to files, then exit
$d = Split-Path -Parent $MyInvocation.MyCommand.Path
$p = Start-Process -FilePath "$env:SystemRoot\System32\PING.EXE" -ArgumentList '-n','12','127.0.0.1' -PassThru -WindowStyle Hidden `
     -RedirectStandardError "$d\child.err" -RedirectStandardOutput "$d\child.out"
Write-Output "started pid $($p.Id)"
exit 0
'@
$lp = Join-Path $tmp 'launch.ps1'
[System.IO.File]::WriteAllText($lp, $launch, $utf8)
$L = New-Object System.Collections.Generic.List[string]
$L.Add('# w2-069: does the caller wait for the launched program when the output of a run-*.ps1 style launcher is piped / redirected?')
$L.Add('# launcher (same Start-Process -RedirectStandardError/-RedirectStandardOutput pattern as scripts\run-desktop.ps1 line 96):')
$launch -split "`n" | ForEach-Object { $L.Add('#   ' + $_.TrimEnd()) }
$L.Add('# child = PING.EXE -n 12 127.0.0.1 (about 11 s, ends by itself); the launcher itself exits at once.')
$L.Add('')
$t = @{}
function Time-Case([string]$id, [string]$name, [scriptblock]$b) {
    $sw = [Diagnostics.Stopwatch]::StartNew(); & $b; $sw.Stop()
    $t[$id] = $sw.Elapsed.TotalSeconds
    $L.Add(('{0,-60} returned after {1,5:N1} s' -f $name, $sw.Elapsed.TotalSeconds))
    Start-Sleep -Seconds 12   # let the child end before the next case
}
Time-Case 'A' 'A  PowerShell:  powershell -File launch.ps1 | Out-Null' { & powershell -NoProfile -ExecutionPolicy Bypass -File $lp | Out-Null }
Time-Case 'B' 'B  PowerShell:  powershell -File launch.ps1 > file' { & powershell -NoProfile -ExecutionPolicy Bypass -File $lp > (Join-Path $tmp 'b.txt') }
Time-Case 'C' 'C  cmd:         powershell -File launch.ps1 > file' { cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$lp`" > `"$tmp\c.txt`"" }
Time-Case 'D' 'D  cmd:         powershell -File launch.ps1   (direct)' { cmd /c "powershell -NoProfile -ExecutionPolicy Bypass -File `"$lp`"" }
$L.Add('')
$L.Add('Result: a pipe (A) and a PowerShell ">" redirection (B, PowerShell collects the output through a pipe) wait until the')
$L.Add('launched program ends; cmd ">" to a file (C) and a direct run (D) return at once. README / wasm-integration-report say so.')
[System.IO.File]::WriteAllLines($OutFile, $L, $utf8)
Remove-Item -Recurse -Force $tmp
$pass = ($t['A'] -gt 8) -and ($t['B'] -gt 8) -and ($t['C'] -lt 3) -and ($t['D'] -lt 3)
('A {0:N1} s, B {1:N1} s, C {2:N1} s, D {3:N1} s -> {4}' -f $t['A'], $t['B'], $t['C'], $t['D'], $(if ($pass) { 'PASS' } else { 'FAIL' }))
if ($pass) { exit 0 } else { exit 1 }
