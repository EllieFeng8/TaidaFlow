# w2-069 D2: compare the real TaidaFlowApp.exe --write-default-config output with DEPLOY_AND_STARTUP.md section 6.
# Only writes the default file (the app window is NOT started, no device is contacted).
# Usage (repository root): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-069\tools\check-default-config.ps1 [-OutFile <txt>]
# Exit 0 = exit codes 0 / 3 as documented and 0 differences; 1 otherwise.
param([string]$OutFile = "build\w2-069\check-default-config.txt")
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..')).TrimEnd('\')
if (-not [System.IO.Path]::IsPathRooted($OutFile)) { $OutFile = Join-Path $repo $OutFile }
New-Item -ItemType Directory -Force (Split-Path -Parent $OutFile) | Out-Null
function Clean([string]$s) { return $s.Replace($repo, '<repo>') }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$out = Join-Path $repo 'build\w2-069\config.default.json'
New-Item -ItemType Directory -Force (Split-Path -Parent $out) | Out-Null
if (Test-Path $out) { Remove-Item $out }
$qtRoot = if ($env:TAIDAFLOW_QT_ROOT) { $env:TAIDAFLOW_QT_ROOT } else { 'C:\Qt\6.8.3' }
$env:PATH = (Join-Path $qtRoot 'msvc2022_64\bin') + ';' + $env:PATH
Remove-Item Env:TAIDAFLOW_CONFIG -ErrorAction SilentlyContinue
$exe = Join-Path $repo 'build\desktop\TaidaFlowApp.exe'
$L = New-Object System.Collections.Generic.List[string]
$L.Add('# w2-069 D2: config.json defaults from the real program output')
$L.Add("# HEAD: $(git -C $repo rev-parse --short HEAD)   exe: build\desktop\TaidaFlowApp.exe  $((Get-Item $exe).Length) bytes  $((Get-Item $exe).LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'))")
$ninja = & C:\Qt\Tools\Ninja\ninja.exe -C (Join-Path $repo 'build\desktop') -n TaidaFlowApp 2>&1 | Select-Object -Last 1
$L.Add("# ninja -C build\desktop -n TaidaFlowApp -> $ninja   (exe is up to date with the sources)")
$L.Add('')
$L.Add('> build\desktop\TaidaFlowApp.exe --write-default-config build\w2-069\config.default.json   (Qt bin on PATH, TAIDAFLOW_CONFIG unset)')
$r1 = cmd /c "`"$exe`" --write-default-config `"$out`" 2>&1"; $rc1 = $LASTEXITCODE
$r1 | ForEach-Object { $L.Add('  ' + (Clean $_)) }
$L.Add("  exit=$rc1")
$L.Add('> (same command again: the file exists)')
$r2 = cmd /c "`"$exe`" --write-default-config `"$out`" 2>&1"; $rc2 = $LASTEXITCODE
$r2 | ForEach-Object { $L.Add('  ' + (Clean $_)) }
$L.Add("  exit=$rc2")
$L.Add("  SHA-256 $((Get-FileHash $out).Hash)")
$L.Add('')
$L.Add('--- build\w2-069\config.default.json ---')
Get-Content -LiteralPath $out -Encoding utf8 | ForEach-Object { $L.Add($_) }
$L.Add('')
function Flat($o, [string]$p, $acc) {
    foreach ($prop in $o.PSObject.Properties) {
        $k = if ($p) { "$p.$($prop.Name)" } else { $prop.Name }
        if ($prop.Value -is [System.Management.Automation.PSCustomObject]) { Flat $prop.Value $k $acc } else { $acc[$k] = "$($prop.Value)" }
    }
}
$prog = @{}; Flat (Get-Content -Raw -Encoding utf8 $out | ConvertFrom-Json) '' $prog
$deploy = [System.IO.File]::ReadAllText((Join-Path $repo 'docs\DEPLOY_AND_STARTUP.md'), $utf8)
$m = [regex]::Match($deploy, '(?s)## 6\. config\.json.*?```json\r?\n(.*?)```')
$doc = @{}; Flat ($m.Groups[1].Value | ConvertFrom-Json) '' $doc
$L.Add('--- comparison: program output vs. the example JSON of DEPLOY_AND_STARTUP.md section 6 (key by key) ---')
$diff = 0
foreach ($k in ($prog.Keys + $doc.Keys | Sort-Object -Unique)) {
    $a = $prog[$k]; $b = $doc[$k]
    $ok = ($a -eq $b)
    if (-not $ok) { $diff++ }
    $L.Add(('{0,-4} {1,-32} program={2,-20} DEPLOY_s6={3}' -f $(if ($ok) { 'OK' } else { 'DIFF' }), $k, $a, $b))
}
$L.Add("keys: program $($prog.Count), DEPLOY section 6 example $($doc.Count), differences $diff")
$L.Add('')
$L.Add('--- DEPLOY_AND_STARTUP.md section 6 table: default column per key (checked by hand against the list above) ---')
$s6 = $deploy.IndexOf('## 6. config.json'); $tbl = $deploy.Substring($s6, $deploy.IndexOf('```json', $s6) - $s6)
$tbl -split "`r?`n" | Where-Object { $_.StartsWith('| ' + [char]96) } | ForEach-Object { $L.Add($_.Substring(0, [Math]::Min(150, $_.Length))) }
[System.IO.File]::WriteAllLines($OutFile, $L, $utf8)
$pass = ($rc1 -eq 0) -and ($rc2 -eq 3) -and ($diff -eq 0) -and ($prog.Count -gt 0)
"write-default-config exit $rc1 (expect 0), again exit $rc2 (expect 3), keys $($prog.Count) / $($doc.Count), differences $diff -> $(if ($pass) { 'PASS' } else { 'FAIL' })  (details: $(Clean $OutFile))"
if ($pass) { exit 0 } else { exit 1 }
