# w2-069 D1 / D3 text checks of README.md, docs/BUILD.md, docs/DEPLOY_AND_STARTUP.md, docs/wasm-integration-report.md.
# Read-only. Usage (repository root): powershell -NoProfile -ExecutionPolicy Bypass -File docs\evidence\w2-069\tools\check-docs.ps1
# Exit 0 = every check passed.
$ErrorActionPreference = 'Stop'
$repo = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..\..\..')).TrimEnd('\')
$utf8 = New-Object System.Text.UTF8Encoding($false)
$docs = @('README.md', 'docs/BUILD.md', 'docs/DEPLOY_AND_STARTUP.md', 'docs/wasm-integration-report.md')
$text = @{}
foreach ($d in $docs) { $text[$d] = [System.IO.File]::ReadAllText((Join-Path $repo $d), $utf8) }
$fail = 0
function Check([string]$name, [bool]$ok) { if (-not $ok) { $script:fail++ }; [Console]::Out.WriteLine(('{0}  {1}' -f $(if ($ok) { 'PASS' } else { 'FAIL' }), $name)) }
$r = $text['README.md']
# D1 (a) loading page transition
foreach ($s in @('轉場 w1-066', 'requestAnimationFrame', '250 ms', '800 ms', 'prefers-reduced-motion: reduce', '200 ms 的淡出淡入', '1.5 秒保底', '轉場立即中止')) {
    Check "D1 README loading page contains '$s'" ($r.Contains($s))
}
Check "D1 README no longer says the loader is hidden right at onLoaded" (-not $r.Contains('`onLoaded` 後整個載入頁隱藏'))
# D1 (b) mirror: nginx 80 /mirror is the official path, LanRelay 8125 is the fallback, pack 1.0.2 not required
foreach ($s in @('正式的網頁同步經 nginx 80 的 `/mirror`', 'LanRelay(8125,`App/lanrelay.h`)只是 nginx 未啟用時的備援', '不再等待 pack 1.0.2', '可再評估是否移除 LanRelay')) {
    Check "D1 README mirror section contains '$s'" ($r.Contains($s))
}
foreach ($d in $docs) {
    Check "D2 $d has no '暫時做法' / '等 pack 1.0.2' any more" (-not ($text[$d].Contains('暫時做法') -or $text[$d].Contains('等 pack 1.0.2')))
    Check "D2 $d has no 'five tokens' wording (the template has six: + log folder)" (-not ($text[$d].Contains('五個記號') -or $text[$d].Contains('五個值')))
}
# D3 no local machine paths
foreach ($p in @('D:\repo', 'C:\Users', 'D:/repo', 'C:/Users')) {
    $o = & git -C $repo grep -n -F $p -- $docs 2>&1
    Check "D3 git grep -F '$p' in the four documents: no match (exit $LASTEXITCODE)" ($LASTEXITCODE -eq 1)
}
# D3 Traditional Chinese: no Simplified-only characters in the lines changed against HEAD
[Console]::OutputEncoding = [Text.Encoding]::UTF8
$ErrorActionPreference = 'Continue'   # git prints CRLF warnings on stderr
$added = & git -C $repo -c core.quotepath=off diff -U0 HEAD -- $docs 2>$null | Where-Object { $_ -like '+*' -and $_ -notlike '+++*' }
$simp = '这为后时个设载动画错误关闭开启档页资讯连线网络节标显发现应数据库状态复执进过还没问题该实际测试'.ToCharArray()
$hits = @(foreach ($l in $added) { foreach ($c in $simp) { if ($l.Contains([string]$c)) { "$c" } } })
Check "D3 changed lines against HEAD: $(@($added).Count), Simplified-only characters: $($hits.Count)" ($hits.Count -eq 0)
"=== $fail check(s) failed"
if ($fail -eq 0) { exit 0 } else { exit 1 }
