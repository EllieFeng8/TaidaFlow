# Build the portable FIELD package of TaidaFlow (w2-057) - runs on the DEVELOPMENT PC.
#
# Output: dist\TaidaFlow-<yyyyMMdd>-<git short hash>[-dirty]\  (dist\ is git-ignored)
#   TaidaFlowApp.exe                     from build\desktop (preset desktop-release; must be up to date)
#   Qt6*.dll, platforms\, styles\, imageformats\, sqldrivers\, tls\, ..., qml\ (QtQuick, Controls,
#                                        Shapes, Timeline, Qt5Compat.GraphicalEffects, ...)
#                                        = windeployqt (Qt 6.8.3) --release --qmldir <project QML>
#   vcruntime140*.dll, msvcp140*.dll ... MSVC runtime, app-local copy of the VC++ redistributable
#                                        DLLs of the toolset that built the exe (no installer needed)
#   web\                                 WebAssembly page + .gz = scripts\deploy-web.ps1 (build\wasm-release)
#   start-taidaflow.ps1/.bat, stop-taidaflow.ps1/.bat, register-autostart.ps1,
#   unregister-autostart.ps1, taidaflow-site.example.bat, logging\quiet.ini   (deploy\release\)
#   scripts\nginx-web.ps1 + deploy\nginx\taidaflow.conf    nginx control script + config template
#   DEPLOY.md                            = docs\DEPLOY_AND_STARTUP.md
#   VERSION.txt, MANIFEST.txt            build information; every file with size and SHA-256
#   nginx\ (only with -IncludeNginx)     nginx.exe + docs\ (LICENSE and the third-party licences)
# Not included: CMake/Ninja files, .lib/.exp/.pdb, tests, databases, ini files, exports, logs,
# Python files, __pycache__ (checked at the end; any hit fails the packaging).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1
#            [-IncludeNginx] [-NginxDir C:\tools\nginx\nginx-1.30.5] [-OutDir dist]
#            [-BuildDir build\desktop] [-WebSource build\wasm-release] [-Force] [-AllowStale]
#   -Force      : replace an existing package folder of the same name (only below -OutDir)
#   -AllowStale : package even when ninja reports that build\desktop or build\wasm-release is not
#                 up to date (not recommended: the folder name carries the current git hash)
# Exit codes: 0 packaged; 2 missing input (build, web, CRT, nginx, windeployqt); 3 build not up to
# date; 4 package folder exists (use -Force); 5 windeployqt / deploy-web failed; 6 forbidden file
# found in the package.
param(
    [switch]$IncludeNginx,
    [string]$NginxDir = "",
    [string]$OutDir = "dist",
    [string]$BuildDir = "build\desktop",
    [string]$WebSource = "build\wasm-release",
    [string]$QtDir = "C:\Qt\6.8.3\msvc2022_64",
    [switch]$Force,
    [switch]$AllowStale
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
function Full([string]$p) {
    if (-not [System.IO.Path]::IsPathRooted($p)) { $p = Join-Path $root $p }
    return [System.IO.Path]::GetFullPath($p).TrimEnd('\')
}
function Say([string]$m) { [Console]::Out.WriteLine($m) }
$BuildDir = Full $BuildDir; $WebSource = Full $WebSource; $OutDir = Full $OutDir
$ninja = 'C:\Qt\Tools\Ninja\ninja.exe'
$windeployqt = Join-Path $QtDir 'bin\windeployqt.exe'

# --- inputs -----------------------------------------------------------------------------------
$exe = Join-Path $BuildDir 'TaidaFlowApp.exe'
$cache = Join-Path $BuildDir 'CMakeCache.txt'
foreach ($f in $exe, $cache, $windeployqt) { if (-not (Test-Path $f -PathType Leaf)) { Say "missing: $f"; exit 2 } }
$cacheText = [System.IO.File]::ReadAllText($cache)
if ($cacheText -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\s*$') { Say "$BuildDir is not a Release build (CMAKE_BUILD_TYPE)"; exit 2 }
if ($cacheText -notmatch '(?m)^CMAKE_CXX_COMPILER:FILEPATH=(.+)$') { Say "CMAKE_CXX_COMPILER not found in $cache"; exit 2 }
$compiler = $Matches[1].Trim()
Say "compiler: $compiler"

# Up to date? (ninja -n only prints what it would do)
foreach ($b in $BuildDir, $WebSource) {
    if (-not (Test-Path (Join-Path $b 'build.ninja'))) { Say "no build.ninja in $b"; exit 2 }
    $n = & $ninja -C $b -n TaidaFlowApp 2>&1 | ForEach-Object { "$_" }
    $upToDate = ($LASTEXITCODE -eq 0) -and (@($n | Where-Object { $_ -match 'no work to do' }).Count -gt 0)
    Say ("ninja -n {0}: {1}" -f $b, $(if ($upToDate) { 'no work to do (up to date)' } else { 'NOT up to date' }))
    if (-not $upToDate) {
        $n | Select-Object -First 5 | ForEach-Object { Say "  | $_" }
        if (-not $AllowStale) { Say "build first (scripts\build-desktop.bat / scripts\build-wasm.bat) or pass -AllowStale"; exit 3 }
    }
}

# MSVC runtime of the toolset that built the exe: ...\VC\Tools\MSVC\<ver>\bin\... -> ...\VC\Redist\MSVC\<ver>\x64\Microsoft.VC*.CRT
if (($compiler -replace '/', '\') -notmatch '^(?<vc>.+\\VC)\\Tools\\MSVC\\(?<ver>[0-9.]+)\\') {
    Say "cannot derive the VC folder from $compiler"; exit 2
}
$vcDir = $Matches['vc']; $toolsVer = [version]$Matches['ver']
$crtDir = $null
$redistRoot = Join-Path $vcDir 'Redist\MSVC'
$candidates = @(Get-ChildItem $redistRoot -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -match '^\d+\.\d+\.\d+$' } |
                Sort-Object { [version]$_.Name })
foreach ($c in $candidates) {
    if ([version]$c.Name -lt $toolsVer) { continue }
    $crt = @(Get-ChildItem (Join-Path $c.FullName 'x64') -Directory -Filter 'Microsoft.VC*.CRT' -ErrorAction SilentlyContinue)
    if ($crt.Count) { $crtDir = $crt[0].FullName; break }
}
if (-not $crtDir) { Say "no VC++ redistributable x64 CRT >= $toolsVer under $redistRoot"; exit 2 }
Say "MSVC runtime: $crtDir (toolset $toolsVer)"

foreach ($required in 'TaidaFlowApp.html', 'TaidaFlowApp.js', 'TaidaFlowApp.wasm', 'qtloader.js') {
    if (-not (Test-Path (Join-Path $WebSource $required) -PathType Leaf)) { Say "missing web file $WebSource\$required"; exit 2 }
}
foreach ($f in 'deploy\release\start-taidaflow.ps1', 'deploy\release\stop-taidaflow.ps1', 'deploy\release\start-taidaflow.bat',
               'deploy\release\stop-taidaflow.bat', 'deploy\release\register-autostart.ps1', 'deploy\release\unregister-autostart.ps1',
               'deploy\release\taidaflow-site.example.bat', 'deploy\release\logging\quiet.ini', 'deploy\nginx\taidaflow.conf',
               'scripts\nginx-web.ps1', 'scripts\deploy-web.ps1', 'docs\DEPLOY_AND_STARTUP.md') {
    if (-not (Test-Path (Join-Path $root $f) -PathType Leaf)) { Say "missing: $f"; exit 2 }
}
if ($IncludeNginx) {
    if ($NginxDir -eq "") {
        $best = $null; $bestVer = $null
        foreach ($d in @(Get-ChildItem 'C:\tools\nginx' -Directory -Filter 'nginx-*' -ErrorAction SilentlyContinue)) {
            $v = $null
            if ([version]::TryParse($d.Name.Substring(6), [ref]$v) -and (Test-Path (Join-Path $d.FullName 'nginx.exe'))) {
                if ($null -eq $bestVer -or $v -gt $bestVer) { $best = $d.FullName; $bestVer = $v }
            }
        }
        $NginxDir = $best
    }
    if (-not $NginxDir -or -not (Test-Path (Join-Path $NginxDir 'nginx.exe')) -or -not (Test-Path (Join-Path $NginxDir 'docs\LICENSE'))) {
        Say "nginx.exe / docs\LICENSE not found (-NginxDir '$NginxDir')"; exit 2
    }
    $NginxDir = Full $NginxDir
}

# --- package name -----------------------------------------------------------------------------
$hash = (& git -C $root rev-parse --short HEAD).Trim()
if ($LASTEXITCODE -ne 0 -or -not $hash) { Say "git rev-parse failed"; exit 2 }
$dirtyFiles = @(& git -C $root status --porcelain -- App Core TaidaFlow TaidaFlowContent Dependencies integration-pack cmake CMakeLists.txt qds.cmake CMakePresets.json)
$dirty = $dirtyFiles.Count -gt 0
$name = 'TaidaFlow-' + (Get-Date -Format 'yyyyMMdd') + '-' + $hash + $(if ($dirty) { '-dirty' } else { '' })
$pkg = Join-Path $OutDir $name
Say "package: $pkg$(if ($dirty) { "  (source changes not committed: $($dirtyFiles.Count) file(s))" })"
if (Test-Path $pkg) {
    if (-not $Force) { Say "exists: $pkg (use -Force to replace it)"; exit 4 }
    $item = Get-Item -LiteralPath $pkg -Force
    if ($item.Attributes -band [System.IO.FileAttributes]::ReparsePoint) { Say "REFUSED: $pkg is a link/junction"; exit 4 }
    if (-not $pkg.StartsWith($OutDir + '\', [System.StringComparison]::OrdinalIgnoreCase)) { Say "REFUSED: $pkg is not below $OutDir"; exit 4 }
    Remove-Item -LiteralPath $pkg -Recurse -Force
    Say "old package folder removed (-Force)"
}
New-Item -ItemType Directory -Force $pkg | Out-Null
$logDir = Full 'build\package-logs'
New-Item -ItemType Directory -Force $logDir | Out-Null

# --- 1. exe + Qt (windeployqt) ------------------------------------------------------------------
Copy-Item -LiteralPath $exe -Destination $pkg
$qmlDirs = @('TaidaFlowContent', 'TaidaFlow', 'Dependencies') | ForEach-Object { Join-Path $root $_ }
$wdqArgs = @('--release', '--no-compiler-runtime', '--no-translations', '--skip-plugin-types', 'qmltooling,canbus',
             '--exclude-plugins', 'qsqlmimer,qsqlodbc,qsqlpsql', '--verbose', '1')
foreach ($d in $qmlDirs) { $wdqArgs += @('--qmldir', $d) }
$wdqArgs += (Join-Path $pkg 'TaidaFlowApp.exe')
$wdqLog = Join-Path $logDir "$name.windeployqt.log"
Say ("windeployqt " + ($wdqArgs -join ' '))
$savedPath = $env:PATH
$env:PATH = (Join-Path $QtDir 'bin') + ';' + $env:PATH
# windeployqt writes its warnings to stderr; Windows PowerShell turns those lines into error
# records, so 'Stop' must not be active while it runs (judged by its exit code only).
$ErrorActionPreference = 'Continue'
try {
    $wdqOut = & $windeployqt @wdqArgs 2>&1 | ForEach-Object { "$_" }
    $wdqRc = $LASTEXITCODE
} finally { $env:PATH = $savedPath; $ErrorActionPreference = 'Stop' }
[System.IO.File]::WriteAllLines($wdqLog, [string[]]$wdqOut)
Say "windeployqt exit $wdqRc (log $wdqLog)"
if ($wdqRc -ne 0) { $wdqOut | Select-Object -Last 15 | ForEach-Object { Say "  | $_" }; exit 5 }

# --- 2. MSVC runtime (app-local) ---------------------------------------------------------------
$crtFiles = @(Get-ChildItem $crtDir -File -Filter '*.dll')
foreach ($f in $crtFiles) { Copy-Item -LiteralPath $f.FullName -Destination $pkg }
Say "MSVC runtime: $($crtFiles.Count) DLL(s) copied ($(($crtFiles | ForEach-Object { $_.Name }) -join ', '))"

# --- 3. web page (same logic as for the development build: scripts\deploy-web.ps1) -------------
$webOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\deploy-web.ps1') -Source $WebSource -ExeDir $pkg
$webRc = $LASTEXITCODE
$webOut | ForEach-Object { Say "  web | $_" }
if ($webRc -ne 0) { Say "deploy-web.ps1 exit $webRc"; exit 5 }

# --- 4. field scripts, nginx template, documentation --------------------------------------------
foreach ($f in 'start-taidaflow.ps1', 'stop-taidaflow.ps1', 'start-taidaflow.bat', 'stop-taidaflow.bat',
               'register-autostart.ps1', 'unregister-autostart.ps1', 'taidaflow-site.example.bat') {
    Copy-Item -LiteralPath (Join-Path $root "deploy\release\$f") -Destination $pkg
}
New-Item -ItemType Directory -Force (Join-Path $pkg 'logging'), (Join-Path $pkg 'scripts'), (Join-Path $pkg 'deploy\nginx') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'deploy\release\logging\quiet.ini') -Destination (Join-Path $pkg 'logging')
Copy-Item -LiteralPath (Join-Path $root 'scripts\nginx-web.ps1') -Destination (Join-Path $pkg 'scripts')
Copy-Item -LiteralPath (Join-Path $root 'deploy\nginx\taidaflow.conf') -Destination (Join-Path $pkg 'deploy\nginx')
Copy-Item -LiteralPath (Join-Path $root 'docs\DEPLOY_AND_STARTUP.md') -Destination (Join-Path $pkg 'DEPLOY.md')

# --- 5. nginx (optional) -------------------------------------------------------------------------
if ($IncludeNginx) {
    $nDir = Join-Path $pkg 'nginx'
    New-Item -ItemType Directory -Force $nDir | Out-Null
    Copy-Item -LiteralPath (Join-Path $NginxDir 'nginx.exe') -Destination $nDir
    Copy-Item -LiteralPath (Join-Path $NginxDir 'docs') -Destination $nDir -Recurse
    $nginxVer = Split-Path -Leaf $NginxDir
    $nginxHash = (Get-FileHash -Algorithm SHA256 (Join-Path $nDir 'nginx.exe')).Hash
    [System.IO.File]::WriteAllLines((Join-Path $nDir 'SOURCE.txt'), [string[]]@(
        "nginx for Windows, $nginxVer, copied unchanged from $NginxDir (official nginx.org zip, signature",
        "checked when it was installed - see the TaidaFlow README, section nginx).",
        "nginx.exe SHA-256: $nginxHash",
        "Licence: docs\LICENSE (2-clause BSD); bundled libraries: docs\OpenSSL.LICENSE, docs\PCRE.LICENCE, docs\zlib.LICENSE.",
        "Only nginx.exe and docs\ are copied: the configuration is rendered by scripts\nginx-web.ps1 into",
        "<data folder>\nginx\conf\taidaflow.conf; logs and temp files go to <data folder>\nginx\."))
    Say "nginx: $nginxVer copied (nginx.exe + docs\ with LICENSE)"
}

# --- 6. forbidden content ------------------------------------------------------------------------
$all = @(Get-ChildItem -LiteralPath $pkg -Recurse -Force)
$forbidden = @($all | Where-Object {
    $n = $_.Name.ToLowerInvariant()
    ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
    ($_.PSIsContainer -and ($n -in '__pycache__', 'cmakefiles', 'exports', 'data', 'runtime', 'logs', 'temp', 'tests', '.qt', '.rcc')) -or
    (-not $_.PSIsContainer -and ($n -match '\.(sqlite|sqlite3|db|py|pyc|pdb|lib|exp|ilk|obj|ninja|cmake|log|csv)$' -or
                                 $n -in 'cmakecache.txt', 'taidaflowsettings.ini', 'device_info.ini', 'taidaflow-site.bat'))
})
if ($forbidden.Count) {
    Say "FORBIDDEN content in the package:"
    $forbidden | ForEach-Object { Say "  $($_.FullName)" }
    exit 6
}

# --- 7. VERSION.txt / MANIFEST.txt -----------------------------------------------------------------
$files = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object FullName)
$gitDate = (& git -C $root log -1 --format=%cI).Trim()
[System.IO.File]::WriteAllLines((Join-Path $pkg 'VERSION.txt'), [string[]]@(
    "TaidaFlow field package $name",
    "created      : $((Get-Date).ToString('o')) on $env:COMPUTERNAME",
    "git          : $(& git -C $root rev-parse HEAD) ($gitDate)$(if ($dirty) { ' + UNCOMMITTED source changes' })",
    "Qt           : 6.8.3 msvc2022_64 (windeployqt, Release)",
    "exe          : $exe ($((Get-Item $exe).LastWriteTime.ToString('s')))",
    "MSVC runtime : app-local DLLs from $crtDir",
    "web page     : $WebSource (TaidaFlowApp.wasm $((Get-Item (Join-Path $WebSource 'TaidaFlowApp.wasm')).LastWriteTime.ToString('s')))",
    "nginx        : $(if ($IncludeNginx) { "included ($NginxDir)" } else { 'not included (use C:\tools\nginx or -Nginx)' })",
    "start        : start-taidaflow.bat (double-click) or start-taidaflow.ps1 - see DEPLOY.md",
    "REST API     : http://<IP>/api/... through nginx -> 127.0.0.1:18080 (w2-060; start-taidaflow.ps1 -RestPort / TAIDAFLOW_REST_PORT)"))
$manifest = New-Object System.Collections.Generic.List[string]
$manifest.Add("# path<TAB>bytes<TAB>SHA-256 (relative to the package folder; MANIFEST.txt itself not listed)")
$files = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Where-Object { $_.Name -ne 'MANIFEST.txt' } | Sort-Object FullName)
$total = 0
foreach ($f in $files) {
    $rel = $f.FullName.Substring($pkg.Length + 1)
    $manifest.Add("$rel`t$($f.Length)`t$((Get-FileHash -Algorithm SHA256 -LiteralPath $f.FullName).Hash)")
    $total += $f.Length
}
[System.IO.File]::WriteAllLines((Join-Path $pkg 'MANIFEST.txt'), $manifest)

# --- summary ------------------------------------------------------------------------------------------
Say ""
Say "top level of $pkg :"
foreach ($e in @(Get-ChildItem -LiteralPath $pkg | Sort-Object { -not $_.PSIsContainer }, Name)) {
    if ($e.PSIsContainer) {
        $sub = @(Get-ChildItem -LiteralPath $e.FullName -Recurse -File)
        Say ("  {0,-40} {1,5} file(s) {2,14:N0} bytes" -f ($e.Name + '\'), $sub.Count, (($sub | Measure-Object Length -Sum).Sum))
    } else {
        Say ("  {0,-40} {1,28:N0} bytes" -f $e.Name, $e.Length)
    }
}
Say ("PACKAGED: {0} file(s), {1:N0} bytes ({2:N1} MB) -> {3}" -f ($files.Count + 1), ($total + (Get-Item (Join-Path $pkg 'MANIFEST.txt')).Length),
     (($total + (Get-Item (Join-Path $pkg 'MANIFEST.txt')).Length) / 1MB), $pkg)
Say "file list with sizes and SHA-256: $pkg\MANIFEST.txt"
exit 0
