# Deploy the WebAssembly build next to the desktop exe (w2-049).
#
# The desktop app serves the web page itself (Core/AppHttpServer singleton, 0.0.0.0:8124):
#   http://<host>:8124/TaidaFlowApp.html
# Web page folder lookup of the app: TAIDAFLOW_WEB_DIR -> <exe folder>\web -> the repository's
# build\wasm-release (development default). This script fills <exe folder>\web:
#   1. copies the web files of one build (TaidaFlowApp.html/.js/.wasm, qtloader.js, qtlogo.svg and
#      any other top-level *.html *.js *.mjs *.wasm *.css *.json *.svg *.png *.ico) - never the
#      CMake/Ninja files of the build folder;
#   2. writes <file>.gz (gzip, optimal) for .wasm .js .mjs .html .css .json .svg; the server sends
#      it to browsers that accept gzip (Content-Encoding: gzip), the .gz gets the same modification
#      time as its file (a .gz older than its file is never served);
#   3. the old content of <exe folder>\web is removed first, so HTML/JS/WASM of different builds
#      never mix.
# Usage: powershell -ExecutionPolicy Bypass -File scripts\deploy-web.ps1
#            [-Source build\wasm-release] [-ExeDir build\desktop] [-NoGzip]
# Exit 0 = deployed; 2 = source incomplete / exe folder missing.
param(
    [string]$Source = "",
    [string]$ExeDir = "",
    [switch]$NoGzip
)
$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if ($Source -eq "") { $Source = Join-Path $root 'build\wasm-release' }
if ($ExeDir -eq "") { $ExeDir = Join-Path $root 'build\desktop' }
if (-not [System.IO.Path]::IsPathRooted($Source)) { $Source = Join-Path $root $Source }
if (-not [System.IO.Path]::IsPathRooted($ExeDir)) { $ExeDir = Join-Path $root $ExeDir }

foreach ($required in 'TaidaFlowApp.html', 'TaidaFlowApp.js', 'TaidaFlowApp.wasm', 'qtloader.js') {
    if (-not (Test-Path (Join-Path $Source $required) -PathType Leaf)) {
        Write-Output "missing $(Join-Path $Source $required) - build first: scripts\build-wasm.bat"
        exit 2
    }
}
if (-not (Test-Path $ExeDir -PathType Container)) {
    Write-Output "exe folder not found: $ExeDir (build first: scripts\build-desktop.bat)"
    exit 2
}

$web = Join-Path $ExeDir 'web'
if (Test-Path $web) { Remove-Item -LiteralPath $web -Recurse -Force }
New-Item -ItemType Directory -Force $web | Out-Null

$copyTypes = '.html', '.js', '.mjs', '.wasm', '.css', '.json', '.svg', '.png', '.ico'
$gzipTypes = '.html', '.js', '.mjs', '.wasm', '.css', '.json', '.svg'
$files = @(Get-ChildItem -LiteralPath $Source -File | Where-Object { $copyTypes -contains $_.Extension.ToLowerInvariant() })
$total = 0; $totalGz = 0
foreach ($f in $files) {
    $dst = Join-Path $web $f.Name
    Copy-Item -LiteralPath $f.FullName -Destination $dst
    (Get-Item -LiteralPath $dst).LastWriteTimeUtc = $f.LastWriteTimeUtc
    $line = "{0,-24} {1,12} bytes" -f $f.Name, $f.Length
    $total += $f.Length
    if (-not $NoGzip -and ($gzipTypes -contains $f.Extension.ToLowerInvariant())) {
        $gzPath = "$dst.gz"
        $in = [System.IO.File]::OpenRead($dst)
        try {
            $out = [System.IO.File]::Create($gzPath)
            try {
                $gz = New-Object System.IO.Compression.GZipStream($out, [System.IO.Compression.CompressionLevel]::Optimal)
                try { $in.CopyTo($gz) } finally { $gz.Dispose() }
            } finally { $out.Dispose() }
        } finally { $in.Dispose() }
        (Get-Item -LiteralPath $gzPath).LastWriteTimeUtc = $f.LastWriteTimeUtc
        $gzLen = (Get-Item -LiteralPath $gzPath).Length
        $totalGz += $gzLen
        $line += "  -> .gz {0,12} bytes ({1:P0})" -f $gzLen, ($gzLen / [double][Math]::Max(1, $f.Length))
    }
    Write-Output $line
}
Write-Output ("deployed {0} file(s), {1} bytes (+{2} bytes .gz) from {3} to {4}" -f $files.Count, $total, $totalGz, $Source, $web)
Write-Output "the desktop app serves them at http://<host>:8124/TaidaFlowApp.html (restart it if it is running)"
exit 0
