# Build the portable FIELD package of TaidaFlow (w2-057, w2-062, w2-065) - runs on the DEVELOPMENT PC.
#
# Output: dist\TaidaFlow-<yyyyMMdd>-<git short hash>[-dirty]\  (dist\ is git-ignored)
#   TaidaFlowApp.exe                     from build\desktop (preset desktop-release; must be up to date)
#   Qt6*.dll, platforms\, styles\, imageformats\, sqldrivers\, tls\, ..., qml\ (QtQuick, Controls,
#                                        Shapes, Timeline, Qt5Compat.GraphicalEffects, ...)
#                                        = windeployqt (Qt 6.8.3) --release --qmldir <project QML>
#   vcruntime140*.dll, msvcp140*.dll ... MSVC runtime, app-local copy of the VC++ redistributable
#                                        DLLs of the toolset that built the exe (no installer needed)
#   web\                                 WebAssembly page + .gz + runtime.json (the app defaults) =
#                                        scripts\deploy-web.ps1 -NoConfig (build\wasm-release)
#   start-taidaflow.ps1/.bat, stop-taidaflow.ps1/.bat, register-autostart.ps1,
#   unregister-autostart.ps1                                         (deploy\release\)
#                                        (w2-065: no logging\quiet.ini any more - the app writes its own
#                                        log files, quiet + full, into config.json log.dir)
#   scripts\install-nginx-config.ps1, scripts\taidaflow-config.ps1 + deploy\nginx\taidaflow.conf
#                                        nginx.conf generator (one-time install step; start-taidaflow.ps1
#                                        also runs it when nginx.conf is missing or out of date),
#                                        config.json reader, nginx template
#   nginx\                               nginx for Windows (Mango A7): nginx.exe, docs\ (licences), conf\
#                                        WITHOUT nginx.conf (written on the plant PC by
#                                        install-nginx-config.ps1, so an update never overwrites it),
#                                        SOURCE.txt; from -NginxDir, default (w2-065) the folder of nginx.exe of
#                                        the development config.json (TAIDAFLOW_CONFIG, else deploy\dev\config.dev.json:
#                                        nginx.exe), else the newest C:\tools\nginx\nginx-<version> (the build PC's
#                                        convention, as CMake TAIDAFLOW_NGINX_DIR). The PACKAGE never names that
#                                        folder: its config.json default is nginx\nginx.exe (the bundled copy).
#   nginx\nginx-service.exe,             (w2-076) WinSW v2.12.0 (self-contained .NET single file, MIT) = the
#   nginx\LICENSE-WinSW.txt,             service wrapper of scripts\install-nginx-service.ps1 (service
#   nginx\SOURCE-WinSW.txt               TaidaFlowNginx); from -WinSW <WinSW-x64-<version>.exe>, default the newest
#                                        C:\tools\winsw\WinSW-x64-<version>.exe (the build PC's convention, like
#                                        C:\tools\nginx); LICENSE.txt must be in the same folder. nginx-service.xml
#                                        is NOT in the package (written on the plant PC by install-nginx-service.ps1).
#   scripts\install-nginx-service.ps1, scripts\uninstall-nginx-service.ps1,
#   scripts\add-startup-shortcut.ps1, scripts\remove-startup-shortcut.ps1        (w2-076, field scripts)
#   DEPLOY.md                            = docs\DEPLOY_AND_STARTUP.md
#   VERSION.txt, MANIFEST.txt            build information; every file with size and SHA-256
# w2-062: NO config.json (created on the plant PC at the first start by TaidaFlowApp.exe
# --write-default-config, so an update never overwrites the plant's file) and no nginx.conf.
# No path of the build machine is written into any text file of the package (Mango A9).
# Not included: CMake/Ninja files, .lib/.exp/.pdb, tests, databases, ini files, exports, logs,
# config.json, Python files, __pycache__ (checked at the end; any hit fails the packaging).
#
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File scripts\package-release.ps1
#            [-NginxDir <nginx folder>] [-WinSW <WinSW-x64-<version>.exe>] [-OutDir dist] [-BuildDir build\desktop]
#            [-WebSource build\wasm-release] [-Force] [-AllowStale]
#   -Force      : replace an existing package folder of the same name (only below -OutDir)
#   -AllowStale : package even when ninja reports that build\desktop or build\wasm-release is not
#                 up to date (not recommended: the folder name carries the current git hash)
# Exit codes: 0 packaged; 2 missing input (build, web, CRT, windeployqt, nginx, WinSW); 3 build not up to
# date; 4 package folder exists (use -Force); 5 windeployqt / deploy-web failed; 6 forbidden file
# found in the package.
param(
    [string]$NginxDir = "",
    [string]$WinSW = "",
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
               'deploy\nginx\taidaflow.conf',
               'scripts\install-nginx-config.ps1', 'scripts\taidaflow-config.ps1', 'scripts\deploy-web.ps1', 'docs\DEPLOY_AND_STARTUP.md',
               'scripts\install-nginx-service.ps1', 'scripts\uninstall-nginx-service.ps1',
               'scripts\add-startup-shortcut.ps1', 'scripts\remove-startup-shortcut.ps1') {
    if (-not (Test-Path (Join-Path $root $f) -PathType Leaf)) { Say "missing: $f"; exit 2 }
}
# WinSW (w2-076): -WinSW, else the newest C:\tools\winsw\WinSW-x64-<version>.exe; LICENSE.txt beside it.
$winswFrom = '-WinSW'
if ($WinSW -eq "") {
    $winswFrom = 'newest C:\tools\winsw\WinSW-x64-<version>.exe'
    $bestW = $null; $bestWVer = $null
    foreach ($f in @(Get-ChildItem 'C:\tools\winsw' -File -Filter 'WinSW-x64-*.exe' -ErrorAction SilentlyContinue)) {
        $wv = $null
        if ([version]::TryParse($f.BaseName.Substring(10), [ref]$wv) -and ($null -eq $bestWVer -or $wv -gt $bestWVer)) { $bestW = $f.FullName; $bestWVer = $wv }
    }
    $WinSW = $bestW
}
if (-not $WinSW -or -not (Test-Path -LiteralPath $WinSW -PathType Leaf)) {
    Say "WinSW not found (-WinSW '$WinSW'): download https://github.com/winsw/winsw/releases/download/v2.12.0/WinSW-x64.exe to C:\tools\winsw\WinSW-x64-2.12.0.exe with LICENSE.txt (docs\BUILD.md, WinSW)"
    exit 2
}
$WinSW = [System.IO.Path]::GetFullPath($WinSW)
$winswLicense = Join-Path (Split-Path -Parent $WinSW) 'LICENSE.txt'
if (-not (Test-Path -LiteralPath $winswLicense -PathType Leaf)) { Say "WinSW licence not found: $winswLicense (LICENSE.txt of the same release, beside the exe)"; exit 2 }
$winswInfo = (Get-Item -LiteralPath $WinSW).VersionInfo
$winswVer = (("$($winswInfo.ProductVersion)" -split '\+')[0]).Trim()
if ($winswInfo.ProductName -ne 'Windows Service Wrapper' -or -not $winswVer) { Say "$WinSW is not WinSW (product '$($winswInfo.ProductName)', version '$($winswInfo.ProductVersion)')"; exit 2 }
Say "WinSW to bundle: $WinSW ($winswVer, from $winswFrom)"
# nginx for Windows to bundle (Mango A7). w2-065: first the nginx.exe of the development config.json.
function Test-NginxDir([string]$d) { return ($d -and (Test-Path (Join-Path $d 'nginx.exe')) -and (Test-Path (Join-Path $d 'docs\LICENSE')) -and (Test-Path (Join-Path $d 'conf\mime.types'))) }
$nginxFrom = '-NginxDir'
if ($NginxDir -eq "") {
    . (Join-Path $PSScriptRoot 'taidaflow-config.ps1')
    $devConfig = Resolve-TaidaFlowConfigPath $root ''
    if (Test-Path -LiteralPath $devConfig -PathType Leaf) {
        $dc = Get-TaidaFlowConfig -Path $devConfig -Exe $exe
        if (-not $dc.Error) {
            $cand = Split-Path -Parent (Resolve-TaidaFlowNginxExe $dc)
            if (Test-NginxDir $cand) { $NginxDir = $cand; $nginxFrom = "nginx.exe of $devConfig" }
            else { Say "note: nginx.exe of $devConfig ($(Resolve-TaidaFlowNginxExe $dc)) is not a complete nginx folder (nginx.exe, docs\LICENSE, conf\mime.types)" }
        } else { Say "note: $devConfig unusable: $($dc.Error)" }
    }
}
if ($NginxDir -eq "") {
    $nginxFrom = 'newest C:\tools\nginx\nginx-<version>'
    $best = $null; $bestVer = $null
    foreach ($d in @(Get-ChildItem 'C:\tools\nginx' -Directory -Filter 'nginx-*' -ErrorAction SilentlyContinue)) {
        $v = $null
        if ([version]::TryParse($d.Name.Substring(6), [ref]$v) -and (Test-Path (Join-Path $d.FullName 'nginx.exe'))) {
            if ($null -eq $bestVer -or $v -gt $bestVer) { $best = $d.FullName; $bestVer = $v }
        }
    }
    $NginxDir = $best
}
if (-not $NginxDir -or -not (Test-Path (Join-Path $NginxDir 'nginx.exe')) -or -not (Test-Path (Join-Path $NginxDir 'docs\LICENSE')) -or
    -not (Test-Path (Join-Path $NginxDir 'conf\mime.types'))) {
    Say "nginx.exe / docs\LICENSE / conf\mime.types not found (-NginxDir '$NginxDir')"; exit 2
}
Say "nginx to bundle: $NginxDir (from $nginxFrom)"
$NginxDir = Full $NginxDir

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
# -NoConfig: runtime.json from the app's default values (the package must not carry the development
# configuration; the app rewrites runtime.json from the plant's config.json at every start).
$webOut = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File (Join-Path $root 'scripts\deploy-web.ps1') -Source $WebSource -ExeDir $pkg -NoConfig
$webRc = $LASTEXITCODE
$webOut | ForEach-Object { Say "  web | $_" }
if ($webRc -ne 0) { Say "deploy-web.ps1 exit $webRc"; exit 5 }

