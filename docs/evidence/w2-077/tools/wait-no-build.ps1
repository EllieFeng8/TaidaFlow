# w2-077: wait until no other build is running (cl / clang++ / clang / wasm-ld / ninja / cmake), by
# process name only. Max -MaxMinutes (default 60). exit 0 = free, 1 = still busy after the limit.
param([int]$MaxMinutes = 60, [string]$Tag = "")
$names = 'cl', 'clang++', 'clang', 'wasm-ld', 'ninja', 'cmake'
$deadline = (Get-Date).AddMinutes($MaxMinutes)
$waited = 0
while ($true) {
    $p = @(Get-Process -Name $names -ErrorAction SilentlyContinue)
    if ($p.Count -eq 0) {
        Write-Output ("[wait-no-build] {0} {1:yyyy-MM-dd HH:mm:ss} no cl/clang++/clang/wasm-ld/ninja/cmake running (waited {2}s)" -f $Tag, (Get-Date), $waited)
        exit 0
    }
    if ((Get-Date) -gt $deadline) {
        Write-Output ("[wait-no-build] {0} still busy after {1} min: {2}" -f $Tag, $MaxMinutes, (($p | ForEach-Object { "$($_.Name)#$($_.Id) $($_.Path)" }) -join '; '))
        exit 1
    }
    if ($waited % 60 -eq 0) {
        Write-Output ("[wait-no-build] {0} {1:HH:mm:ss} busy: {2}" -f $Tag, (Get-Date), (($p | ForEach-Object { "$($_.Name)#$($_.Id)" }) -join ', '))
    }
    Start-Sleep -Seconds 15
    $waited += 15
}
