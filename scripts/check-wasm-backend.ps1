# Prove "Desktop-only Core / WASM only builds the Proxy" on already-built trees.
# (w2-062: PowerShell port of the former scripts/check_wasm_backend.py; same checks, same output.)
#
#   powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-wasm-backend.ps1 build\desktop build\wasm-release
#
# For each build directory (the target is TaidaFlowApp.exe on desktop, TaidaFlowApp.js on
# WebAssembly) it reports, from Ninja's own build graph and the produced binary:
#
#   backend_sources   : backend .cpp files (Core/core.cpp, manager.cpp, Modbus_Client.cpp,
#                       Modbus_Server.cpp, Ms300FaultReader.cpp, SqlManager.cpp, RESTManager.cpp,
#                       HistoryExport.cpp [w2-041], AppHttpServer/AppHttpServer.cpp [w2-049]) that
#                       appear in `ninja -t commands <target>` (every compile/link command needed
#                       to produce the target)
#   backend_qt_libs   : Qt SerialBus / SerialPort / Sql / HttpServer / Concurrent libraries on the
#                       final link command
#   backend_qt_found  : Qt6<Module>_DIR entries for those modules in CMakeCache.txt (a
#                       find_package hit, direct or transitive)
#   backend_strings   : backend-only literals found in the binary (.exe / .wasm)
#
# Expected: desktop has all of them (sanity check that the probe works); the WASM build has
# backend_sources=0, backend_qt_libs=0, backend_strings=0 and no SerialBus/SerialPort/HttpServer/
# Concurrent package. (Qt6Sql_DIR may appear in the WASM cache: Qt's own QML plugin package scan
# loads Qt6QmlLocalStorage, which depends on Sql; it is not linked.)
#
# Ninja: TAIDAFLOW_QT_TOOLS\Ninja\ninja.exe (default C:\Qt\Tools\Ninja\ninja.exe), or -Ninja <path>.
# Exit 0 when the WASM tree(s) contain no backend source, library or string and every desktop tree
# does contain them; 1 otherwise; 2 = no build folder given.
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$Ninja = "",
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$BuildDirs
)
$ErrorActionPreference = 'Stop'
if ($Ninja -eq "") {
    $tools = if ($env:TAIDAFLOW_QT_TOOLS) { $env:TAIDAFLOW_QT_TOOLS } else { 'C:\Qt\Tools' }
    $Ninja = Join-Path $tools 'Ninja\ninja.exe'
}
$backendSources = @('core.cpp', 'manager.cpp', 'Modbus_Client.cpp', 'Modbus_Server.cpp',
                    'Ms300FaultReader.cpp', 'SqlManager.cpp', 'RESTManager.cpp',
                    'HistoryExport.cpp', 'AppHttpServer.cpp')
$backendModules = @('SerialBus', 'SerialPort', 'Sql', 'HttpServer', 'Concurrent')
$backendStrings = @('192.168.1.201', '192.168.1.205', 'COM2', 'TaidaFlowSettings.ini',
                    'settings.sqlite', 'device_info.ini', '[ModbusServer]', '[MS300]',
                    'ModbusClient', 'SqlManager', 'RESTManager', 'QModbusTcpClient',
                    'HistoryExportManager',
                    # w2-049: AppHttpServer singleton + Core's web page folder lookup
                    '[AppHttpServer]', 'AppHttpServerThread', 'TAIDAFLOW_WEB_DIR',
                    # w2-060: REST API enabled in Core (Core::startRestServer)
                    '[REST] REST API',
                    # w2-062: Core reads config.json (AppConfig) and writes runtime.json; the
                    # former TAIDAFLOW_REST_PORT variable is no longer read (removed from the list)
                    '[Config] Core', '[Web] runtime.json')
function Say([string]$m) { [Console]::Out.WriteLine($m) }

if (-not $BuildDirs -or $BuildDirs.Count -lt 1) {
    Say "usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\check-wasm-backend.ps1 <build dir> [<build dir> ...]"
    Say "  e.g. scripts\check-wasm-backend.ps1 build\desktop build\wasm-release"
    exit 2
}
# Byte-exact text view of a file (ISO-8859-1: one char per byte) for substring search.
$latin1 = [System.Text.Encoding]::GetEncoding(28591)