# --- 4. field scripts, nginx template, documentation --------------------------------------------
foreach ($f in 'start-taidaflow.ps1', 'stop-taidaflow.ps1', 'start-taidaflow.bat', 'stop-taidaflow.bat',
               'register-autostart.ps1', 'unregister-autostart.ps1') {
    Copy-Item -LiteralPath (Join-Path $root "deploy\release\$f") -Destination $pkg
}
New-Item -ItemType Directory -Force (Join-Path $pkg 'scripts'), (Join-Path $pkg 'deploy\nginx') | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'scripts\install-nginx-config.ps1') -Destination (Join-Path $pkg 'scripts')
Copy-Item -LiteralPath (Join-Path $root 'scripts\taidaflow-config.ps1') -Destination (Join-Path $pkg 'scripts')
foreach ($f in 'install-nginx-service.ps1', 'uninstall-nginx-service.ps1', 'add-startup-shortcut.ps1', 'remove-startup-shortcut.ps1') {
    Copy-Item -LiteralPath (Join-Path $root "scripts\$f") -Destination (Join-Path $pkg 'scripts')
}
Copy-Item -LiteralPath (Join-Path $root 'deploy\nginx\taidaflow.conf') -Destination (Join-Path $pkg 'deploy\nginx')
Copy-Item -LiteralPath (Join-Path $root 'docs\DEPLOY_AND_STARTUP.md') -Destination (Join-Path $pkg 'DEPLOY.md')

