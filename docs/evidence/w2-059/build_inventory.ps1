# w2-059 D3: list every top-level entry of taidaflow\build and taidaflow\dist with its size,
# mark the ones kept by the assignment (build\desktop, wasm-release, runtime-cwd, sim-cwd, nginx,
# runtime-logs [last 7 days]; dist = newest package only) and print the totals.
# Read-only unless -Delete is given; with -Delete the entries marked DELETE are removed and the
# result of every removal is printed.  Refuses (exit 2) while TaidaFlowApp, Adam60xxSimulator or
# nginx is running, or a process command line points into build\ or dist\ (other than tail.exe,
# which is reported).
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-059\build_inventory.ps1 [-Delete]
param([switch]$Delete)
$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$build = Join-Path $root 'build'
$dist = Join-Path $root 'dist'
$keepBuild = @('desktop', 'wasm-release', 'runtime-cwd', 'sim-cwd', 'nginx', 'runtime-logs')

function SizeOf($item) {
    if ($item.PSIsContainer) {
        $s = (Get-ChildItem -LiteralPath $item.FullName -Recurse -Force -File -ErrorAction SilentlyContinue | Measure-Object Length -Sum).Sum
        if ($null -eq $s) { 0 } else { [int64]$s }
    } else { [int64]$item.Length }
}

$procs = @(Get-Process TaidaFlowApp, Adam60xxSimulator, nginx -ErrorAction SilentlyContinue)
$users = @(Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -and $_.ProcessId -ne $PID -and
        ($_.CommandLine -match [regex]::Escape($build) -or $_.CommandLine -match [regex]::Escape($dist) -or
         $_.CommandLine -match 'taidaflow/build/|taidaflow/dist/') -and $_.CommandLine -notmatch 'build_inventory\.ps1' })
foreach ($u in $users) { Write-Output ("process using build/dist: PID {0} {1}: {2}" -f $u.ProcessId, $u.Name, $u.CommandLine) }
if ($procs.Count) {
    Write-Output ("running: " + (($procs | ForEach-Object { "$($_.ProcessName)#$($_.Id)" }) -join ', ') + " - nothing deleted")
    exit 2
}
$blocking = @($users | Where-Object { $_.Name -ne 'tail.exe' })
if ($Delete -and $blocking.Count) { Write-Output 'a process uses build/dist - nothing deleted'; exit 2 }

$rows = New-Object System.Collections.Generic.List[object]
$cutoff = (Get-Date).AddDays(-7)
foreach ($it in Get-ChildItem -LiteralPath $build -Force) {
    $action = if ($keepBuild -contains $it.Name) { 'KEEP' } else { 'DELETE' }
    $rows.Add([pscustomobject]@{ Path = "build\$($it.Name)"; Bytes = (SizeOf $it); Modified = $it.LastWriteTime.ToString('yyyy-MM-dd HH:mm'); Action = $action; Item = $it })
}
$old = @(Get-ChildItem -LiteralPath (Join-Path $build 'runtime-logs') -Recurse -Force -File -ErrorAction SilentlyContinue | Where-Object { $_.LastWriteTime -lt $cutoff })
foreach ($f in $old) {
    $rows.Add([pscustomobject]@{ Path = $f.FullName.Substring($root.Length + 1); Bytes = $f.Length; Modified = $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm'); Action = 'DELETE(>7d)'; Item = $f })
}
$packs = @(Get-ChildItem -LiteralPath $dist -Force -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending)
for ($i = 0; $i -lt $packs.Count; $i++) {
    $rows.Add([pscustomobject]@{ Path = "dist\$($packs[$i].Name)"; Bytes = (SizeOf $packs[$i]); Modified = $packs[$i].LastWriteTime.ToString('yyyy-MM-dd HH:mm'); Action = $(if ($i -eq 0) { 'KEEP(newest)' } else { 'DELETE' }); Item = $packs[$i] })
}

$rows | Select-Object Path, @{ n = 'MB'; e = { '{0:N1}' -f ($_.Bytes / 1MB) } }, Bytes, Modified, Action | Format-Table -AutoSize | Out-String -Width 220 | Write-Output
$total = ($rows | Where-Object { $_.Path -notmatch '^build\\runtime-logs\\' } | Measure-Object Bytes -Sum).Sum
$del = ($rows | Where-Object { $_.Action -like 'DELETE*' } | Measure-Object Bytes -Sum).Sum
Write-Output ("TOTAL build+dist bytes={0} ({1:N2} GB); marked DELETE bytes={2} ({3:N2} GB); after deletion ~{4:N2} GB" -f $total, ($total / 1GB), $del, ($del / 1GB), (($total - $del) / 1GB))

if ($Delete) {
    $failed = 0
    foreach ($r in $rows | Where-Object { $_.Action -like 'DELETE*' }) {
        try {
            Remove-Item -LiteralPath $r.Item.FullName -Recurse -Force -Confirm:$false -ErrorAction Stop
            Write-Output "deleted $($r.Path)"
        } catch {
            $failed++
            Write-Output "NOT deleted $($r.Path): $($_.Exception.Message)"
        }
    }
    Write-Output "delete failures=$failed"
}
exit 0
