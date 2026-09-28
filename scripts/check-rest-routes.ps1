# Compare the REST route table that Core logs with the routes RESTManager really registers (w2-060).
# (w2-062: PowerShell port of the former scripts/check_rest_routes.py; same checks, same output.)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-rest-routes.ps1
#
# * Core/RESTManager.cpp : every  m_httpServer.route("<path>", QHttpServerRequest::Method::<M>, ...)
# * Core/core.cpp        : the kRestRoutes table ({"GET,PUT,OPTIONS", "<path>", "<purpose>"}) that
#                          Core::startRestServer() prints as "[REST] route ..." at start-up and that
#                          README / DEPLOY_AND_STARTUP.md document.
#
# Prints both sets as "<METHOD> <path>" and the differences. A route registered twice (same method
# and path) is reported as a note: QHttpServer uses the first registration, the second is never
# reached. Exit 0 when the (method, path) sets are equal, 1 when they differ, 2 on a parse error.
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Say([string]$m) { [Console]::Out.WriteLine($m) }
$utf8 = New-Object System.Text.UTF8Encoding($false)
$rest = [System.IO.File]::ReadAllText((Join-Path $root 'Core\RESTManager.cpp'), $utf8)
$core = [System.IO.File]::ReadAllText((Join-Path $root 'Core\core.cpp'), $utf8)
$ordinal = [System.StringComparer]::Ordinal

$registered = [regex]::Matches($rest, 'm_httpServer\.route\(\s*"([^"]+)"\s*,\s*QHttpServerRequest::Method::(\w+)')
if ($registered.Count -eq 0) {
    Say "no m_httpServer.route(...) calls found in Core/RESTManager.cpp"
    exit 2
}
# (METHOD, path) -> count; key "METHOD`tpath"
$code = @{}
$codeOrder = New-Object System.Collections.Generic.List[string]
foreach ($m in $registered) {
    $key = $m.Groups[2].Value.ToUpperInvariant() + "`t" + $m.Groups[1].Value
    if ($code.ContainsKey($key)) { $code[$key]++ } else { $code[$key] = 1; $codeOrder.Add($key) }
}
# Python "\n};" is a literal newline; accept CRLF too.
$table = [regex]::Match($core, 'constexpr RestRoute kRestRoutes\[\] = \{(.*?)\r?\n\};', [System.Text.RegularExpressions.RegexOptions]::Singleline)
if (-not $table.Success) {
    Say "kRestRoutes table not found in Core/core.cpp"
    exit 2
}
$logged = New-Object 'System.Collections.Generic.HashSet[string]' ([System.StringComparer]::Ordinal)
foreach ($row in [regex]::Matches($table.Groups[1].Value, '\{\s*"([A-Z,]+)"\s*,\s*"([^"]+)"\s*,')) {
    foreach ($method in $row.Groups[1].Value.Split(',')) { [void]$logged.Add($method + "`t" + $row.Groups[2].Value) }
}

function Split-Key([string]$k) { $i = $k.IndexOf("`t"); return @($k.Substring(0, $i), $k.Substring($i + 1)) }
# Sort keys by (a, b) with ordinal string comparison, like Python tuple sorting.
function Sort-Pairs([string[]]$keys, [switch]$PathFirst) {
    $list = New-Object System.Collections.Generic.List[object]
    foreach ($k in $keys) {
        $p = Split-Key $k
        if ($PathFirst) { $list.Add([pscustomobject]@{ a = $p[1]; b = $p[0]; key = $k }) }
        else { $list.Add([pscustomobject]@{ a = $p[0]; b = $p[1]; key = $k }) }
    }
    $arr = $list.ToArray()
    [System.Array]::Sort($arr, [System.Collections.Generic.Comparer[object]]::Create([System.Comparison[object]]{
        param($x, $y)
        $c = [string]::CompareOrdinal($x.a, $y.a)
        if ($c -ne 0) { return $c }
        return [string]::CompareOrdinal($x.b, $y.b)
    }))
    return @($arr | ForEach-Object { $_.key })
}

$total = 0
foreach ($v in $code.Values) { $total += $v }
Say "RESTManager.cpp registrations: $total ($($code.Count) distinct method+path)"
foreach ($k in (Sort-Pairs @($code.Keys) -PathFirst)) {
    $p = Split-Key $k
    Say ("  {0,-8} {1}" -f $p[0], $p[1])
}
foreach ($k in (Sort-Pairs @($code.Keys))) {
    if ($code[$k] -gt 1) {
        $p = Split-Key $k
        Say "  note: $($p[0]) $($p[1]) is registered $($code[$k]) times (the first registration answers)"
    }
}
Say "core.cpp kRestRoutes: $($logged.Count) method+path"
$onlyCode = @(Sort-Pairs @($code.Keys | Where-Object { -not $logged.Contains($_) }))
$onlyTable = @(Sort-Pairs @($logged | Where-Object { -not $code.ContainsKey($_) }))
foreach ($k in $onlyCode) { $p = Split-Key $k; Say "  MISSING in kRestRoutes: $($p[0]) $($p[1])" }
foreach ($k in $onlyTable) { $p = Split-Key $k; Say "  NOT registered by RESTManager: $($p[0]) $($p[1])" }
$gets = [string[]]@($code.Keys | Where-Object { (Split-Key $_)[0] -eq 'GET' } | ForEach-Object { (Split-Key $_)[1] })
$puts = [string[]]@($code.Keys | Where-Object { (Split-Key $_)[0] -eq 'PUT' } | ForEach-Object { (Split-Key $_)[1] })
[System.Array]::Sort($gets, $ordinal)
[System.Array]::Sort($puts, $ordinal)
Say "GET routes ($($gets.Count)): $($gets -join ' ')"
Say "PUT routes ($($puts.Count)): $($puts -join ' ')"
$ok = ($onlyCode.Count -eq 0) -and ($onlyTable.Count -eq 0)
Say "check_rest_routes exit=$(if ($ok) { 0 } else { 1 })"
if ($ok) { exit 0 } else { exit 1 }