function Get-Analysis([string]$dirArg) {
    # Printed like Python's Path(): backslashes, no trailing separator.
    $shown = ($dirArg -replace '/', '\').TrimEnd('\')
    if ($shown -eq '') { $shown = '.' }
    $build = [System.IO.Path]::GetFullPath($dirArg)
    $wasm = (Test-Path (Join-Path $build 'TaidaFlowApp.js') -PathType Leaf) -and (Test-Path (Join-Path $build 'TaidaFlowApp.wasm') -PathType Leaf)
    $target = if ($wasm) { 'TaidaFlowApp.js' } else { 'TaidaFlowApp.exe' }
    $binary = Join-Path $build $(if ($wasm) { 'TaidaFlowApp.wasm' } else { 'TaidaFlowApp.exe' })
    $saved = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    $commands = @(& $Ninja -C $build -t commands $target 2>$null | ForEach-Object { "$_" })
    $rc = $LASTEXITCODE
    $ErrorActionPreference = $saved
    if ($rc -ne 0) { return @($false, "${shown}: ninja -t commands failed ($rc)") }
    $compiled = New-Object System.Collections.Generic.List[string]
    foreach ($src in $backendSources) {
        # Core/<file>, or one class folder below Core (Core/AppHttpServer/AppHttpServer.cpp)
        $pat = New-Object System.Text.RegularExpressions.Regex ('[/\\]Core[/\\](?:[A-Za-z0-9_]+[/\\])?' + [regex]::Escape($src) + '(\s|"|$)')
        foreach ($c in $commands) { if ($pat.IsMatch($c)) { $compiled.Add($src); break } }
    }
    # The final link edge in build.ninja: its LINK_LIBRARIES variable holds the libraries
    # (MSVC links through a response file, so `ninja -t commands` only shows @rsp).
    $ninjaText = [System.IO.File]::ReadAllText((Join-Path $build 'build.ninja'), [System.Text.Encoding]::UTF8)
    $edge = [regex]::Match($ninjaText, '^build ' + [regex]::Escape($target) + '[ :].*?(?=^build |\z)',
                           [System.Text.RegularExpressions.RegexOptions]::Multiline -bor [System.Text.RegularExpressions.RegexOptions]::Singleline)
    $linkVars = if ($edge.Success) { $edge.Value } else { '' }
    $libs = @($backendModules | Where-Object { [regex]::IsMatch($linkVars, '(?i)(Qt6' + $_ + '\.lib|libQt6' + $_ + '\.a)') })
    $cache = [System.IO.File]::ReadAllText((Join-Path $build 'CMakeCache.txt'), [System.Text.Encoding]::UTF8)
    $found = @($backendModules | Where-Object { [regex]::IsMatch($cache, '^Qt6' + $_ + '_DIR:PATH=(?!.*NOTFOUND)', [System.Text.RegularExpressions.RegexOptions]::Multiline) })
    $bytes = [System.IO.File]::ReadAllBytes($binary)
    $data = $latin1.GetString($bytes)
    # QStringLiteral stores UTF-16; plain literals are UTF-8/Latin-1. Look for both.
    $strings = @($backendStrings | Where-Object {
        $data.IndexOf($_, [System.StringComparison]::Ordinal) -ge 0 -or
        $data.IndexOf($latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($_)), [System.StringComparison]::Ordinal) -ge 0 })
    $kind = if ($wasm) { 'wasm' } else { 'desktop' }
    function Join-OrDash($list) { if (@($list).Count) { (@($list) -join ', ') } else { '-' } }
    $report = "$shown [$kind] target=$target binary_bytes=$($bytes.Length) commands=$($commands.Count)`n" +
              "  backend_sources ($($compiled.Count)): $(Join-OrDash $compiled)`n" +
              "  backend_qt_libs ($($libs.Count)): $(Join-OrDash $libs)`n" +
              "  backend_qt_found ($($found.Count)): $(Join-OrDash $found)`n" +
              "  backend_strings ($($strings.Count)): $(Join-OrDash $strings)"
    if ($wasm) {
        $badFound = @($found | Where-Object { $_ -ne 'Sql' })
        $ok = ($compiled.Count -eq 0) -and ($libs.Count -eq 0) -and ($strings.Count -eq 0) -and ($badFound.Count -eq 0)
    } else {
        $ok = ($compiled.Count -eq $backendSources.Count) -and ($libs.Count -gt 0) -and ($strings.Count -gt 0)
    }
    return @($ok, ($report + "`n  verdict: " + $(if ($ok) { 'OK' } else { 'FAIL' })))
}

$status = 0
foreach ($d in $BuildDirs) {
    $r = Get-Analysis $d
    foreach ($line in ($r[1] -split "`n")) { Say $line }
    if (-not $r[0]) { $status = 1 }
}
Say "check_wasm_backend exit=$status"
exit $status
