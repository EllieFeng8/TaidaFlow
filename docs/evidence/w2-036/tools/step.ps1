# w2-036 test helper: send one UI input with scripts\desktop_input.py and record it (with a
# millisecond timestamp) in docs\evidence\w2-036\input-log.txt.
#   -Target desk : TaidaFlow window (title "TaidaFlow")
#   -Target sim  : Adam60xxSimulator window (title "Adam60xxSimulator")
# Example: powershell -ExecutionPolicy Bypass -File step.ps1 -Target sim -X 382 -Y 712 -Note "DI2 -> 1"
param(
    [ValidateSet('desk', 'sim')][string]$Target = 'desk',
    [int]$X = -1,
    [int]$Y = -1,
    [string]$Key = "",
    [string]$Type = "",
    [string]$Note = ""
)
$ErrorActionPreference = 'Stop'
$evidence = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $evidence))   # taidaflow\
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
$tool = Join-Path $root 'scripts\desktop_input.py'
$titleArgs = @()
if ($Target -eq 'sim') { $titleArgs = @('--title', 'Adam60xxSimulator') }

$ts = (Get-Date).ToString('HH:mm:ss.fff')
if ($X -ge 0) {
    $out = & $py $tool @titleArgs click $X $Y
    $what = "click ($X,$Y)"
} elseif ($Key -ne "") {
    $out = & $py $tool @titleArgs key $Key
    $what = "key $Key"
} elseif ($Type -ne "") {
    $out = & $py $tool @titleArgs type $Type
    $what = "type '$Type'"
} else { Write-Output 'nothing to do'; exit 1 }
$code = $LASTEXITCODE
$line = "[$ts] w2-036 $Target $what : $Note (exit $code; $out)"
Add-Content -Path (Join-Path $evidence 'input-log.txt') -Value $line -Encoding utf8
Write-Output $line
exit $code
