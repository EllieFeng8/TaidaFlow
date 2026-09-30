# w2-074 D5: search the package's text files (.bat, .ps1, .md, .txt, .json, .conf, nginx conf files - every
# file without a NUL byte, below 20 MB) and the default config.json the package exe writes for
# development-machine paths and names. Prints every hit with file:line. Read-only.
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File check-dev-paths.ps1 -Package <folder> [-DefaultConfig <config.json written by --write-default-config>]
# Exit: 0 no hit; 1 at least one hit (see the list); 2 bad parameters.
param([Parameter(Mandatory = $true)][string]$Package, [string]$DefaultConfig = "")
$Package = [System.IO.Path]::GetFullPath($Package).TrimEnd('\')
if (-not (Test-Path -LiteralPath (Join-Path $Package 'TaidaFlowApp.exe'))) { [Console]::Out.WriteLine("no package at $Package"); exit 2 }
$patterns = [ordered]@{
    'D:\repo'   = '(?i)(?<![A-Za-z])D:[\\/]+repo'
    'C:\Users'  = '(?i)(?<![A-Za-z])C:[\\/]+Users'
    'C:\Qt'     = '(?i)(?<![A-Za-z])C:[\\/]+Qt\b'
    'C:\tools'  = '(?i)(?<![A-Za-z])C:[\\/]+tools\b'
    "user name $env:USERNAME" = ('(?i)\b' + [regex]::Escape($env:USERNAME) + '\b')
    "computer name $env:COMPUTERNAME" = ('(?i)' + [regex]::Escape($env:COMPUTERNAME))
}
$files = @(Get-ChildItem -LiteralPath $Package -Recurse -File | Where-Object { $_.Length -lt 20MB -and $_.Extension -notmatch '^\.(exe|dll|wasm|gz|png|jpg|ico|ttf|otf|woff2?|qmlc)$' })
if ($DefaultConfig) { $files += Get-Item -LiteralPath $DefaultConfig }
$n = 0; $hits = 0
$utf8 = New-Object System.Text.UTF8Encoding($false)
foreach ($f in $files) {
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    if ([Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }
    $n++
    $lines = $utf8.GetString($bytes) -split "`r?`n"
    $rel = if ($f.FullName.StartsWith($Package + '\')) { $f.FullName.Substring($Package.Length + 1) } else { "(default config) $($f.FullName)" }
    for ($i = 0; $i -lt $lines.Count; $i++) {
        foreach ($k in $patterns.Keys) {
            if ($lines[$i] -match $patterns[$k]) { $hits++; [Console]::Out.WriteLine(("HIT [{0}] {1}:{2}: {3}" -f $k, $rel, ($i + 1), $lines[$i].Trim())) }
        }
    }
}
[Console]::Out.WriteLine("scanned $n text file(s); hits: $hits")
if ($hits) { exit 1 }
exit 0