# --- 5. nginx for Windows (Mango A7): nginx.exe, docs\, conf\ without nginx.conf ------------------
$nDir = Join-Path $pkg 'nginx'
New-Item -ItemType Directory -Force $nDir, (Join-Path $nDir 'conf') | Out-Null
Copy-Item -LiteralPath (Join-Path $NginxDir 'nginx.exe') -Destination $nDir
Copy-Item -LiteralPath (Join-Path $NginxDir 'docs') -Destination $nDir -Recurse
foreach ($c in @(Get-ChildItem -LiteralPath (Join-Path $NginxDir 'conf') -File | Where-Object { $_.Name -ne 'nginx.conf' })) {
    Copy-Item -LiteralPath $c.FullName -Destination (Join-Path $nDir 'conf')
}
$nginxVer = Split-Path -Leaf $NginxDir
$nginxHash = (Get-FileHash -Algorithm SHA256 (Join-Path $nDir 'nginx.exe')).Hash
[System.IO.File]::WriteAllLines((Join-Path $nDir 'SOURCE.txt'), [string[]]@(
    "nginx for Windows, $nginxVer, copied unchanged from the official nginx.org zip (signature checked when",
    "it was installed on the build PC - see the TaidaFlow README, section nginx).",
    "nginx.exe SHA-256: $nginxHash",
    "Licence: docs\LICENSE (2-clause BSD); bundled libraries: docs\OpenSSL.LICENSE, docs\PCRE.LICENCE, docs\zlib.LICENSE.",
    "conf\ holds the zip's files WITHOUT nginx.conf: scripts\install-nginx-config.ps1 writes conf\nginx.conf",
    "for this installation (start-taidaflow.ps1 does it too when it is missing or out of date).",
    "Start: cd <installation folder>\nginx  then  start nginx ; apply changes: nginx -s reload ; stop: nginx -s quit.",
    "logs\ (nginx.pid, nginx's own start-up messages) and temp\ are created by install-nginx-config.ps1;",
    "the request / error logs are written to config.json log.dir (nginx-access-YYYY-MM-DD.log, nginx-error.log)."))
