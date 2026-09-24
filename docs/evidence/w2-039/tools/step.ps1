# w2-039 test helper (adapted from docs\evidence\w2-037\tools\step.ps1): send one click to the
# TaidaFlow window with scripts\desktop_input.py and record it with a millisecond timestamp in
# docs\evidence\w2-039\input-log.txt.  The click is refused (exit 4, nothing sent) when the
# window is not at -ExpectLeft/-ExpectTop.
#   powershell -ExecutionPolicy Bypass -File step.ps1 -X 1165 -Y 81 -ExpectLeft 949 -ExpectTop 459 -Note "History tab"
param(
    [Parameter(Mandatory = $true)][int]$X,
    [Parameter(Mandatory = $true)][int]$Y,
    [Parameter(Mandatory = $true)][int]$ExpectLeft,
    [Parameter(Mandatory = $true)][int]$ExpectTop,
    [string]$Note = ""
)
$ErrorActionPreference = 'Stop'
$evidence = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $evidence))   # taidaflow\
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
$tool = Join-Path $root 'scripts\desktop_input.py'
$info = & $py $tool info
$ts = (Get-Date).ToString('HH:mm:ss.fff')
if ($info -notmatch "L=$ExpectLeft T=$ExpectTop ") {
    $line = "[$ts] w2-039 click ($X,$Y) REFUSED: window not at L=$ExpectLeft T=$ExpectTop ($info) : $Note"
    Add-Content -Path (Join-Path $evidence 'input-log.txt') -Value $line -Encoding utf8
    Write-Output $line
    exit 4
}
$out = & $py $tool click $X $Y
$line = "[$ts] w2-039 click ($X,$Y) -> $out : $Note"
Add-Content -Path (Join-Path $evidence 'input-log.txt') -Value $line -Encoding utf8
Write-Output $line
