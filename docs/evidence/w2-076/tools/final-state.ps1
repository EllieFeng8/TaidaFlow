# w2-076: final state after the task (read-only): dist\, retired packages, running processes / ports of ours,
# the service TaidaFlowNginx, the real Startup folders, scheduled task TaidaFlow, git status.
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..\..')).Path
function Say([string]$m) { [Console]::Out.WriteLine($m) }
Say "time: $((Get-Date).ToString('o'))"
Say "--- dist\"
Get-ChildItem -LiteralPath (Join-Path $root 'dist') | ForEach-Object { Say ("  {0}  {1}" -f $_.Name, $(if ($_.PSIsContainer) { "$(@(Get-ChildItem -LiteralPath $_.FullName -Recurse -File).Count) files" } else { "$($_.Length) bytes SHA-256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash)" })) }
Say "--- build\w2-076-retired-dist\ (old package, moved - not deleted)"
Get-ChildItem -LiteralPath (Join-Path $root 'build\w2-076-retired-dist') -ErrorAction SilentlyContinue | ForEach-Object { Say "  $($_.Name)" }
Say "--- processes TaidaFlowApp / nginx / Adam60xxSimulator"
$p = @(Get-Process TaidaFlowApp, nginx, Adam60xxSimulator -ErrorAction SilentlyContinue)
if ($p.Count) { $p | ForEach-Object { Say "  $($_.ProcessName) $($_.Id) $(try { $_.Path } catch { '' })" } } else { Say "  none" }
Say "--- listeners on 80 502 8124 8125 18080 18125"
$l = @(Get-NetTCPConnection -State Listen -ErrorAction SilentlyContinue | Where-Object { @(80, 502, 8124, 8125, 18080, 18125) -contains $_.LocalPort })
if ($l.Count) { $l | ForEach-Object { Say "  $($_.LocalAddress):$($_.LocalPort) pid $($_.OwningProcess)" } } else { Say "  none" }
Say "--- Windows service TaidaFlowNginx: $(@(Get-CimInstance Win32_Service -Filter "Name='TaidaFlowNginx'").Count) (0 = not installed)"
Say "--- real Startup folders"
foreach ($d in [Environment]::GetFolderPath('Startup'), [Environment]::GetFolderPath('CommonStartup')) { Say "  $d : $((@(Get-ChildItem -LiteralPath $d -Force | ForEach-Object { $_.Name })) -join ', ')" }
$t = $null; try { $t = Get-ScheduledTask -TaskName TaidaFlow -TaskPath '\' -ErrorAction Stop } catch { }
Say "--- scheduled task \TaidaFlow: $(if ($t) { "EXISTS ($($t.State))" } else { 'none' })"
Say "--- git (taidaflow) HEAD $(& git -C $root rev-parse --short HEAD), branch $(& git -C $root rev-parse --abbrev-ref HEAD)"
& git -C $root status --porcelain | ForEach-Object { Say "  $_" }
exit 0