Say "nginx: $nginxVer bundled (nginx.exe + docs\ + conf\ without nginx.conf)"

# --- 5b. WinSW = nginx\nginx-service.exe (w2-076) ------------------------------------------------------------
# Copied unchanged under the name the service uses (WinSW reads <its own name>.xml = nginx-service.xml, written
# on the plant PC by scripts\install-nginx-service.ps1 - never shipped).
Copy-Item -LiteralPath $WinSW -Destination (Join-Path $nDir 'nginx-service.exe')
Copy-Item -LiteralPath $winswLicense -Destination (Join-Path $nDir 'LICENSE-WinSW.txt')
$winswHash = (Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $nDir 'nginx-service.exe')).Hash
$winswSig = (Get-AuthenticodeSignature -LiteralPath (Join-Path $nDir 'nginx-service.exe')).Status
[System.IO.File]::WriteAllLines((Join-Path $nDir 'SOURCE-WinSW.txt'), [string[]]@(
    "nginx-service.exe = WinSW (Windows Service Wrapper) $winswVer, WinSW-x64.exe of the official GitHub release, unchanged:",
    "  https://github.com/winsw/winsw/releases/download/v$winswVer/WinSW-x64.exe   (renamed: WinSW reads <its name>.xml)",
    "  file version $($winswInfo.FileVersion), product version $($winswInfo.ProductVersion)",
    "  SHA-256 $winswHash ; Authenticode: $winswSig (the project does not sign this release)",
    "  self-contained .NET 6 single-file program (no .NET installation needed), 64-bit Windows 10 / 11.",
    "Licence: MIT, LICENSE-WinSW.txt (copyright Kohsuke Kawaguchi, Sun Microsystems, CloudBees, Oleg Nenashev and contributors).",
    "Used by scripts\install-nginx-service.ps1 (administrator): writes nginx-service.xml here (service TaidaFlowNginx:",
    "nginx.exe -p <this folder>, stop: nginx -s quit, start mode Automatic) and runs  nginx-service.exe install / start.",
    "Commands (administrator cmd, in this folder): nginx-service.exe status | start | stop | restart | uninstall.",
    "Removal: scripts\uninstall-nginx-service.ps1. Details: DEPLOY.md section 2A (nginx service + Startup folder)."))
Say "WinSW: $winswVer bundled as nginx\nginx-service.exe (SHA-256 $winswHash, Authenticode $winswSig) + LICENSE-WinSW.txt + SOURCE-WinSW.txt"

