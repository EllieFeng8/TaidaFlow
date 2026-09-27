# Start the nginx web front end on 0.0.0.0:8123 (w2-050) = scripts\nginx-web.ps1 -Action start.
# Deploy the page first:  scripts\deploy-web.ps1   (build\wasm-release -> <exe folder>\web + .gz)
# Usage: powershell -ExecutionPolicy Bypass -File scripts\nginx-start.ps1
#            [-WebRoot <folder>] [-ExeDir <folder>] [-ExportDir <folder>] [-RuntimeDir <folder>]
#            [-Nginx <nginx.exe or folder>] [-Test]   (-Test: only render the configuration, run nginx -t)
# -ExportDir: the app's export folder (default build\runtime-cwd\exports); nginx serves
#   /exports/<file> from it. Start the app with TAIDAFLOW_DOWNLOAD_PORT=8123 so that the download
#   links point to nginx (without it the links keep using the app's own port 8124).
# Exit codes: see scripts\nginx-web.ps1 (0 started, 2 missing, 3 nginx -t failed, 4 port 8123 busy,
# 5 link/junction in the web root or export folder, 6 not listening in time).
param(
    [string]$WebRoot = "",
    [string]$ExeDir = "",
    [string]$ExportDir = "",
    [string]$RuntimeDir = "",
    [string]$Nginx = "",
    [int]$TimeoutSec = 20,
    [switch]$Test
)
$forward = @{} + $PSBoundParameters
$forward.Remove('Test') | Out-Null
$action = if ($Test) { 'test' } else { 'start' }
& (Join-Path $PSScriptRoot 'nginx-web.ps1') -Action $action @forward
exit $LASTEXITCODE
