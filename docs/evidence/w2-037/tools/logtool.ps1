# w2-037 evidence helper: read the running desktop log (shared read; the app keeps it open)
# and either print its line count as a marker in input-log.txt, or print lines matching a
# pattern from a given line on.  The Qt stderr log is written in the console code page
# (cp950) and is decoded as such.
#   powershell -File logtool.ps1 -Log <path> -Marker "before DI1 -> 1"
#   powershell -File logtool.ps1 -Log <path> -From 120 -Pattern 'Alarm inserted|\[Alarm\]\[UI\]'
param(
    [Parameter(Mandatory = $true)][string]$Log,
    [string]$Marker = "",
    [int]$From = 1,
    [string]$Pattern = ""
)
$ErrorActionPreference = 'Stop'
$fs = [System.IO.File]::Open($Log, 'Open', 'Read', 'ReadWrite')
try {
    $ms = New-Object System.IO.MemoryStream
    $fs.CopyTo($ms)
    $text = [System.Text.Encoding]::GetEncoding(950).GetString($ms.ToArray())
} finally { $fs.Close() }
$lines = $text -split "`r?`n"
if ($Marker -ne "") {
    $evidence = Split-Path -Parent $PSScriptRoot
    $line = "[$((Get-Date).ToString('HH:mm:ss.fff'))] w2-037 marker: desktop log line $($lines.Count) : $Marker"
    Add-Content -Path (Join-Path $evidence 'input-log.txt') -Value $line -Encoding utf8
    Write-Output $line
    exit 0
}
for ($i = [Math]::Max(0, $From - 1); $i -lt $lines.Count; $i++) {
    if ($Pattern -eq "" -or $lines[$i] -match $Pattern) { Write-Output ("{0}: {1}" -f ($i + 1), $lines[$i]) }
}