# --- 6. forbidden content ------------------------------------------------------------------------
$all = @(Get-ChildItem -LiteralPath $pkg -Recurse -Force)
$forbidden = @($all | Where-Object {
    $n = $_.Name.ToLowerInvariant()
    ($_.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -or
    ($_.PSIsContainer -and ($n -in '__pycache__', 'cmakefiles', 'exports', 'data', 'runtime', 'logs', 'logging', 'temp', 'tests', '.qt', '.rcc')) -or
    (-not $_.PSIsContainer -and ($n -match '\.(sqlite|sqlite3|db|py|pyc|pdb|lib|exp|ilk|obj|ninja|cmake|log|csv)$' -or
                                 $n -in 'cmakecache.txt', 'taidaflowsettings.ini', 'device_info.ini',
                                        'config.json', 'config.effective.json', 'taidaflow-app.json', 'nginx.conf', 'nginx-service.xml'))
})
if ($forbidden.Count) {
    Say "FORBIDDEN content in the package:"
    $forbidden | ForEach-Object { Say "  $($_.FullName)" }
    exit 6
}
# No path of the build machine (Mango A9): the repository folder and the user profile, in any text
# file (no NUL byte) and in TaidaFlowApp.exe (ASCII / UTF-16). Qt's own DLLs are not ours and not checked.
$machinePaths = @($root, (Split-Path -Parent $root), $env:USERPROFILE) | Where-Object { $_ } | ForEach-Object { @($_, ($_ -replace '\\', '/')) }
$latin1 = [System.Text.Encoding]::GetEncoding(28591)
$hits = New-Object System.Collections.Generic.List[string]
foreach ($f in @(Get-ChildItem -LiteralPath $pkg -Recurse -File)) {
    $isExe = $f.Name -eq 'TaidaFlowApp.exe'
    if (-not $isExe -and ($f.Length -gt 20MB -or $f.Extension -match '^\.(exe|dll|wasm|gz|png|jpg|ico|ttf|otf|woff2?|qmlc)$')) { continue }
    $bytes = [System.IO.File]::ReadAllBytes($f.FullName)
    if (-not $isExe -and [Array]::IndexOf($bytes, [byte]0) -ge 0) { continue }
    $text = $latin1.GetString($bytes)
    foreach ($m in $machinePaths) {
        $u16 = $latin1.GetString([System.Text.Encoding]::Unicode.GetBytes($m))
        if ($text.IndexOf($m, [System.StringComparison]::OrdinalIgnoreCase) -ge 0 -or ($isExe -and $text.IndexOf($u16, [System.StringComparison]::OrdinalIgnoreCase) -ge 0)) {
            $hits.Add("$($f.FullName.Substring($pkg.Length + 1)): $m")
        }
    }
}
if ($hits.Count) {
    Say "BUILD-MACHINE PATH in the package ($($hits.Count)):"
    $hits | ForEach-Object { Say "  $_" }
    exit 6
}
Say "no build-machine path (repository, user profile) in the package's text files or TaidaFlowApp.exe"

# --- 7. VERSION.txt / MANIFEST.txt -----------------------------------------------------------------
$files = @(Get-ChildItem -LiteralPath $pkg -Recurse -File | Sort-Object FullName)
$gitDate = (& git -C $root log -1 --format=%cI).Trim()
[System.IO.File]::WriteAllLines((Join-Path $pkg 'VERSION.txt'), [string[]]@(
    "TaidaFlow field package $name",
    "created      : $((Get-Date).ToString('o')) on $env:COMPUTERNAME",
    "git          : $(& git -C $root rev-parse HEAD) ($gitDate)$(if ($dirty) { ' + UNCOMMITTED source changes' })",
    "Qt           : 6.8.3 msvc2022_64 (windeployqt, Release)",
    "exe          : TaidaFlowApp.exe built $((Get-Item $exe).LastWriteTime.ToString('s')) (desktop-release)",
    "MSVC runtime : app-local DLLs, $(Split-Path -Leaf $crtDir) $(Split-Path -Leaf (Split-Path -Parent (Split-Path -Parent $crtDir)))",
    "web page     : WebAssembly build of $((Get-Item (Join-Path $WebSource 'TaidaFlowApp.wasm')).LastWriteTime.ToString('s')) (wasm-release)",
    "nginx        : $nginxVer bundled in nginx\ (start: cd nginx + start nginx); conf\nginx.conf written on the plant PC by scripts\install-nginx-config.ps1",
    "nginx service: WinSW $winswVer as nginx\nginx-service.exe (SHA-256 $winswHash, MIT, LICENSE-WinSW.txt); optional, administrator: scripts\install-nginx-service.ps1 (service TaidaFlowNginx)",
    "log-on start : scripts\add-startup-shortcut.ps1 (Startup folder) or register-autostart.ps1 (Task Scheduler) - one of the two",
    "config.json  : not included - created at the first start with the defaults (TaidaFlowApp.exe --write-default-config); never overwritten by an update",
    "start        : start-taidaflow.bat (double-click) or start-taidaflow.ps1 - see DEPLOY.md",
    "ports        : config.json; plant firewall inbound only 80 (nginx: page, /mirror, /exports, /api/) and 502 (Modbus server)"))
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
