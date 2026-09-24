# w2-037 test helper: send one UI input with scripts\desktop_input.py and record it (with a
# millisecond timestamp) in docs\evidence\w2-037\input-log.txt.
#   -Target desk : TaidaFlow window (title "TaidaFlow")
#   -Target sim  : Adam60xxSimulator window (title "Adam60xxSimulator")
# Example: powershell -ExecutionPolicy Bypass -File step.ps1 -Target sim -X 382 -Y 712 -Note "DI2 -> 1"
param(
    [ValidateSet('desk', 'sim')][string]$Target = 'desk',
    [int]$X = -1,
    [int]$Y = -1,
    [string]$Key = "",
    [string]$Type = "",
    [string]$Note = "",
    [int]$ExpectLeft = -99999,
    [int]$ExpectTop = -99999
)
$ErrorActionPreference = 'Stop'
$evidence = Split-Path -Parent $PSScriptRoot
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $evidence))   # taidaflow\
$py = 'C:\Users\TED\AppData\Local\Programs\Python\Python312\python.exe'
$tool = Join-Path $root 'scripts\desktop_input.py'
$titleArgs = @()
if ($Target -eq 'sim') { $titleArgs = @('--title', 'Adam60xxSimulator') }

$ts = (Get-Date).ToString('HH:mm:ss.fff')
# w2-037 guard (added after the 17:40:55 click landed outside the simulator window): with
# -ExpectLeft/-ExpectTop the window rectangle is read first and the click is refused
# (exit 4, nothing sent) when the window is not at the expected position.
if ($X -ge 0 -and $ExpectLeft -ne -99999) {
    $info = & $py $tool @titleArgs info
    if ($info -notmatch "L=$ExpectLeft T=$ExpectTop ") {
        $line = "[$ts] w2-037 $Target click ($X,$Y) REFUSED: window not at L=$ExpectLeft T=$ExpectTop ($info) : $Note"
        Add-Content -Path (Join-Path $evidence 'input-log.txt') -Value $line -Encoding utf8
        Write-Output $line
        exit 4
    }
}
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
$line = "[$ts] w2-037 $Target $what : $Note (exit $code; $out)"
Add-Content -Path (Join-Path $evidence 'input-log.txt') -Value $line -Encoding utf8
Write-Output $line
exit $code

